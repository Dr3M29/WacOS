"""
Air Ring training: teaches it YOUR gestures (about 4 minutes).

    python trainer.py               # full session
    python trainer.py --only swipe_left,side_tap   # record more of just these
    python trainer.py --retrain     # rebuild the model from saved recordings

Recordings go to data/clips.jsonl (kept between sessions, so you can add more
later); the model to data/model.npz. Air Ring picks it up automatically.
"""

import argparse
import json
import sys
import time

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions, vision

import air_ring as ar
import gesture_model as gm

WIN = "Air Ring - training"

STEPS = [
    # kind, label, instruction, reps, seconds
    ("pose", "open", "OPEN HAND, palm to the camera. Tilt it around a little.", 2, 2.5),
    ("pose", "peace", "PEACE SIGN. Tilt it around a little.", 2, 2.5),
    ("pose", "three", "THREE FINGERS (pointer, middle, ring).", 2, 2.5),
    ("pose", "rock", "ROCK SIGN (pointer + pinky).", 2, 2.5),
    ("pose", "thumbs_up", "THUMBS UP.", 2, 2.5),
    ("pose", "fist", "FIST.", 2, 2.5),
    ("pose", "point", "POINT with your pointer finger.", 2, 2.5),
    ("motion", "swipe_left", "Open hand: SWIPE LEFT once, the way you normally would.", 6, 2.0),
    ("motion", "swipe_right", "Open hand: SWIPE RIGHT once.", 6, 2.0),
    ("motion", "swipe_up", "Open hand: SWIPE UP once.", 6, 2.0),
    ("motion", "swipe_down", "Open hand: SWIPE DOWN once.", 6, 2.0),
    ("motion", "side_tap", "TAP your thumb on the side of your pointer finger once.", 6, 2.0),
    # Pinch clips tune the pinch-volume thresholds (not used by the kNN model).
    ("pinch", "pinch_hold", "PINCH thumb + pointer tips together and HOLD it still.", 3, 3.0),
    ("pinch", "pinch_drag", "PINCH and hold it, then move your hand slowly UP and DOWN.", 3, 4.0),
    ("none", "none", "Just move NORMALLY, no gestures: talk with your hands, reach for the "
                     "mouse, scratch your face, drop your arm and raise it, turn your hand.", 1, 30.0),
    ("snap", "snap", "SNAP your fingers 6 times, a second or so apart.", 1, 10.0),
]


class Quit(Exception):
    pass


class Session:
    def __init__(self):
        self.cfg = ar.load_config()
        idx, name = ar.pick_camera(self.cfg.get("camera", "auto"))
        if idx is None:
            raise SystemExit(f"Camera: {name}")
        self.cap = cv2.VideoCapture(idx, cv2.CAP_DSHOW)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        self.cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        self.cap.set(cv2.CAP_PROP_FPS, 60)
        if self.cfg.get("exposure") is not None:
            self.cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.25)
            self.cap.set(cv2.CAP_PROP_EXPOSURE, self.cfg["exposure"])
        self.lmk = vision.HandLandmarker.create_from_options(vision.HandLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=ar.ensure_model()),
            running_mode=vision.RunningMode.VIDEO, num_hands=1,
            min_hand_detection_confidence=0.5, min_hand_presence_confidence=0.4,
            min_tracking_confidence=0.4))
        self.t0 = time.monotonic()
        ar.SOUND_FILES.update(ar.make_sounds())

    def frame(self, lines, big=None, colour=ar.WHITE, bar=None):
        ok, img = self.cap.read()
        if not ok:
            raise SystemExit("camera read failed")
        img = cv2.flip(img, 1)
        now = time.monotonic()
        res = self.lmk.detect_for_video(mp.Image(image_format=mp.ImageFormat.SRGB,
                                                 data=cv2.cvtColor(img, cv2.COLOR_BGR2RGB)),
                                        int((now - self.t0) * 1000))
        lm = res.hand_landmarks[0] if res.hand_landmarks else None
        wl = res.hand_world_landmarks[0] if res.hand_world_landmarks else None
        h, w = img.shape[:2]
        if lm is not None:
            pts = [(int(p.x * w), int(p.y * h)) for p in lm]
            for c in vision.HandLandmarksConnections.HAND_CONNECTIONS:
                cv2.line(img, pts[c.start], pts[c.end], ar.MAGENTA, 2, cv2.LINE_AA)
        img = cv2.copyMakeBorder(img, 0, 120, 0, 0, cv2.BORDER_CONSTANT, value=(20, 20, 20))
        for i, line in enumerate(lines):
            cv2.putText(img, line, (12, h + 28 + i * 24), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                        ar.CYAN if i == 0 else ar.WHITE, 1, cv2.LINE_AA)
        if big:
            (tw, th), _ = cv2.getTextSize(big, cv2.FONT_HERSHEY_DUPLEX, 2.0, 3)
            cv2.putText(img, big, ((w - tw) // 2, h // 2), cv2.FONT_HERSHEY_DUPLEX, 2.0, (0, 0, 0), 8, cv2.LINE_AA)
            cv2.putText(img, big, ((w - tw) // 2, h // 2), cv2.FONT_HERSHEY_DUPLEX, 2.0, colour, 3, cv2.LINE_AA)
        if bar is not None:
            cv2.rectangle(img, (0, h - 8), (int(w * bar), h), ar.CYAN, -1)
        if lm is None:
            cv2.putText(img, "no hand", (w - 110, 28), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (60, 60, 230), 2)
        cv2.imshow(WIN, img)
        k = cv2.waitKey(1) & 0xFF
        if k == 27 or cv2.getWindowProperty(WIN, cv2.WND_PROP_VISIBLE) < 1:
            raise Quit
        return now, lm, wl, k

    def wait_key(self, lines, keys=(32,)):
        while True:
            _, _, _, k = self.frame(lines)
            if k in keys:
                return k

    def countdown(self, lines, secs):
        """Returns True if R (redo) was pressed."""
        end = time.monotonic() + secs
        redo = False
        while (left := end - time.monotonic()) > 0:
            _, _, _, k = self.frame(lines, big=str(int(left) + 1) if secs > 1 else "ready", colour=ar.GREY)
            redo |= k in (ord("r"), ord("R"))
        return redo

    def record(self, lines, secs):
        ar.play("arm")
        frames = []
        start = time.monotonic()
        while (el := time.monotonic() - start) < secs:
            now, lm, wl, _ = self.frame(lines, big="GO" if el < 0.5 else None, colour=ar.CYAN, bar=el / secs)
            if lm is not None and wl is not None:
                frames.append((now - start, gm.lm_array(lm), gm.lm_array(wl)))
        ar.play("action")
        return frames


def save_clip(kind, label, frames):
    gm.DATA.mkdir(exist_ok=True)
    with open(gm.CLIPS, "a", encoding="utf-8") as f:
        f.write(json.dumps({"kind": kind, "label": label, "time": time.time(), "frames": [
            [round(t, 4), np.round(lm, 4).ravel().tolist(), np.round(wl, 5).ravel().tolist()]
            for t, lm, wl in frames]}) + "\n")


def drop_last_clip():
    lines = gm.CLIPS.read_text(encoding="utf-8").splitlines(keepends=True)
    gm.CLIPS.write_text("".join(lines[:-1]), encoding="utf-8")


def snap_step(s, instr, secs):
    cfg = ar.load_config()
    cfg["tuning"] = dict(cfg["tuning"], snap_level=0.002, snap_ratio=4.0)
    peaks = []
    det = ar.SnapDetector(cfg, lambda t: peaks.append(det.last_peak))
    det.start()
    try:
        start = time.monotonic()
        while (el := time.monotonic() - start) < secs:
            s.frame([instr, f"snaps heard: {len(peaks)}"], big=str(len(peaks)), colour=ar.CYAN, bar=el / secs)
    finally:
        det.stop()
    if len(peaks) >= 3:
        level = max(0.003, float(np.median(peaks)) * 0.4)
        cal = gm.load_calibration()
        cal["snap_level"] = round(level, 4)
        gm.CALIB.write_text(json.dumps(cal, indent=2))
        return f"snaps: heard {len(peaks)}, loudness ~{np.median(peaks):.3f} -> trigger set to {level:.4f}"
    return f"snaps: only heard {len(peaks)} - kept the old trigger level (move closer to the mic?)"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma-separated labels to record (e.g. swipe_left,side_tap,none)")
    ap.add_argument("--retrain", action="store_true", help="just rebuild the model from saved clips")
    args = ap.parse_args()

    if args.retrain:
        model = gm.build()
        print("\n".join(model.report()) if model else "no recordings yet")
        return

    steps = STEPS
    if args.only:
        want = set(args.only.split(","))
        steps = [s for s in STEPS if s[1] in want]

    s = Session()
    notes = []
    try:
        n_old = len(gm.load_clips())
        k = s.wait_key(["Air Ring training - follow the prompts. Sit how you normally sit.",
                        f"{n_old} recordings already saved." if n_old else "About 4 minutes.",
                        "SPACE = start" + ("   F = delete old recordings and start fresh" if n_old else ""),
                        "ESC = quit (what's recorded so far is kept)"], keys=(32, ord("f"), ord("F")))
        if k in (ord("f"), ord("F")) and gm.CLIPS.exists():
            gm.CLIPS.unlink()

        for si, (kind, label, instr, reps, secs) in enumerate(steps):
            head = f"Step {si + 1}/{len(steps)}: {instr}"
            if kind == "snap":
                s.wait_key([head, "SPACE when ready"])
                notes.append(snap_step(s, head, secs))
                continue
            if kind == "none":
                s.wait_key([head, "This is what it learns to IGNORE.", "SPACE when ready"])
                save_clip(kind, label, s.record([head, "keep moving normally..."], secs))
                continue
            s.countdown([head, "get ready"], 3)
            r = 0
            while r < reps:
                fr = s.record([head, f"{r + 1} of {reps}"], secs)
                if len(fr) < secs * 8:
                    s.countdown([head, "lost your hand - again"], 1.2)
                    continue
                save_clip(kind, label, fr)
                r += 1
                if s.countdown([head, f"{r} of {reps} done   (R = redo the last one)"], 1.2):
                    drop_last_clip()
                    r -= 1

        s.frame(["Training..."])
        model = gm.build()
        lines = (model.report() if model else ["no recordings"]) + notes
        print("\n".join(lines))
        while True:
            img = np.full((60 + 22 * len(lines), 640, 3), 25, np.uint8)
            cv2.putText(img, "Done! Air Ring uses this now. Any key to close.", (12, 30),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.6, ar.CYAN, 1, cv2.LINE_AA)
            for i, line in enumerate(lines):
                cv2.putText(img, line, (12, 60 + i * 22), cv2.FONT_HERSHEY_SIMPLEX, 0.5, ar.WHITE, 1, cv2.LINE_AA)
            cv2.imshow(WIN, img)
            if cv2.waitKey(50) != -1 or cv2.getWindowProperty(WIN, cv2.WND_PROP_VISIBLE) < 1:
                break
    except Quit:
        if gm.CLIPS.exists():
            gm.build()
    finally:
        s.cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
