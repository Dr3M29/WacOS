"""
Air-ring prototype: webcam hand gestures (no mouse control).

Stand-in for the IMU ring until the XIAO arrives. Same safety idea as the ring:
nothing fires from ordinary hand movement.

  * HOLD gestures (thumbs up, peace, ...) are deliberate, so they are always on.
    Hold the pose ~0.5 s and it fires once; change pose to fire again.
  * SWIPES are where accidents happen, so they need arming first: hold an open
    palm still for ~0.4 s (the ring turns cyan), then swipe within 3 s.
  * PINCH TAP: pinch thumb + index and release quickly.

Dry run by default: events only show on screen / in the console.
Pass --live to actually send the key bindings in ACTIONS to the computer.

    python gestures.py            # dry run, camera 0
    python gestures.py --camera 1 # another webcam
    python gestures.py --live     # send keys
"""

import argparse
import math
import time
import urllib.request
from collections import deque
from pathlib import Path

import cv2
import mediapipe as mp
from mediapipe.tasks.python import BaseOptions, vision

MODEL_URL = ("https://storage.googleapis.com/mediapipe-models/hand_landmarker/"
             "hand_landmarker/float16/latest/hand_landmarker.task")
MODEL_PATH = Path(__file__).parent / "models" / "hand_landmarker.task"

# What each event does in --live mode. Keys are pynput names (Key.<name>) or
# single characters; a list is a combo pressed together.
ACTIONS = {
    "swipe_right": ["media_next"],
    "swipe_left":  ["media_previous"],
    "swipe_up":    ["media_volume_up"],
    "swipe_down":  ["media_volume_down"],
    "thumbs_up":   ["media_play_pause"],
    "peace":       None,          # unbound — shows on screen only
    "point":       None,
    "fist":        None,
    "pinch_tap":   None,
}

# Tuning
HOLD_FRAMES = 12          # frames a pose must be stable before it fires (~0.4 s @ 30 fps)
ARM_STILL_FRAMES = 12     # frames of still open palm to arm swipes
ARM_WINDOW_S = 3.0        # how long swipes stay armed
STILL_TOL = 0.012         # palm movement per frame (normalized) that still counts as "still"
SWIPE_WINDOW_S = 0.30     # swipe must happen within this time span
SWIPE_DIST = 0.22         # min travel, fraction of frame
PINCH_ON = 0.30           # thumb-index distance / palm size to count as pinched
PINCH_OFF = 0.45          # hysteresis for release
PINCH_TAP_MAX_S = 0.45    # pinch shorter than this = tap
COOLDOWN_S = 0.6          # after any event

CYAN, MAGENTA, WHITE, GREY = (255, 230, 0), (200, 40, 255), (255, 255, 255), (110, 110, 110)

# Landmark indices
WRIST, THUMB_IP, THUMB_TIP = 0, 3, 4
INDEX_MCP, INDEX_PIP, INDEX_TIP = 5, 6, 8
MIDDLE_MCP, MIDDLE_PIP, MIDDLE_TIP = 9, 10, 12
RING_PIP, RING_TIP = 14, 16
PINKY_MCP, PINKY_PIP, PINKY_TIP = 17, 18, 20


def ensure_model():
    if not MODEL_PATH.exists():
        MODEL_PATH.parent.mkdir(exist_ok=True)
        print(f"Downloading hand model (~8 MB) to {MODEL_PATH} ...")
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
    return str(MODEL_PATH)


def dist(a, b):
    return math.hypot(a.x - b.x, a.y - b.y)


def palm_size(lm):
    return dist(lm[WRIST], lm[MIDDLE_MCP]) or 1e-6


def finger_states(lm):
    """[thumb, index, middle, ring, pinky] extended? Distance-based so it works at any hand angle."""
    w = lm[WRIST]
    fingers = [dist(lm[tip], w) > dist(lm[pip], w) * 1.1
               for pip, tip in ((INDEX_PIP, INDEX_TIP), (MIDDLE_PIP, MIDDLE_TIP),
                                (RING_PIP, RING_TIP), (PINKY_PIP, PINKY_TIP))]
    # Thumb: extended when its tip is well away from the side of the palm.
    thumb = dist(lm[THUMB_TIP], lm[PINKY_MCP]) > dist(lm[THUMB_IP], lm[PINKY_MCP]) * 1.1 and \
        dist(lm[THUMB_TIP], lm[INDEX_MCP]) > palm_size(lm) * 0.5
    return [thumb] + fingers


def classify(lm):
    thumb, index, middle, ring, pinky = finger_states(lm)
    if dist(lm[THUMB_TIP], lm[INDEX_TIP]) / palm_size(lm) < PINCH_ON:
        return "pinch"
    if index and middle and ring and pinky:
        return "open"
    if not (index or middle or ring or pinky):
        # y grows downward: thumb tip clearly above the wrist = thumbs up
        if thumb and lm[THUMB_TIP].y < lm[WRIST].y - palm_size(lm) * 0.5:
            return "thumbs_up"
        return "fist"
    if index and middle and not ring and not pinky:
        return "peace"
    if index and not (middle or ring or pinky):
        return "point"
    return None


def make_sender(live):
    if not live:
        return lambda name: None
    from pynput.keyboard import Controller, Key
    kb = Controller()

    def send(name):
        combo = ACTIONS.get(name)
        if not combo:
            return
        keys = [getattr(Key, k) if len(k) > 1 else k for k in combo]
        for k in keys:
            kb.press(k)
        for k in reversed(keys):
            kb.release(k)
    return send


class GestureEngine:
    def __init__(self, on_event):
        self.on_event = on_event
        self.pose, self.pose_frames, self.fired_pose = None, 0, False
        self.still_frames = 0
        self.armed_until = 0.0
        self.track = deque()            # (t, x, y) palm centre
        self.pinch_down_at = None
        self.cooldown_until = 0.0

    def armed(self, now):
        return now < self.armed_until

    def emit(self, name, now):
        self.cooldown_until = now + COOLDOWN_S
        self.on_event(name)

    def update(self, lm, now):
        if lm is None:
            self.pose, self.pose_frames, self.still_frames = None, 0, 0
            self.track.clear()
            self.pinch_down_at = None
            return None

        pose = classify(lm)
        cx = (lm[WRIST].x + lm[MIDDLE_MCP].x) / 2
        cy = (lm[WRIST].y + lm[MIDDLE_MCP].y) / 2
        moved = math.hypot(cx - self.track[-1][1], cy - self.track[-1][2]) if self.track else 0
        self.track.append((now, cx, cy))
        while self.track and now - self.track[0][0] > SWIPE_WINDOW_S:
            self.track.popleft()

        # Pinch tap (hysteresis so jitter doesn't double-fire)
        ratio = dist(lm[THUMB_TIP], lm[INDEX_TIP]) / palm_size(lm)
        if self.pinch_down_at is None and ratio < PINCH_ON:
            self.pinch_down_at = now
        elif self.pinch_down_at is not None and ratio > PINCH_OFF:
            if now - self.pinch_down_at < PINCH_TAP_MAX_S and now >= self.cooldown_until:
                self.emit("pinch_tap", now)
            self.pinch_down_at = None

        # Arming: still open palm
        if pose == "open" and moved < STILL_TOL:
            self.still_frames += 1
            if self.still_frames >= ARM_STILL_FRAMES and not self.armed(now):
                self.armed_until = now + ARM_WINDOW_S
                self.track.clear()
                self.track.append((now, cx, cy))
        else:
            self.still_frames = 0

        # Swipes (only while armed)
        if self.armed(now) and now >= self.cooldown_until and len(self.track) > 2:
            dx = self.track[-1][1] - self.track[0][1]
            dy = self.track[-1][2] - self.track[0][2]
            if max(abs(dx), abs(dy)) > SWIPE_DIST:
                if abs(dx) > abs(dy):
                    name = "swipe_right" if dx > 0 else "swipe_left"
                else:
                    name = "swipe_down" if dy > 0 else "swipe_up"
                self.armed_until = 0
                self.track.clear()
                self.emit(name, now)

        # Hold poses (always on). "open" and "pinch" are handled above.
        if pose == self.pose:
            self.pose_frames += 1
        else:
            self.pose, self.pose_frames, self.fired_pose = pose, 1, False
        if (pose not in (None, "open", "pinch") and not self.fired_pose
                and self.pose_frames >= HOLD_FRAMES and now >= self.cooldown_until):
            self.fired_pose = True
            self.emit(pose, now)
        return pose


def draw(frame, lm, pose, engine, now, last_event, fps, live):
    h, w = frame.shape[:2]
    if lm is not None:
        pts = [(int(p.x * w), int(p.y * h)) for p in lm]
        for c in vision.HandLandmarksConnections.HAND_CONNECTIONS:
            cv2.line(frame, pts[c.start], pts[c.end], MAGENTA, 2, cv2.LINE_AA)
        for p in pts:
            cv2.circle(frame, p, 4, CYAN, -1, cv2.LINE_AA)
        # Arm ring around the palm: fills while you hold still, solid cyan when armed
        cx = int((lm[WRIST].x + lm[MIDDLE_MCP].x) / 2 * w)
        cy = int((lm[WRIST].y + lm[MIDDLE_MCP].y) / 2 * h)
        r = int(palm_size(lm) * w * 0.9)
        if engine.armed(now):
            cv2.circle(frame, (cx, cy), r, CYAN, 3, cv2.LINE_AA)
        elif engine.still_frames:
            sweep = int(360 * min(1, engine.still_frames / ARM_STILL_FRAMES))
            cv2.ellipse(frame, (cx, cy), (r, r), -90, 0, sweep, GREY, 2, cv2.LINE_AA)

    status = "ARMED" if engine.armed(now) else "idle"
    cv2.putText(frame, f"pose: {pose or '-'}   swipes: {status}", (12, 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, CYAN, 2, cv2.LINE_AA)
    cv2.putText(frame, f"{fps:4.1f} fps   {'LIVE - keys are sent' if live else 'dry run'}",
                (12, h - 14), cv2.FONT_HERSHEY_SIMPLEX, 0.55, MAGENTA if live else GREY, 1, cv2.LINE_AA)
    if last_event and now - last_event[1] < 1.2:
        text = last_event[0].upper().replace("_", " ")
        (tw, _), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_DUPLEX, 1.4, 2)
        cv2.putText(frame, text, ((w - tw) // 2, h // 2), cv2.FONT_HERSHEY_DUPLEX, 1.4, WHITE, 2, cv2.LINE_AA)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--camera", type=int, default=0, help="webcam index (default 0)")
    ap.add_argument("--live", action="store_true", help="send the key bindings in ACTIONS")
    args = ap.parse_args()

    cap = cv2.VideoCapture(args.camera, cv2.CAP_DSHOW)
    if not cap.isOpened():
        raise SystemExit(f"No webcam at index {args.camera}. Plug one in or try --camera 1.")

    landmarker = vision.HandLandmarker.create_from_options(vision.HandLandmarkerOptions(
        base_options=BaseOptions(model_asset_path=ensure_model()),
        running_mode=vision.RunningMode.VIDEO,
        num_hands=1,
        min_hand_detection_confidence=0.6,
        min_tracking_confidence=0.5,
    ))

    send = make_sender(args.live)
    last_event = None

    def on_event(name):
        nonlocal last_event
        last_event = (name, time.monotonic())
        bound = ACTIONS.get(name)
        print(f"[{time.strftime('%H:%M:%S')}] {name}" + (f"  -> {'+'.join(bound)}" if bound else ""))
        send(name)

    engine = GestureEngine(on_event)
    t0, prev, fps = time.monotonic(), time.monotonic(), 0.0
    print("Running. Press Q or Esc in the video window to quit.")

    while True:
        ok, frame = cap.read()
        if not ok:
            break
        frame = cv2.flip(frame, 1)  # mirror, so moving right looks like right
        now = time.monotonic()
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
        result = landmarker.detect_for_video(image, int((now - t0) * 1000))
        lm = result.hand_landmarks[0] if result.hand_landmarks else None

        pose = engine.update(lm, now)
        fps = 0.9 * fps + 0.1 / max(now - prev, 1e-6)
        prev = now
        draw(frame, lm, pose, engine, now, last_event, fps, args.live)

        cv2.imshow("air-ring", frame)
        if cv2.waitKey(1) & 0xFF in (ord("q"), 27):
            break

    cap.release()
    cv2.destroyAllWindows()
    landmarker.close()


if __name__ == "__main__":
    main()
