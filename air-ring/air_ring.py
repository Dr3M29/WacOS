"""
Air Ring: always-on webcam version of the ring's gestures.

Runs in the system tray. Nothing fires from ordinary hand movement:

  1. CLICK to arm: snap your fingers, or tap your thumb + index finger together.
     You hear a rising chirp and the tray dot turns cyan. Armed for 3 s, and every
     gesture you do restarts the 3 s, so you can skip several songs in a row.
  2. While armed:
       open-hand swipe  left/right  -> previous / next track
                        up/down     -> volume up / down
       peace-sign swipe left/right  -> previous / next virtual desktop
                        up          -> Task View      down -> show desktop
       click again                  -> play / pause
       hold thumbs up               -> open Claude
       hold rock sign (index+pinky) -> open Spotify
       hold three fingers           -> claude.ai
       hold a fist                  -> cancel
  3. Any time (no snap): pinch thumb + pointer, hold still a moment, then move the
     hand up/down -> volume follows it; open the pinch to stop.
  All of it is remappable in config.json.

    pythonw air_ring.py            # normal: tray only, no console
    python  air_ring.py --preview  # start with the camera window open
    python  air_ring.py --snap-test  # print mic levels to tune snap detection
"""

import argparse
import ctypes
import json
import logging
import math
import os
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import wave
import winsound
from collections import Counter, deque
from pathlib import Path

import cv2
import gesture_model as gm
import pointing as pt
import face_unlock as fu
import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions, vision

ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "config.json"
MODEL_PATH = ROOT / "models" / "hand_landmarker.task"
MODEL_URL = ("https://storage.googleapis.com/mediapipe-models/hand_landmarker/"
             "hand_landmarker/float16/latest/hand_landmarker.task")
SOUNDS = ROOT / "assets"
STARTUP_LNK = Path(os.environ["APPDATA"]) / r"Microsoft\Windows\Start Menu\Programs\Startup\Air Ring.lnk"

log = logging.getLogger("air-ring")

# Landmarks
WRIST, THUMB_IP, THUMB_TIP = 0, 3, 4
INDEX_MCP, INDEX_PIP, INDEX_TIP = 5, 6, 8
MIDDLE_MCP, MIDDLE_PIP, MIDDLE_TIP = 9, 10, 12
RING_PIP, RING_TIP = 14, 16
PINKY_MCP, PINKY_PIP, PINKY_TIP = 17, 18, 20

CYAN, MAGENTA, WHITE, GREY = (255, 230, 0), (200, 40, 255), (255, 255, 255), (110, 110, 110)


# Pinch-drag volume. Overridden by "pinch_volume" in config.json. Distances are in
# fractions of the camera image height; on/off are thumb-index 3D distance / palm size.
VOL_DEFAULTS = {
    "enabled": True,
    "needs_arm": False,       # true = only after a snap
    "on": 0.66,               # pinch closes below this. MediaPipe never lets the tips meet: a
                              # real pinch reads 0.26-0.78 (median ~0.5), an open hand 0.8+...
    "off": 0.85,              # ...and only counts as released above this (hysteresis)
    # Pinch vs fist (measured on a recorded pinch and fist, data/pinch_probe.jsonl). In a fist
    # the thumb also sits right by the pointer, so the shape has to look like a pinch too:
    "min_spread": 0.75,       # pointer tip to middle knuckle: pinch 0.79+, fist 0.71 and under
    "min_thumb_out": 1.0,     # thumb tip to pinky knuckle: pinch 1.03+, fist 0.91 and under
    "min_index_bend": 50,     # a pinch curls the pointer (57-79); a flat hand with the thumb
                              # tucked in has a straight pointer (20-45)
    "hold_s": 0.2,            # pinch held still this long before it engages
    "still_tol": 0.07,        # "still" while engaging
    "release_s": 0.15,        # pinch must look open this long to end a drag (blur flickers)
    "deadzone": 0.03,         # extra travel before the first step
    "step": 0.025,            # hand travel per volume step
    "smooth": 0.35,           # 0..1, lower = smoother but laggier
    "max_steps_per_frame": 2,
}

VIRTUAL_CAMS = ("virtual camera", "meta quest", "moonlight", "obs", "immersed", "voicemod", "snap camera")


def pick_camera(setting):
    """Returns (index, name) or (None, reason). setting: "auto", a name substring, or an index."""
    try:
        from pygrabber.dshow_graph import FilterGraph
        names = FilterGraph().get_input_devices()
    except Exception:
        names = []
    if isinstance(setting, int):
        return setting, names[setting] if setting < len(names) else f"#{setting}"
    for i, n in enumerate(names):
        low = n.lower()
        if setting == "auto" and not any(v in low for v in VIRTUAL_CAMS):
            return i, n
        if setting != "auto" and setting.lower() in low:
            return i, n
    return None, f"no real webcam found (seen: {', '.join(names) or 'none'})"


def load_config():
    # config.json is yours to edit; the first run makes it from the defaults that ship with WacOS
    if not CONFIG_PATH.exists() and (ROOT / "config.default.json").exists():
        CONFIG_PATH.write_text((ROOT / "config.default.json").read_text(encoding="utf-8"), encoding="utf-8")
    with open(CONFIG_PATH, encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------- hand geometry

def dist(a, b):
    """2D distance in the image (for positions on screen)."""
    return math.hypot(a.x - b.x, a.y - b.y)


def dist3(a, b):
    """3D distance. Used on MediaPipe's world landmarks (metres, centred on the hand),
    which keep their true shape when the hand is tilted toward the camera."""
    return math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (getattr(a, "z", 0) - getattr(b, "z", 0)) ** 2)


def palm_size(lm):
    return dist(lm[WRIST], lm[MIDDLE_MCP]) or 1e-6


def palm3(wl):
    return dist3(wl[WRIST], wl[MIDDLE_MCP]) or 1e-6


def bend(wl, a, b, c):
    """Angle (deg) the finger bends at joint b. ~0 = straight, 90+ = curled."""
    v1 = [getattr(wl[b], k, 0) - getattr(wl[a], k, 0) for k in "xyz"]
    v2 = [getattr(wl[c], k, 0) - getattr(wl[b], k, 0) for k in "xyz"]
    n = math.sqrt(sum(x * x for x in v1) * sum(x * x for x in v2)) or 1e-9
    return math.degrees(math.acos(max(-1, min(1, sum(x * y for x, y in zip(v1, v2)) / n))))


FINGERS = ((5, 6, 7, 8), (9, 10, 11, 12), (13, 14, 15, 16), (17, 18, 19, 20))   # mcp, pip, dip, tip


def finger_states(wl):
    """[thumb, index, middle, ring, pinky] extended? Uses 3D joint angles, so it doesn't
    matter which way the hand faces the camera."""
    fingers = [bend(wl, m, p, d) + bend(wl, p, d, t) < 70 for m, p, d, t in FINGERS]
    thumb = dist3(wl[THUMB_TIP], wl[PINKY_MCP]) > dist3(wl[THUMB_IP], wl[PINKY_MCP]) * 1.1 and \
        dist3(wl[THUMB_TIP], wl[INDEX_MCP]) > palm3(wl) * 0.5
    return [thumb] + fingers


def seg_dist(p, a, b):
    """3D distance from point p to the segment a-b."""
    ap = [getattr(p, k, 0) - getattr(a, k, 0) for k in "xyz"]
    ab = [getattr(b, k, 0) - getattr(a, k, 0) for k in "xyz"]
    t = max(0, min(1, sum(x * y for x, y in zip(ap, ab)) / (sum(x * x for x in ab) or 1e-9)))
    return math.sqrt(sum((x - t * y) ** 2 for x, y in zip(ap, ab)))


def palm_facing(wl):
    """0 = palm edge-on to the camera, 1 = palm (or back of the hand) flat to it."""
    a = [getattr(wl[INDEX_MCP], k, 0) - getattr(wl[WRIST], k, 0) for k in "xyz"]
    b = [getattr(wl[PINKY_MCP], k, 0) - getattr(wl[WRIST], k, 0) for k in "xyz"]
    n = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    return abs(n[2]) / (math.sqrt(sum(c * c for c in n)) or 1e-9)


def pinch_ratio(wl):
    # In a fist the thumb rests on the curled index finger: that's not a pinch.
    if bend(wl, 5, 6, 7) + bend(wl, 6, 7, 8) > 130:
        return 9.0
    return dist3(wl[THUMB_TIP], wl[INDEX_TIP]) / palm3(wl)


def classify(lm, pinch_on, wl=None):
    wl = wl or lm
    thumb, index, middle, ring, pinky = finger_states(wl)
    if pinch_ratio(wl) < pinch_on:
        return "pinch"
    if index and middle and ring and pinky:
        return "open"
    if not (index or middle or ring or pinky):
        if thumb and lm[THUMB_TIP].y < lm[WRIST].y - palm_size(lm) * 0.5:
            return "thumbs_up"
        return "fist"
    if index and middle and not ring and not pinky:
        return "peace"
    if index and middle and ring and not pinky:
        return "three"
    if index and pinky and not middle and not ring:
        return "rock"
    if index and not (middle or ring or pinky):
        return "point"
    return None


# ---------------------------------------------------------------- feedback sounds

def make_sounds():
    SOUNDS.mkdir(exist_ok=True)
    sr = 22050

    def tone_file(name, notes):
        path = SOUNDS / f"{name}.wav"
        if path.exists():
            return path
        frames = bytearray()
        for freq, dur in notes:
            n = int(sr * dur)
            for i in range(n):
                env = min(1, i / 80, (n - i) / 300)
                frames += struct.pack("<h", int(9000 * env * math.sin(2 * math.pi * freq * i / sr)))
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes(bytes(frames))
        return path

    return {
        "arm": tone_file("arm", [(880, 0.05), (1320, 0.07)]),
        "action": tone_file("action", [(1180, 0.045)]),
        "cancel": tone_file("cancel", [(660, 0.05), (440, 0.07)]),
    }


SOUND_FILES = {}


def play(name):
    path = SOUND_FILES.get(name)
    if path:
        winsound.PlaySound(str(path), winsound.SND_FILENAME | winsound.SND_ASYNC | winsound.SND_NODEFAULT)


# ---------------------------------------------------------------- actions

_kb = None


def run_action(spec):
    global _kb
    if not spec:
        return
    if "keys" in spec:
        if _kb is None:
            from pynput.keyboard import Controller
            _kb = Controller()
        from pynput.keyboard import Key
        keys = [getattr(Key, k) if len(k) > 1 else k for k in spec["keys"]]
        for _ in range(int(spec.get("repeat", 1))):
            for k in keys:
                _kb.press(k)
            for k in reversed(keys):
                _kb.release(k)
            time.sleep(0.01)
    elif "launch" in spec:
        target = spec["launch"]
        if target.startswith("shell:"):
            subprocess.Popen(["explorer.exe", target])
        else:
            os.startfile(target)


# ---------------------------------------------------------------- snap detector

class SnapDetector:
    """Finger snaps = short, loud, high-frequency clicks that die out in < 0.1 s."""

    def __init__(self, cfg, on_snap, verbose=False):
        t = cfg["tuning"]
        self.level, self.ratio = t["snap_level"], t["snap_ratio"]
        self.hf, self.decay = t["snap_hf"], t["snap_decay"]
        self.on_snap, self.verbose = on_snap, verbose
        self.device = cfg.get("mic")
        self.bg = 0.002
        self.cand = None        # (t0, peak)
        self.stream = None

    def start(self):
        import sounddevice as sd
        dev = self.device
        if isinstance(dev, str):      # same mic is listed once per audio API: take the first
            dev = next((i for i, d in enumerate(sd.query_devices())
                        if d["max_input_channels"] > 0 and dev.lower() in d["name"].lower()), None)
        self.stream = sd.InputStream(device=dev, channels=1, samplerate=44100,
                                     blocksize=256, dtype="float32", callback=self._cb)
        self.stream.start()
        log.info("mic: %s", sd.query_devices(self.stream.device)["name"])

    def stop(self):
        if self.stream:
            self.stream.stop()
            self.stream.close()
            self.stream = None

    def _cb(self, indata, frames, time_info, status):
        x = indata[:, 0]
        now = time.monotonic()
        rms = float(np.sqrt(np.mean(x * x))) + 1e-9
        h = float(np.sqrt(np.mean(np.diff(x) ** 2)))
        if self.cand:
            t0, peak = self.cand
            peak = max(peak, h)
            self.cand = (t0, peak)
            if now - t0 > 0.07:
                ok = h < peak * self.decay
                log.info("sharp sound: peak %.3f, after 70ms %.3f -> %s", peak, h,
                         "snap" if ok else "too long, not a snap")
                if self.verbose:
                    print(f"  candidate peak {peak:.3f}  bg {self.bg:.4f}  after 70ms {h:.3f} -> "
                          f"{'SNAP' if ok else 'too long (not a snap)'}")
                if ok:
                    self.last_peak = peak
                    self.on_snap(t0)
                self.cand = None
            return
        if h > max(self.level, self.ratio * self.bg) and h / rms > self.hf:
            self.cand = (now, h)
            return
        self.bg = 0.995 * self.bg + 0.005 * h


# ---------------------------------------------------------------- gesture engine

class Engine:
    def __init__(self, cfg, on_event):
        self.on_event = on_event
        self.apply_config(cfg)
        self.armed_until = 0.0
        self.cooldown_until = 0.0
        self.block_dir, self.block_until = None, 0.0
        self.track = deque()          # (t, x, y, pose)
        self.pinch_down_at = None
        self.last_click = -1e9
        self.pose, self.pose_since, self.pose_fired = None, 0.0, False
        self.last_hand = 0.0
        self.last_prepose = 0.0       # thumb touching middle finger (snap wind-up)
        self.pending_snaps = deque()
        self.recent_poses = deque(maxlen=5)
        self.side_down_at, self.side_away = None, 0
        self.pending_click = None
        self.motion = deque()
        self.armed_at = 0.0
        self.pending_swipe = None
        self.show_help = True
        self.model = None             # trained personal model (trainer.py), else rules
        self.frames = deque()         # (t, lm, wl) arrays for the motion model
        self.streak = (None, 0)
        self.motion_block_until = 0.0
        self.motion_label = (None, 0.0)
        self.pointing = False         # snap, then point: cursor follows your finger
        self.point_pose_since = None
        self.point_off_since = None
        self.fist_since = None
        self.vol_since = None         # pinch-drag volume: pinch first seen (still hand)
        self.vol_on = False           # engaged: vertical hand moves step the volume
        self.vol_anchor = 0.0         # hand height at the last volume step
        self.vol_y = None             # smoothed hand height
        self.vol_open_since = None    # when the pinch started looking open while engaged
        self.vol_lost = None          # hand dropped out while engaged
        self.vol_steps = 0
        self.vol_hist = deque(maxlen=5)   # (pinch ratio, pinch-shaped?) per frame
        self.last_pinch_seen = -1e9       # chest swipes stay off around a pinch (volume drags)
        self.lock = threading.Lock()

    def apply_config(self, cfg):
        self.t = cfg["tuning"]
        self.bindings = cfg["bindings"]
        self.cfg_arm = set(cfg.get("arm_with", ["snap"]))
        self.cfg_click = set(cfg.get("click_with", ["snap", "side_tap"]))
        self.point_enabled = cfg.get("pointing", True)
        self.vol = {**VOL_DEFAULTS, **(cfg.get("pinch_volume") or {})}
        # Swipes without snapping first, limited to these directions.
        self.swipe_free = not cfg.get("swipes_need_arm", True)
        self.swipe_dirs = set(cfg.get("swipe_dirs", ["left", "right", "up", "down"]))

    def armed(self, now):
        return now < self.armed_until

    def arm(self, now):
        self.armed_until = now + self.t["arm_window_s"]
        self.armed_at = now
        self.track.clear()
        self.pose_fired = True        # don't fire a pose that was already held when arming
        self.on_event("armed", None)

    def disarm(self, sound=True):
        self.armed_until = 0.0
        self.track.clear()
        if sound:
            self.on_event("cancel", None)

    def fire(self, name, now, keep_armed=True):
        self.cooldown_until = now + self.t["cooldown_s"]
        if keep_armed and self.armed(now):
            self.armed_until = now + self.t["arm_window_s"]
        else:
            self.armed_until = 0.0
        self.on_event(name, self.bindings.get(name))

    def click(self, now, source):
        if self.pointing and source in ("snap", "side_tap"):
            if now - self.last_click < 0.4:
                return
            self.last_click = now
            log.info("point click (%s)", source)
            self.on_event("point_click", None)
            return
        armed = self.armed(now)
        allowed = self.cfg_click if armed else self.cfg_arm
        if source not in allowed:
            return
        if now - self.last_click < 0.4:     # two detectors seeing the same motion = one click
            log.info("click (%s) merged with previous", source)
            return
        self.last_click = now
        log.info("click (%s)", source)
        if armed:
            self.fire("click_while_armed", now)
        else:
            self.arm(now)

    def snap(self, t):                       # called from the audio thread
        with self.lock:
            self.pending_snaps.append(t)

    def update(self, lm, now, wl=None):
        with self.lock:
            snaps = list(self.pending_snaps)
            self.pending_snaps.clear()
        for t in snaps:
            log.info("snap heard")
            hand_ok = now - self.last_hand < 0.8
            pre_ok = not self.t["snap_needs_prepose"] or t - self.last_prepose < 0.9
            if hand_ok and pre_ok:
                self.click(now, "snap")
            else:
                log.info("snap ignored (hand seen %s, wind-up seen %s)", hand_ok, pre_ok)

        if self.armed_until and now >= self.armed_until:
            self.armed_until = 0.0
            self.on_event("timeout", None)

        if lm is None:
            # Fast moves blur the hand for a frame or two: keep a volume drag alive briefly.
            if self.vol_on:
                self.vol_lost = self.vol_lost or now
                if now - self.vol_lost > 0.3:
                    self._vol_end(now, "hand lost")
            self.vol_since = None
            if not self.vol_on:
                self.vol_hist.clear()
            self.pinch_down_at = None
            self.pose = None
            self.recent_poses.clear()
            self.motion.clear()
            self.pending_click, self.side_down_at, self.side_away = None, None, 0
            if self.frames and now - self.frames[-1][0] > 0.35:
                self.frames.clear()
            self.streak = (None, 0)
            if self.pointing and now - self.last_hand > 0.6:
                self._end_pointing(now)
            # Fast swipes blur the hand and MediaPipe drops it for a few frames.
            # Keep the track a moment, and if the hand vanished mid-swipe, count it.
            if self.track and now - self.track[-1][0] > 0.35:
                self.track.clear()
            if self.pending_swipe and now - self.pending_swipe[1] > 0.5:
                log.info("%s cancelled: hand left the view (arm lowered?)", self.pending_swipe[0])
                self.pending_swipe = None
            return None

        self.last_hand = now
        self.vol_lost = None
        # A swipe only counts once the hand is seen again, still in view, right after it.
        # Dropping your arm out of frame looks like a swipe down but never comes back.
        if self.pending_swipe and now - self.pending_swipe[1] >= 0.1:
            name = self.pending_swipe[0]
            self.pending_swipe = None
            self.fire(name, now)
        wl = wl or lm
        p3 = palm3(wl)
        if dist3(wl[THUMB_TIP], wl[MIDDLE_TIP]) / p3 < 0.35:
            self.last_prepose = now

        trained_pose = self.model and self.model.pose_knn
        if trained_pose:
            raw = "pinch" if pinch_ratio(wl) < self.t["pinch_on"] else self.model.pose(lm, wl)
        else:
            raw = classify(lm, self.t["pinch_on"], wl)
        self.recent_poses.append(raw)
        pose = Counter(self.recent_poses).most_common(1)[0][0]   # smooth out 1-2 frame flickers

        self._update_pointing(wl, pose, now)

        ratio = pinch_ratio(wl)
        if self.vol.get("debug") and now - getattr(self, "_dbg_t", 0) > 0.4:
            self._dbg_t = now
            log.info("dbg pinch: ratio %.2f  raw %.2f  index bend %.0f  reach %.2f  pose %s  vol_on %s",
                     ratio, dist3(wl[THUMB_TIP], wl[INDEX_TIP]) / palm3(wl),
                     bend(wl, 5, 6, 7) + bend(wl, 6, 7, 8), dist3(wl[INDEX_TIP], wl[WRIST]) / palm3(wl),
                     raw, self.vol_on)
        if self._pinch_volume(lm, wl, ratio, now):
            self.pinch_down_at = None         # a volume drag is never also a pinch tap
            self.recent_poses.clear()
            self.recent_poses.append("pinch")
            return "pinch"

        # Pinch tap = click
        if self.pinch_down_at is None and ratio < self.t["pinch_on"]:
            self.pinch_down_at = now
        elif self.pinch_down_at is not None and ratio > self.t["pinch_off"]:
            if now - self.pinch_down_at < self.t["pinch_tap_max_s"]:
                self.click(now, "pinch")
            self.pinch_down_at = None

        if self.model and self.model.motion_knn:
            self.track.append((now, 0, 0, raw, False))
            while self.track and now - self.track[0][0] > 0.6:
                self.track.popleft()
            if self._model_motion(lm, wl, now):
                return pose
            return self._holds(pose, now)

        # Side tap = thumb tip taps the side of the pointer finger (like the ring's
        # touch pad), fingertips apart. Needs thumb away -> touch -> away, quickly,
        # so a thumb just resting against the finger doesn't count.
        # Swiping blurs the hand, and a flat hand's thumb lies along the pointer, so
        # it only counts while the hand is held still, and fires a beat later unless
        # a swipe starts.
        cx0 = (lm[WRIST].x + lm[MIDDLE_MCP].x) / 2
        cy0 = (lm[WRIST].y + lm[MIDDLE_MCP].y) / 2
        self.motion.append((now, cx0, cy0))
        while self.motion and now - self.motion[0][0] > 0.4:
            self.motion.popleft()
        xs = [m[1] for m in self.motion]
        ys = [m[2] for m in self.motion]
        moving = max(max(xs) - min(xs), max(ys) - min(ys)) > self.t["still_tol"]

        side = seg_dist(wl[THUMB_TIP], wl[INDEX_MCP], wl[7]) / p3
        tips = dist3(wl[THUMB_TIP], wl[INDEX_TIP]) / p3
        if moving:
            self.side_down_at, self.side_away, self.pending_click = None, 0, None
        elif side > self.t["side_off"]:
            if self.side_down_at is not None and now - self.side_down_at < self.t["side_tap_max_s"]:
                self.pending_click = now
            self.side_down_at = None
            self.side_away += 1
        elif side < self.t["side_on"] and tips > 0.35 and self.side_away >= 4 and self.side_down_at is None:
            self.side_down_at = now
            self.side_away = 0
        elif self.side_down_at is not None and now - self.side_down_at > self.t["side_tap_max_s"]:
            self.side_down_at = None      # held too long: resting, not tapping
        if self.pending_click and now - self.pending_click >= 0.25:
            self.pending_click = None
            if not self.pending_swipe and not self.track_moved_since(now - 0.5):
                self.click(now, "side_tap")

        # Swipes (armed only)
        cx = (lm[WRIST].x + lm[MIDDLE_MCP].x) / 2
        cy = (lm[WRIST].y + lm[MIDDLE_MCP].y) / 2
        # "flat" = 3+ fingers out. Looser than the "open" pose, which motion blur in a
        # dim room often breaks mid-swipe. An OK-sign pinch also has 3 fingers out, so a
        # pinch never counts as flat: moving it sideways must not skip a song.
        flat = sum(finger_states(wl)[1:]) >= 3 and ratio >= self.vol["on"]
        self.track.append((now, cx, cy, raw, flat, lm[WRIST].x, lm[WRIST].y, palm_facing(wl)))
        while self.track and now - self.track[0][0] > self.t["swipe_window_s"]:
            self.track.popleft()
        if self._check_swipe(now, self.t["swipe_dist"]):
            return pose
        return self._holds(pose, now)

    def _model_motion(self, lm, wl, now):
        """Trained motion model: classify the last 0.6 s of movement."""
        self.frames.append((now, gm.lm_array(lm), gm.lm_array(wl)))
        while self.frames and now - self.frames[0][0] > gm.WINDOW_S + 0.05:
            self.frames.popleft()
        label, conf = self.model.motion(list(self.frames))
        self.motion_label = (label, conf)
        if label and label != "none" and conf >= 0.8:
            self.streak = (label, self.streak[1] + 1 if self.streak[0] == label else 1)
        else:
            self.streak = (None, 0)

        fired = False
        if self.streak[1] >= 2 and now >= self.motion_block_until:
            if label == "side_tap":
                if not self.pending_click:
                    self.pending_click = now
            elif label.startswith("swipe_") and self._swipe_allowed(label[6:], now):
                d = label[6:]
                poses = Counter(t[3] for t in self.track)
                # Unarmed, the hand must be clearly open for most of the move, so a
                # relaxed hand drifting sideways doesn't skip your song.
                open_ok = not self.swipe_free or poses["open"] >= 0.5 * max(1, len(self.track))
                if not (d == self.block_dir and now < self.block_until) and open_ok:
                    kind = "peace" if poses["peace"] > poses["open"] and not self.swipe_free else "open"
                    self._start_swipe(d, kind, now)
                    self.pending_click = None
                    fired = True
            self.motion_block_until = now + 0.5
            self.streak = (None, 0)

        if self.pending_click and now - self.pending_click >= 0.25:
            self.pending_click = None
            if not self.pending_swipe and not self.track_moved_since(now - 0.5):
                self.click(now, "side_tap")
        return fired

    def _start_swipe(self, d, kind, now):
        name = f"swipe_{d}" if kind == "open" else f"peace_swipe_{d}"
        # nothing else swipes for a moment: moving the arm over and back is one swipe, not two
        self.swipe_lock_until = now + self.t.get("swipe_refractory_s", 1.0)
        self.block_dir = {"left": "right", "right": "left", "up": "down", "down": "up"}[d]
        self.block_until = now + self.t["return_block_s"]
        self.track.clear()
        self.frames.clear()
        self.cooldown_until = now + self.t["cooldown_s"]
        if self.swipe_free and d in ("left", "right"):
            # The wait-for-the-hand-to-come-back check is for arm drops, which are
            # vertical. A sideways swipe often ends with the hand out of frame.
            self.fire(name, now)
            return
        self.pending_swipe = (name, now)

    def _holds(self, pose, now):
        if self.pointing:
            return pose
        # Hold poses (armed only)
        if pose != self.pose:
            self.pose, self.pose_since, self.pose_fired = pose, now, False
        held = now - self.pose_since >= self.t["hold_s"]
        if (self.armed(now) and held and not self.pose_fired and now >= self.cooldown_until
                and pose in ("thumbs_up", "rock", "three", "point", "fist")):
            self.pose_fired = True
            if pose == "fist":
                self.disarm()
            elif self.bindings.get(pose):
                self.fire(pose, now, keep_armed=False)
        return pose

    def _update_pointing(self, wl, pose, now):
        pointing_hand = pt.is_pointing(wl)
        if not self.pointing:
            # Starts only after a snap: armed + pointing hand held for a moment.
            if pointing_hand and self.armed(now) and self.point_enabled:
                self.point_pose_since = self.point_pose_since or now
                if now - self.point_pose_since >= 0.25:
                    self.pointing = True
                    self.point_off_since = None
                    self.fist_since = None
                    log.info("pointing on")
                    self.on_event("point_start", None)
            else:
                self.point_pose_since = None
            return
        self.armed_until = now + self.t["arm_window_s"]       # stays on while you point
        if pointing_hand:
            self.point_off_since = None
            self.fist_since = None
            return
        # Closing to a fist right after pointing = grab: move the active window to
        # the screen you were pointing at.
        if pose == "fist":
            self.fist_since = self.fist_since or now
            if now - self.fist_since >= 0.3:
                self.on_event("grab", None)
                self._end_pointing(now)
            return
        self.fist_since = None
        self.point_off_since = self.point_off_since or now
        if now - self.point_off_since >= 0.6:
            self._end_pointing(now)

    def _end_pointing(self, now):
        self.pointing = False
        self.point_pose_since = self.point_off_since = self.fist_since = None
        self.armed_until = 0.0
        self.cooldown_until = now + self.t["cooldown_s"]
        log.info("pointing off")
        self.on_event("point_end", None)

    def _pinch_volume(self, lm, wl, ratio, now):
        """Pinch and hold still ~0.25 s, then move the hand up/down: the volume follows,
        one step per `step` of hand travel. Returns True while it owns the hand."""
        v = self.vol
        if not v["enabled"] or self.pointing:
            if self.vol_on:
                self._vol_end(now, "pointing")
            self.vol_since = None
            return False
        y = (lm[WRIST].y + lm[MIDDLE_MCP].y) / 2
        x = (lm[WRIST].x + lm[MIDDLE_MCP].x) / 2

        # Live landmarks jitter frame to frame: judge the pinch on the median of the
        # last few frames so one bad frame doesn't reset the hold.
        p3 = palm3(wl)
        spread = dist3(wl[INDEX_TIP], wl[MIDDLE_MCP]) / p3
        thumb_out = dist3(wl[THUMB_TIP], wl[PINKY_MCP]) / p3
        ibend = bend(wl, 5, 6, 7) + bend(wl, 6, 7, 8)
        shape = spread >= v["min_spread"] and thumb_out >= v["min_thumb_out"] and ibend >= v["min_index_bend"]
        self.vol_hist.append((ratio, shape))
        r_med = sorted(h[0] for h in self.vol_hist)[len(self.vol_hist) // 2]
        shape_ok = 2 * sum(h[1] for h in self.vol_hist) > len(self.vol_hist)   # most recent frames
        if r_med < v["on"] and shape_ok:
            self.last_pinch_seen = now

        if not self.vol_on:
            # Engage: thumb + index together, hand held still, for hold_s. A hand moving
            # through a pinch-like shape (swipes, waving) never gets there.
            why = ("open" if r_med >= v["on"]
                   else f"shape (spread {spread:.2f}, thumb {thumb_out:.2f}, bend {ibend:.0f})" if not shape_ok
                   else "arm" if v["needs_arm"] and not self.armed(now)
                   else "cooldown" if now < self.cooldown_until else None)
            if why is None and self.vol_since is not None:
                moved = max(abs(x - self.vol_hold[0]), abs(y - self.vol_hold[1]))
                if moved > v["still_tol"]:
                    why = f"moving {moved:.3f}"
            if why:
                if v.get("debug") and r_med < v["on"] + 0.1 and now - getattr(self, "_dbg_w", 0) > 0.25:
                    self._dbg_w = now
                    log.info("dbg pinch not engaged: %s (ratio med %.2f)", why, r_med)
                self.vol_since = None if why != "cooldown" else self.vol_since
                if why.startswith("moving"):
                    self.vol_since, self.vol_hold = now, (x, y)
                return False
            if self.vol_since is None:
                self.vol_since, self.vol_hold = now, (x, y)
                return False
            if now - self.vol_since < v["hold_s"]:
                return False
            self.vol_on, self.vol_y, self.vol_anchor = True, y, y
            self.vol_open_since, self.vol_steps, self.vol_broke = None, 0, False
            self.pending_swipe, self.pending_click = None, None
            log.info("pinch volume on (ratio %.2f, spread %.2f, thumb %.2f, bend %.0f)",
                     r_med, spread, thumb_out, ibend)
            self.on_event("volume_on", None)
            return True

        # Release with hysteresis: must look clearly open (or closed into a fist) for
        # release_s. Moving the hand blurs it, and a frame or two of a "wide" pinch
        # shouldn't drop the drag.
        if r_med > v["off"] or not shape_ok:
            self.vol_open_since = self.vol_open_since or now
            if now - self.vol_open_since >= v["release_s"]:
                self._vol_end(now, "released")
                return False
            return True
        self.vol_open_since = None

        a = v["smooth"]
        self.vol_y = a * y + (1 - a) * self.vol_y
        delta = self.vol_anchor - self.vol_y            # up = positive (image y grows downward)
        need = v["step"] + (0 if self.vol_broke else v["deadzone"])
        if abs(delta) >= need:
            n = min(int((abs(delta) - (need - v["step"])) / v["step"]), v["max_steps_per_frame"])
            d = 1 if delta > 0 else -1
            self.vol_broke = True
            self.vol_anchor -= d * (need - v["step"] + n * v["step"])
            self.vol_steps += d * n
            self.on_event("volume_step", d * n)
        return True

    def _vol_end(self, now, why):
        self.vol_on, self.vol_since, self.vol_lost = False, None, None
        log.info("pinch volume off (%s), %+d steps", why, self.vol_steps)
        # Opening the hand after a drag shouldn't read as a swipe or a tap.
        self.track.clear()
        self.frames.clear()
        self.motion.clear()
        self.pending_swipe, self.pending_click = None, None
        self.side_down_at, self.side_away = None, 0
        self.cooldown_until = now + self.t["cooldown_s"]
        self.motion_block_until = now + 0.5
        self.on_event("volume_off", None)

    def track_moved_since(self, t0):
        """Did a swipe fire or start since t0?"""
        return self.block_until - self.t["return_block_s"] > t0

    def _check_slide(self, now, min_dist):
        """Unarmed left/right: a flat hand, palm (or back) to the camera, sliding sideways.
        Measured at the WRIST, so a wave (which pivots at the wrist) never counts. Only
        the first half has to be flat and facing: the hand can relax as it goes over."""
        tr = list(self.track)
        if len(tr) < 4:
            return False
        dx = tr[-1][5] - tr[0][5]
        dy = tr[-1][6] - tr[0][6]
        if abs(dx) <= min_dist:
            return False
        d = "right" if dx > 0 else "left"
        why = None
        if abs(dx) < self.t.get("swipe_straight", 2.0) * abs(dy):
            why = f"too diagonal (dx {dx:+.2f}, dy {dy:+.2f})"
        elif not self._swipe_allowed(d, now):
            why = "cooldown/volume" if d in self.swipe_dirs else None
            if why is None:
                return False
        elif d == self.block_dir and now < self.block_until:
            return False                      # the hand coming back after a swipe
        else:
            first = tr[: max(2, len(tr) // 2)]
            flat = sum(1 for t in first if t[4])
            facing = sum(1 for t in first if t[7] > self.t.get("swipe_facing", 0.5))
            need = self.t.get("swipe_flat_frac", 0.5) * len(first)
            if flat < need:
                why = f"hand not flat ({flat}/{len(first)} frames 3+ fingers)"
            elif facing < need:
                why = f"hand edge-on, not facing camera ({facing}/{len(first)})"
        if why:
            if self.vol.get("debug") and now - getattr(self, "_dbg_s", 0) > 0.3:
                self._dbg_s = now
                log.info("dbg swipe %s rejected: %s", d, why)
            return False
        log.info("swipe %s (wrist moved %.2f in %.2f s)", d, abs(dx), tr[-1][0] - tr[0][0])
        self._start_swipe(d, "open", now)
        return True

    def _check_swipe(self, now, min_dist):
        if self.pointing:
            return False
        if self.swipe_free:
            return self._check_slide(now, min_dist)
        if len(self.track) < 3:
            return False
        dx = self.track[-1][1] - self.track[0][1]
        dy = self.track[-1][2] - self.track[0][2]
        if max(abs(dx), abs(dy)) <= min_dist:
            return False
        if max(abs(dx), abs(dy)) < 2 * min(abs(dx), abs(dy)):
            return False                      # diagonal: not a clear swipe
        if abs(dx) > abs(dy):
            d = "right" if dx > 0 else "left"
        else:
            d = "down" if dy > 0 else "up"
        if not self._swipe_allowed(d, now):
            return False
        if d == self.block_dir and now < self.block_until:
            return False                      # the hand coming back after a swipe
        # The hand must be flat (open or peace) for most of the swipe, so moving a
        # relaxed or curled hand around doesn't count.
        tr = list(self.track)
        moving = [tr[i] for i in range(1, len(tr))
                  if math.hypot(tr[i][1] - tr[i - 1][1], tr[i][2] - tr[i - 1][2]) > 0.008]
        poses = Counter(t[3] for t in moving)
        if self.swipe_free:
            flat = sum(1 for t in moving if t[4])
        else:
            flat = poses["open"] + poses["peace"]
        if len(moving) < 2 or flat < 0.5 * len(moving):
            if self.vol.get("debug"):
                log.info("dbg swipe %s rejected: hand flat in %d of %d moving frames", d, flat, len(moving))
            return False
        kind = "peace" if poses["peace"] > poses["open"] and not self.swipe_free else "open"
        self._start_swipe(d, kind, now)
        return True

    def external_swipe(self, d, now, source="chest"):
        """A swipe seen some other way than the hand landmarks (ChestSwipe). Same rules as a
        hand swipe: switched-on directions, cooldown, the return block; and never around a pinch."""
        if self.vol_on or self.vol_since is not None or now - self.last_pinch_seen < 0.6:
            log.info("%s swipe %s ignored: pinching", source, d)
            return False
        if not self._swipe_allowed(d, now) or (d == self.block_dir and now < self.block_until):
            return False
        self._start_swipe(d, "open", now)
        return True

    def _swipe_allowed(self, d, now):
        if self.pointing or self.vol_on or now < self.cooldown_until or d not in self.swipe_dirs:
            return False
        if now < getattr(self, "swipe_lock_until", 0.0):
            return False
        if self.swipe_free:
            return True
        return self.armed(now) and now - self.armed_at >= self.t["arm_grace_s"]


# ---------------------------------------------------------------- preview drawing

HELP_LINES = [
    "[H hides this]",
    "open hand swipe  L/R: prev/next song",
    "pinch + hold still, then move up/down: volume",
]


def draw(frame, lm, pose, engine, now, last_event, fps):
    h, w = frame.shape[:2]
    if lm is not None:
        pts = [(int(p.x * w), int(p.y * h)) for p in lm]
        for c in vision.HandLandmarksConnections.HAND_CONNECTIONS:
            cv2.line(frame, pts[c.start], pts[c.end], MAGENTA, 2, cv2.LINE_AA)
        for p in pts:
            cv2.circle(frame, p, 4, CYAN, -1, cv2.LINE_AA)
        if engine.armed(now):
            cx = int((lm[WRIST].x + lm[MIDDLE_MCP].x) / 2 * w)
            cy = int((lm[WRIST].y + lm[MIDDLE_MCP].y) / 2 * h)
            r = int(palm_size(lm) * w * 0.9)
            left = (engine.armed_until - now) / engine.t["arm_window_s"]
            cv2.ellipse(frame, (cx, cy), (r, r), -90, 0, int(360 * left), CYAN, 3, cv2.LINE_AA)
    if lm is not None and engine.vol_on:
        cy = int(engine.vol_anchor * h)
        cv2.line(frame, (0, cy), (w, cy), CYAN, 1, cv2.LINE_AA)
    status = (f"VOLUME {engine.vol_steps:+d}" if engine.vol_on else "POINTING" if engine.pointing
              else "ARMED" if engine.armed(now) else "ready")
    if engine.model and engine.model.motion_knn:
        ml, mc = engine.motion_label
        cv2.putText(frame, f"trained model  motion: {ml or '-'} {mc:.1f}", (12, h - 36),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, GREY, 1, cv2.LINE_AA)
    cv2.putText(frame, f"pose: {pose or '-'}   {status}", (12, 28),
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, CYAN, 2, cv2.LINE_AA)
    cv2.putText(frame, f"{fps:4.1f} fps", (12, h - 14), cv2.FONT_HERSHEY_SIMPLEX, 0.55, GREY, 1, cv2.LINE_AA)
    if engine.show_help:
        for i, line in enumerate(HELP_LINES):
            y = 58 + i * 20
            cv2.putText(frame, line, (12, y), cv2.FONT_HERSHEY_SIMPLEX, 0.48, (0, 0, 0), 3, cv2.LINE_AA)
            cv2.putText(frame, line, (12, y), cv2.FONT_HERSHEY_SIMPLEX, 0.48,
                        CYAN if i == 0 else WHITE, 1, cv2.LINE_AA)
    if last_event and now - last_event[1] < 1.2:
        text = last_event[0].upper().replace("_", " ")
        (tw, _), _ = cv2.getTextSize(text, cv2.FONT_HERSHEY_DUPLEX, 1.4, 2)
        cv2.putText(frame, text, ((w - tw) // 2, h // 2), cv2.FONT_HERSHEY_DUPLEX, 1.4, WHITE, 2, cv2.LINE_AA)


# ---------------------------------------------------------------- chest-zone swipes

POSE_PATH = ROOT / "models" / "pose_landmarker_lite.task"
CHEST_DEFAULTS = {
    "enabled": True,
    "diff": 10,             # grey-level change that counts as motion (dark room: keep it low)
    "min_pixels": 0.02,     # share of the zone that has to move
    "travel_box": 0.6,      # a swipe moves the centre of motion across this much of the box...
    "window_s": 0.6,        # ...within this long...
    "straight": 1.5,        # ...this many times more sideways than up/down
    # The box, in shoulder widths: from box_top above the shoulder line down to box_bottom below
    # it (about the bottom of the chest), and box_side past each shoulder. Hands in your lap or
    # down by the chair are outside it.
    "box_top": 0.2,
    "box_bottom": 0.6,
    "box_side": 0.3,
    "band": 0.6,            # the motion has to stay above this (shoulder widths below the shoulder line)
    "head_still": 0.25,     # while the nose moves less than this (shoulder widths)
    "shoulders_still": 0.15,   # and the shoulders less than this: a shrug or a turn isn't a swipe
                               # (a real arm swipe rotates them ~0.12 on the recording)
    "need_raised": True,    # and a forearm is up (wrist above elbow) during the swipe: typing or
                            # reaching for the mouse keeps the wrist at or below the elbow
    "pose_hz": 15,          # body-pose runs at most this often (CPU); the zone moves slowly
}


class ChestSwipe:
    """Swipes from the motion in front of your chest, for when the hand model can't see the hand.

    Measured on a recording (2026-11-21, dim room, sitting back): mid-swipe the hand is a blur
    against the shirt and the hand model found it in 3% of frames, so swipes almost never fired.
    The body-pose model still finds the shoulders, which place a zone in front of the chest;
    frame differencing shows what moves in it, and a swipe is the centre of that motion
    travelling sideways far and fast enough. This found ~11 swipes in the same recording."""

    def __init__(self, cfg):
        self.apply_config(cfg)
        self.pose = vision.PoseLandmarker.create_from_options(vision.PoseLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=str(POSE_PATH)), running_mode=vision.RunningMode.VIDEO,
            num_poses=1, min_pose_detection_confidence=0.4, min_pose_presence_confidence=0.4,
            min_tracking_confidence=0.4))
        self.prev = None
        self.zone = None        # x0, y0, x1, y1 (normalized), shoulder width, shoulder line y, aspect
        self.nose = None
        self.hist = deque()     # (t, cx, cy, nose x, rows below the shoulder line), in shoulder widths
        self.cool = 0.0
        self.last_motion = -1e9
        self.last_pose = -1e9
        self.raised_at = -1e9   # last time a wrist was seen above its elbow
        self.trail = deque(maxlen=12)

    def apply_config(self, cfg):
        self.c = {**CHEST_DEFAULTS, **(cfg.get("chest_swipes") or {})}

    def update(self, frame, now, ts):
        """frame: the mirrored camera frame. Returns "left" / "right" or None."""
        c = self.c
        if not c["enabled"]:
            return None
        h, w = frame.shape[:2]
        aspect = w / h
        r = None
        if now - self.last_pose >= 1.0 / c["pose_hz"]:
            self.last_pose = now
            small = cv2.resize(frame, (640, int(640 / aspect)), interpolation=cv2.INTER_AREA)
            r = self.pose.detect_for_video(mp.Image(image_format=mp.ImageFormat.SRGB,
                                                    data=cv2.cvtColor(small, cv2.COLOR_BGR2RGB)), ts)
        if r is not None and r.pose_landmarks:
            pl = r.pose_landmarks[0]
            if any(pl[wr].visibility > 0.3 and pl[wr].y < pl[el].y - 0.01 for wr, el in ((15, 13), (16, 14))):
                self.raised_at = now
            if pl[11].visibility > 0.5 and pl[12].visibility > 0.5:
                sx0, sx1 = sorted((pl[11].x, pl[12].x))
                sw = max(sx1 - sx0, 0.05)
                sy = (pl[11].y + pl[12].y) / 2
                swh = sw * aspect                       # shoulder width in units of image height
                self.zone = (sx0 - c["box_side"] * sw, sy - c["box_top"] * swh,
                             sx1 + c["box_side"] * sw, sy + c["box_bottom"] * swh, sw, sy, aspect)
                self.nose = pl[0].x / sw
                self.shoulders = ((sx0 + sx1) / 2 / sw, sy / swh)
        fw, fh = 320, int(320 / aspect)
        g = cv2.GaussianBlur(cv2.cvtColor(cv2.resize(frame, (fw, fh), interpolation=cv2.INTER_AREA),
                                          cv2.COLOR_BGR2GRAY), (5, 5), 0)
        p, self.prev = self.prev, g
        if p is not None and self.zone is not None and p.shape == g.shape:
            x0, y0, x1, y1, sw, sy, _ = self.zone
            X0, X1 = max(0, int(x0 * fw)), min(fw, int(x1 * fw))
            Y0, Y1 = max(0, int(y0 * fh)), min(fh, int(y1 * fh))
            if X1 - X0 > 10 and Y1 - Y0 > 10:
                m = cv2.absdiff(g[Y0:Y1, X0:X1], p[Y0:Y1, X0:X1]) > c["diff"]
                if m.mean() >= c["min_pixels"]:
                    ys, xs = np.nonzero(m)
                    cxn, cyn = (X0 + xs.mean()) / fw, (Y0 + ys.mean()) / fh
                    self.last_motion = now
                    self.trail.append((now, cxn, cyn))
                    swh = sw * aspect
                    shx, shy = getattr(self, "shoulders", (0.0, 0.0))
                    self.hist.append((now, cxn / sw, cyn / swh, self.nose or 0.0, (cyn - sy) / swh, shx, shy))
        while self.hist and now - self.hist[0][0] > c["window_s"]:
            self.hist.popleft()
        if len(self.hist) < 3 or now < self.cool:
            return None
        hs = self.hist
        dx, dy = hs[-1][1] - hs[0][1], hs[-1][2] - hs[0][2]
        box_w = (self.zone[2] - self.zone[0]) / self.zone[4]     # box width in shoulder widths
        if abs(dx) < c["travel_box"] * box_w or abs(dx) < c["straight"] * abs(dy):
            return None
        d = "right" if dx > 0 else "left"
        low = max(x[4] for x in hs)
        head = max(abs(x[3] - hs[0][3]) for x in hs)
        shoulders = max(max(abs(x[5] - hs[0][5]), abs(x[6] - hs[0][6])) for x in hs)
        hs.clear()
        self.cool = now + 0.3
        if low > c["band"]:
            log.info("chest swipe %s ignored: below the chest (%.2f)", d, low)
            return None
        if head > c["head_still"]:
            log.info("chest swipe %s ignored: head moved too (%.2f)", d, head)
            return None
        if shoulders > c["shoulders_still"]:
            log.info("chest swipe %s ignored: shoulders moved (%.2f)", d, shoulders)
            return None
        if c["need_raised"] and now - self.raised_at > c["window_s"] + 0.3:
            log.info("chest swipe %s ignored: no forearm raised", d)
            return None
        log.info("chest swipe %s (moved %.0f%% of the box)", d, 100 * abs(dx) / box_w)
        return d

    def draw(self, frame, now):
        """The zone and the recent centre of motion, on the preview."""
        if not self.zone or not self.c["enabled"]:
            return
        h, w = frame.shape[:2]
        x0, y0, x1, y1 = self.zone[:4]
        cv2.rectangle(frame, (int(x0 * w), int(y0 * h)), (int(x1 * w), int(y1 * h)), GREY, 1, cv2.LINE_AA)
        pts = [(int(x * w), int(y * h)) for t, x, y in self.trail if now - t < 0.6]
        for a, b in zip(pts, pts[1:]):
            cv2.line(frame, a, b, (0, 200, 255), 2, cv2.LINE_AA)


# ---------------------------------------------------------------- link to Liquid

_k32 = ctypes.windll.kernel32
_k32.CreateEventW.restype = ctypes.c_void_p
_k32.CreateEventW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_wchar_p]
_k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
_k32.WaitForSingleObject.restype = ctypes.c_uint32


class LiquidLink:
    """Liquid's Air Gestures panel talks to Air Ring through named kernel objects:

      Local\\AirRingShared   shared memory, laid out as below
      Local\\AirRingPreview  event Liquid keeps set while the panel shows the camera
      Local\\AirRingCmd      event Liquid sets after writing a command

      0     b"AIR1"
      4     seq (u32): odd while Air Ring is writing, bumped on every status write (heartbeat)
      8     status length (u32)    12  jpeg length (u32)
      64    status: UTF-8 "key=value" lines
      1088  command from Liquid: seq (u32), length (u32), UTF-8 text at 1096
      1600  JPEG of the camera with the hand drawn on it
    """
    SIZE = 512 * 1024
    STATUS, CMD, JPEG = 64, 1088, 1600

    def __init__(self):
        import mmap
        self.mm = mmap.mmap(-1, self.SIZE, tagname="Local\\AirRingShared")
        self.mm[0:4] = b"AIR1"
        self.seq = 0
        self.cmd_seq = 0
        self.ev_preview = _k32.CreateEventW(None, True, False, "Local\\AirRingPreview")
        self.ev_cmd = _k32.CreateEventW(None, False, False, "Local\\AirRingCmd")
        self.jpeg = b""
        self.last_status = 0.0
        self.last_frame = 0.0

    def preview_wanted(self):
        return bool(self.ev_preview) and _k32.WaitForSingleObject(self.ev_preview, 0) == 0

    def command(self):
        """The newest command Liquid wrote, or None."""
        if not self.ev_cmd or _k32.WaitForSingleObject(self.ev_cmd, 0) != 0:
            return None
        seq, n = struct.unpack_from("<II", self.mm, self.CMD)
        if seq == self.cmd_seq or n > 500:
            return None
        self.cmd_seq = seq
        return bytes(self.mm[self.CMD + 8:self.CMD + 8 + n]).decode("utf-8", "replace").strip()

    def publish(self, status, frame=None):
        """status: dict. frame: BGR image to share (only while the preview is wanted)."""
        if frame is not None:
            h, w = frame.shape[:2]
            small = cv2.resize(frame, (480, int(480 * h / w)), interpolation=cv2.INTER_AREA)
            ok, buf = cv2.imencode(".jpg", small, [cv2.IMWRITE_JPEG_QUALITY, 72])
            if ok and len(buf) < self.SIZE - self.JPEG:
                self.jpeg = buf.tobytes()
        text = "\n".join(f"{k}={v}" for k, v in status.items()).encode("utf-8")[:1000]
        self.seq = (self.seq + 1) | 1                          # odd: writing
        struct.pack_into("<I", self.mm, 4, self.seq)
        jl = len(self.jpeg) if frame is not None else 0       # no preview wanted: no picture
        struct.pack_into("<II", self.mm, 8, len(text), jl)
        self.mm[self.STATUS:self.STATUS + len(text)] = text
        if jl:
            self.mm[self.JPEG:self.JPEG + jl] = self.jpeg
        self.seq += 1                                          # even: done
        struct.pack_into("<I", self.mm, 4, self.seq)


# ---------------------------------------------------------------- main worker

class App:
    def __init__(self, preview=False):
        self.cfg = load_config()
        self.preview = preview
        self.paused = self.cfg.get("start_paused", False)
        self.quit = False
        self.state = "idle"           # idle | armed | paused | nohand
        self.on_state = lambda s: None
        self.last_event = None
        self.engine = Engine(self.cfg, self.handle_event)
        self.snap = None
        self.load_model()
        from pynput.mouse import Controller as Mouse
        self.mouse = Mouse()
        self.pointer = pt.PointMap.load()
        self.mons = pt.monitors()
        self.face = None
        self.face_unlock = fu.FaceUnlock(self.cfg)
        self.euro = pt.OneEuro()
        self.point_hist = deque()
        self.last_point_mon = None
        self.point_freeze_until = 0.0
        self.cam_name = ""
        self.fps = 0.0
        self.pose = None
        self.chest = None             # ChestSwipe, made by the camera loop
        try:
            self.link = LiquidLink()
        except Exception:
            log.exception("could not open the link to Liquid")
            self.link = None
        log.info("pointing: %s", "calibrated" if self.pointer else "not calibrated (tray > Calibrate pointing)")

    # ------------------------------------------------------------ Liquid's Air Gestures panel

    def status(self):
        now = time.monotonic()
        e = self.engine
        last = f"{self.last_event[0]} {now - self.last_event[1]:.0f}" if self.last_event else ""
        return {
            "state": self.state, "camera": self.cam_name, "fps": f"{self.fps:.0f}",
            "pose": self.pose or "", "hand": int(now - e.last_hand < 0.5),
            "volume": f"{e.vol_steps:+d}" if e.vol_on else "", "last": last,
            "pinch": int(bool(e.vol["enabled"])), "swipes": int(bool(e.swipe_dirs)),
            "face": len(self.face_unlock.samples) if self.face_unlock.enrolled else 0,
            "model": "trained" if e.model else "rules",
        }

    def link_tick(self, frame=None, lm=None, now=None):
        """Every camera frame (and a few times a second while paused): run Liquid's commands,
        publish status, and while the panel is open, the camera with the hand drawn on it."""
        if not self.link:
            return
        now = now or time.monotonic()
        try:
            cmd = self.link.command()
            if cmd:
                self.do_command(cmd)
            if frame is not None and self.link.preview_wanted():
                vis = frame.copy()
                help_was, self.engine.show_help = self.engine.show_help, False
                # no big event banner: Liquid prints what happened under the picture
                draw(vis, lm, self.pose, self.engine, now, None, self.fps)
                if self.chest is not None:
                    self.chest.draw(vis, now)
                self.engine.show_help = help_was
                self.link.publish(self.status(), vis)
                self.link.last_status = now
            elif now - self.link.last_status >= 0.5:
                self.link.publish(self.status())
                self.link.last_status = now
        except Exception:
            log.exception("Liquid link failed")

    def do_command(self, cmd):
        log.info("Liquid: %s", cmd)
        word, _, arg = cmd.partition(" ")
        on = arg.strip() in ("1", "on", "true")
        if word == "train":
            self.train()
        elif word == "pause":
            self.paused = True
        elif word == "resume":
            self.paused = False
        elif word == "reload":
            self.reload()
        elif word in ("pinch", "swipes"):
            cfg = load_config()
            if word == "pinch":
                cfg.setdefault("pinch_volume", {})["enabled"] = on
            else:
                cfg["swipe_dirs"] = ["left", "right"] if on else []
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(cfg, f, indent=2)
                f.write("\n")
            self.cfg = cfg
            self.engine.apply_config(cfg)
        else:
            log.warning("Liquid: unknown command %r", cmd)

    def load_model(self):
        cal = gm.load_calibration()
        if "snap_level" in cal:
            self.cfg["tuning"]["snap_level"] = cal["snap_level"]
        try:
            self.engine.model = gm.load()
        except Exception:
            log.exception("could not load trained model, using built-in rules")
            self.engine.model = None
        log.info("gestures: %s", "trained model" if self.engine.model else "built-in rules (run training!)")

    def handle_event(self, name, spec):
        now = time.monotonic()
        if name in ("point_start", "point_end"):
            self.euro = pt.OneEuro()
            self.point_hist.clear()
            if name == "point_start" and self.pointer is None:
                log.warning("pointing isn't calibrated yet - tray menu > Calibrate pointing")
        elif name == "point_click":
            self.point_click(now)
        elif name == "grab":
            if self.last_point_mon and now - self.last_point_mon[1] < 2:
                if pt.move_foreground_to(self.last_point_mon[0]):
                    play("action")
                    log.info("grab -> moved active window to screen %d", self.last_point_mon[0] + 1)
        elif name == "armed":
            play("arm")
        elif name == "cancel":
            play("cancel")
        elif name == "timeout":
            pass
        elif name in ("volume_on", "volume_off"):
            pass
        elif name == "volume_step":
            # Volume keys rather than setting the level ourselves: Liquid catches them,
            # shows its HUD, and in AudioRelay mode sets each app's level (setting the
            # master directly would be inaudible there and undone by Liquid's AudioTick).
            # Without Liquid, Windows handles them with its own flyout.
            key = "media_volume_up" if spec > 0 else "media_volume_down"
            try:
                run_action({"keys": [key], "repeat": abs(spec)})
            except Exception:
                log.exception("volume step failed")
            return
        else:
            play("action")
            log.info("%s -> %s", name, spec)
            try:
                before = pt.foreground_hwnd()
                run_action(spec)
                # Apps open on the screen you pointed at most recently.
                if spec and "launch" in spec and self.last_point_mon and now - self.last_point_mon[1] < 15:
                    threading.Thread(target=self._place_new_window, args=(before, self.last_point_mon[0]),
                                     daemon=True).start()
            except Exception:
                log.exception("action %s failed", name)
        self.last_event = (name, now)

    def point_click(self, now):
        if self.pointer is None or not self.point_hist:
            return
        # Click where you were pointing just BEFORE the tap: the tap itself wiggles the finger.
        target = min(self.point_hist, key=lambda h: abs(h[0] - (now - 0.35)))
        from pynput.mouse import Button
        self.mouse.position = (int(target[1]), int(target[2]))
        self.mouse.click(Button.left)
        self.point_freeze_until = now + 0.3
        play("action")

    @staticmethod
    def _place_new_window(before, mon):
        end = time.monotonic() + 6
        while time.monotonic() < end:
            h = pt.foreground_hwnd()
            if h and h != before:
                time.sleep(0.4)            # let it finish opening
                pt.move_foreground_to(mon)
                return
            time.sleep(0.1)

    def update_pointer(self, image, lm, wl, now):
        if not self.engine.pointing or self.pointer is None or lm is None:
            return
        if self.face is None:
            self.face = pt.FaceTracker()
        face = self.face.update(image, now)
        f = pt.point_features(gm.lm_array(lm), gm.lm_array(wl), face)
        p = self.euro(self.pointer.predict(f), now)
        x, y = pt.clamp_to_screens(p[0], p[1], self.mons)
        self.point_hist.append((now, x, y))
        while self.point_hist and now - self.point_hist[0][0] > 1.0:
            self.point_hist.popleft()
        self.last_point_mon = (pt.monitor_at(x, y, self.mons), now)
        if now >= self.point_freeze_until:
            self.mouse.position = (int(x), int(y))

    def calibrate_pointing(self):
        def go():
            was = self.paused
            self.paused = True
            time.sleep(0.8)
            exe = Path(sys.executable)
            subprocess.run([str(exe), str(ROOT / "pointing.py")], cwd=ROOT,
                           creationflags=subprocess.CREATE_NO_WINDOW if exe.name == "pythonw.exe" else 0)
            self.pointer = pt.PointMap.load()
            self.mons = pt.monitors()
            log.info("pointing: %s", "calibrated" if self.pointer else "not calibrated")
            self.paused = was
        threading.Thread(target=go, daemon=True).start()

    def reload(self):
        self.cfg = load_config()
        self.engine.apply_config(self.cfg)
        self.face_unlock.apply_config(self.cfg)
        if self.chest is not None:
            self.chest.apply_config(self.cfg)
        self.load_model()
        self.stop_snap()              # restarted with the new settings on the next frame
        log.info("config reloaded")

    def train(self):
        """Pause (frees the camera), run the training window, then load the new model."""
        def go():
            was = self.paused
            self.paused = True
            time.sleep(0.8)
            exe = Path(sys.executable)
            subprocess.run([str(exe), str(ROOT / "trainer.py")], cwd=ROOT,
                           creationflags=subprocess.CREATE_NO_WINDOW if exe.name == "pythonw.exe" else 0)
            self.reload()
            self.paused = was
        threading.Thread(target=go, daemon=True).start()

    def start_snap(self):
        if self.cfg.get("snap_enabled", True) and self.snap is None:
            try:
                self.snap = SnapDetector(self.cfg, self.engine.snap)
                self.snap.start()
            except Exception:
                log.exception("mic unavailable, snaps disabled")
                self.snap = None

    def stop_snap(self):
        if self.snap:
            self.snap.stop()
            self.snap = None

    def boost(self, frame):
        """Dim room: brighten the frame the hand tracker sees with a gamma curve (keeps
        dark areas from clipping, unlike a plain gain). Re-measured about once a second."""
        target = self.cfg.get("low_light_target", 110)
        if not target:
            return frame
        self._boost_n = getattr(self, "_boost_n", 0) + 1
        if self._boost_n % 30 == 1:
            luma = float(frame[::8, ::8].mean())
            g = 1.0
            if 4 < luma < target:
                g = max(0.4, math.log(target / 255) / math.log(luma / 255))
            if abs(g - getattr(self, "_boost_g", 1.0)) > 0.05:
                self._boost_g = g
                self._boost_lut = (np.linspace(0, 1, 256) ** g * 255).astype(np.uint8) if g < 0.98 else None
                log.info("light: frame brightness %.0f -> %s", luma,
                         f"boosting (gamma {g:.2f})" if g < 0.98 else "no boost")
        lut = getattr(self, "_boost_lut", None)
        return cv2.LUT(frame, lut) if lut is not None else frame

    def set_state(self, s):
        if s != self.state:
            self.state = s
            self.on_state(s)

    def run(self):
        landmarker = vision.HandLandmarker.create_from_options(vision.HandLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=ensure_model()),
            running_mode=vision.RunningMode.VIDEO,
            num_hands=1,
            # Lower than MediaPipe's defaults so a relaxed or curled hand gets picked
            # up in a dim room, not just a flat palm.
            min_hand_detection_confidence=self.cfg.get("hand_detect_conf", 0.5),
            min_hand_presence_confidence=self.cfg.get("hand_presence_conf", 0.4),
            min_tracking_confidence=self.cfg.get("hand_track_conf", 0.4),
        ))
        cap = None
        t0 = time.monotonic()
        prev, fps, last_seen = t0, 0.0, 0.0
        window_open = False
        cam_msg = None

        while not self.quit:
            if self.paused:
                if cap is not None:
                    cap.release()
                    cap = None
                    self.stop_snap()
                if window_open:
                    cv2.destroyWindow("Air Ring")
                    window_open = False
                self.set_state("paused")
                self.link_tick()
                time.sleep(0.2)
                continue

            if cap is None:
                idx, name = pick_camera(self.cfg.get("camera", "auto"))
                if idx is not None:
                    cap = cv2.VideoCapture(idx, cv2.CAP_DSHOW)
                    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
                    # 1280x720: sitting back, your face is ~50 px at 640x480, too small to recognise
                    # (match 0.35 against the 0.45 needed); 720p doubles it. Hands and swipes shrink
                    # the frame for their own models, so they don't care.
                    cap.set(cv2.CAP_PROP_FRAME_WIDTH, int(self.cfg.get("frame_width", 1280)))
                    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, int(self.cfg.get("frame_height", 720)))
                    cap.set(cv2.CAP_PROP_FPS, 60)
                    exp = self.cfg.get("exposure")
                    if exp is not None:   # manual: auto exposure drops to 20 fps and blurs fast hands
                        cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.25)
                        cap.set(cv2.CAP_PROP_EXPOSURE, exp)
                    else:
                        cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.75)
                if idx is None or not cap.isOpened():
                    if cam_msg != name:
                        log.error("camera: %s - retrying every 5 s", name)
                        cam_msg = name
                    cap = None
                    self.set_state("nocam")
                    self.cam_name = ""
                    for _ in range(25):           # keep answering Liquid while we wait
                        self.link_tick()
                        time.sleep(0.2)
                    continue
                log.info("camera: %s", name)
                self.cam_name = name
                cam_msg = None
                self.start_snap()

            if self.snap is None:
                self.start_snap()
            ok, frame = cap.read()
            if not ok:
                log.warning("camera read failed, reopening")
                cap.release()
                cap = None
                time.sleep(1)
                continue

            frame = cv2.flip(frame, 1)
            try:                              # only does work while Liquid is locked or enrolling
                self.face_unlock.process(frame, time.monotonic())
            except Exception:
                log.exception("face unlock failed")
            now = time.monotonic()
            frame = self.boost(frame)
            if self.engine.vol.get("debug") and now - getattr(self, "_snap_t", 0) > 3:
                self._snap_t = now                # what the hand tracker sees, for tuning
                cv2.imwrite(str(ROOT / "data" / "debug_view.jpg"), frame)
            # The hand model gets a small, slightly blurred copy. Measured on a recording in this dim
            # room (2026-11-21): the grainy full frame defeats MediaPipe's palm finder (7% of swipe
            # frames, 61% of pinch frames found at 640 wide, confidence 0.5); shrinking to 256 wide
            # with INTER_AREA plus a 3x3 blur averages the grain out: 68% and 84%.
            hw = int(self.cfg.get("hand_input_width", 256))
            hsmall = cv2.resize(frame, (hw, int(hw * frame.shape[0] / frame.shape[1])), interpolation=cv2.INTER_AREA)
            k = int(self.cfg.get("hand_denoise", 3))
            if k > 1:
                hsmall = cv2.GaussianBlur(hsmall, (k | 1, k | 1), 0)
            image = mp.Image(image_format=mp.ImageFormat.SRGB, data=cv2.cvtColor(hsmall, cv2.COLOR_BGR2RGB))
            # MediaPipe throws if two frames share a millisecond (frames can arrive back to
            # back after a slow one), so keep the timestamp strictly increasing.
            ts = max(int((now - t0) * 1000), getattr(self, "_last_ts", -1) + 1)
            self._last_ts = ts
            result = landmarker.detect_for_video(image, ts)
            lm = result.hand_landmarks[0] if result.hand_landmarks else None
            wl = result.hand_world_landmarks[0] if result.hand_world_landmarks else None
            if lm is not None:
                last_seen = now
                self.face_unlock.saw_hand(now)    # hands in view count as someone at the desk

            pose = self.engine.update(lm, now, wl)
            try:
                if self.chest is None:
                    self.chest = ChestSwipe(self.cfg)
                d = self.chest.update(frame, now, ts)
                if d and self.face_unlock.locked:
                    log.info("chest swipe %s ignored: Liquid is locked", d)
                elif d:
                    self.engine.external_swipe(d, now)
            except Exception:
                log.exception("chest swipes failed")
            try:
                if self.engine.pointing:      # its face tracker wants the full-size frame
                    self.update_pointer(mp.Image(image_format=mp.ImageFormat.SRGB,
                                                 data=cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)), lm, wl, now)
            except Exception:
                log.exception("pointer failed")
            self.set_state("armed" if self.engine.armed(now) else "idle")
            fps = 0.9 * fps + 0.1 / max(now - prev, 1e-6)
            prev = now
            self.fps, self.pose = fps, pose
            self.link_tick(frame, lm, now)

            if self.preview:
                draw(frame, lm, pose, self.engine, now, self.last_event, fps)
                cv2.imshow("Air Ring", frame)
                window_open = True
                if cv2.waitKey(1) & 0xFF in (ord("h"), ord("H")):
                    self.engine.show_help = not self.engine.show_help
                if cv2.getWindowProperty("Air Ring", cv2.WND_PROP_VISIBLE) < 1:
                    self.preview = False
            if window_open and not self.preview:
                cv2.destroyWindow("Air Ring")
                cv2.waitKey(1)
                window_open = False

            # Save CPU when no hand is around: the camera keeps running but we only
            # look a few times a second until a hand shows up.
            panel = self.link is not None and self.link.preview_wanted()
            moving = self.chest is not None and now - self.chest.last_motion < 1.0   # a blurred swipe in progress
            if now - last_seen > 2 and not self.engine.armed(now) and not self.preview and not panel \
                    and not moving and not self.face_unlock.busy():
                time.sleep(max(0, 1 / self.cfg["tuning"]["idle_fps"] - (time.monotonic() - now)))
                cap.grab()

        if cap is not None:
            cap.release()
        self.stop_snap()
        cv2.destroyAllWindows()
        landmarker.close()


def ensure_model():
    if not MODEL_PATH.exists():
        MODEL_PATH.parent.mkdir(exist_ok=True)
        log.info("downloading hand model to %s", MODEL_PATH)
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
    return str(MODEL_PATH)


# ---------------------------------------------------------------- tray

def set_startup(enabled):
    if enabled:
        pyw = Path(sys.executable).with_name("pythonw.exe")
        ps = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{STARTUP_LNK}');"
              f"$s.TargetPath='{pyw}';$s.Arguments='\"{ROOT / 'air_ring.py'}\"';"
              f"$s.WorkingDirectory='{ROOT}';$s.Save()")
        subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                       creationflags=subprocess.CREATE_NO_WINDOW, check=False)
    elif STARTUP_LNK.exists():
        STARTUP_LNK.unlink()


def tray_image(state):
    from PIL import Image, ImageDraw
    colors = {"idle": (235, 235, 235), "armed": (0, 230, 255), "paused": (90, 90, 90), "nocam": (230, 60, 60)}
    img = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse((6, 6, 58, 58), outline=colors.get(state, (235, 235, 235)), width=9)
    if state == "armed":
        d.ellipse((24, 24, 40, 40), fill=colors["armed"])
    return img


def run_tray(app):
    import pystray
    from pystray import Menu, MenuItem as Item

    def toggle_pause(icon, item):
        app.paused = not app.paused
        log.info("paused" if app.paused else "resumed")

    def toggle_preview(icon, item):
        app.preview = not app.preview

    def toggle_startup(icon, item):
        set_startup(not STARTUP_LNK.exists())

    def quit_app(icon, item):
        app.quit = True
        icon.stop()

    icon = pystray.Icon("air-ring", tray_image(app.state), "Air Ring", Menu(
        Item("Air Gestures in Liquid...", lambda i, it: fu.signal_unlock("Local\\LiquidOpenAirGestures"),
             visible=lambda i: fu.liquid_running()),
        Item("Show camera", toggle_preview, checked=lambda i: app.preview, default=True),
        Item("Pause (camera off)", toggle_pause, checked=lambda i: app.paused),
        Menu.SEPARATOR,
        Item("Train my gestures...", lambda i, it: app.train()),
        Item("Calibrate pointing...", lambda i, it: app.calibrate_pointing()),
        Item("Edit gestures (config.json)", lambda i, it: os.startfile(CONFIG_PATH)),
        Item("Reload config", lambda i, it: app.reload()),
        Item("Start with Windows", toggle_startup, checked=lambda i: STARTUP_LNK.exists()),
        Item("Open log", lambda i, it: os.startfile(ROOT / "air_ring.log")),
        Menu.SEPARATOR,
        Item("Quit", quit_app),
    ))
    titles = {"idle": "Air Ring: click to arm", "armed": "Air Ring: ARMED",
              "paused": "Air Ring: paused", "nocam": "Air Ring: no camera"}

    def on_state(s):
        icon.icon = tray_image(s)
        icon.title = titles.get(s, "Air Ring")
    app.on_state = on_state

    def run_forever():
        # pythonw has no console: without this a crash in the camera loop kills it silently
        # while the tray icon stays up. Log it and start the loop again.
        while not app.quit:
            try:
                app.run()
            except Exception:
                log.exception("camera loop crashed, restarting it in 2 s")
                time.sleep(2)
    worker = threading.Thread(target=run_forever, daemon=True)
    worker.start()
    icon.run()
    app.quit = True
    worker.join(timeout=3)


def liquid_alive():
    h = ctypes.windll.kernel32.OpenMutexW(0x00100000, False, "LiquidShell.Single")   # SYNCHRONIZE
    if h:
        ctypes.windll.kernel32.CloseHandle(h)
    return bool(h)


def run_embedded(app):
    """Part of WacOS: Liquid starts Air Ring and keeps it running, and its settings are in System
    Settings > Air Gestures, so there's no tray icon. It quits once Liquid has been gone for 30
    seconds (a reload of Liquid takes a few)."""
    def run_forever():
        while not app.quit:
            try:
                app.run()
            except Exception:
                log.exception("camera loop crashed, restarting it in 2 s")
                time.sleep(2)
    worker = threading.Thread(target=run_forever, daemon=True)
    worker.start()
    gone = None
    while not app.quit:
        time.sleep(2)
        if liquid_alive():
            gone = None
        elif gone is None:
            gone = time.monotonic()
        elif time.monotonic() - gone > 30:
            log.info("Liquid isn't running any more, quitting")
            break
    app.quit = True
    worker.join(timeout=3)


def single_instance():
    ctypes.windll.kernel32.CreateMutexW(None, False, "AirRingSingleInstance")
    return ctypes.windll.kernel32.GetLastError() != 183    # ERROR_ALREADY_EXISTS


def snap_test():
    cfg = load_config()
    print("Snap test. Snap a few times, then clap, talk, type. Ctrl+C to stop.\n")
    det = SnapDetector(cfg, lambda t: print(f"  >>> SNAP at {t:.2f}"), verbose=True)
    det.start()
    try:
        while True:
            time.sleep(0.5)
            print(f"background level {det.bg:.4f}   (trigger needs > {max(det.level, det.ratio * det.bg):.3f})",
                  end="\r")
    except KeyboardInterrupt:
        det.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preview", action="store_true", help="open the camera window at start")
    ap.add_argument("--snap-test", action="store_true", help="print mic levels to tune snaps")
    ap.add_argument("--train", action="store_true", help="open the gesture training session at start")
    ap.add_argument("--calibrate", action="store_true", help="open pointing calibration at start")
    ap.add_argument("--embedded", action="store_true", help="started by WacOS: no tray icon, quit with Liquid")
    args = ap.parse_args()

    handlers = [logging.FileHandler(ROOT / "air_ring.log", encoding="utf-8")]
    if sys.stdout is not None:
        handlers.append(logging.StreamHandler(sys.stdout))
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S",
                        handlers=handlers)
    logging.getLogger("comtypes").setLevel(logging.WARNING)

    if args.snap_test:
        return snap_test()
    pt.dpi_aware()
    if not single_instance():
        log.info("already running")
        return
    # Task Scheduler starts tasks below normal priority, so a busy program made gestures and face
    # unlock lag. Normal shares the CPU fairly with what you're running.
    ctypes.windll.kernel32.SetPriorityClass(ctypes.c_void_p(-1), 0x20)  # this process, NORMAL_PRIORITY_CLASS
    SOUND_FILES.update(make_sounds())
    app = App(preview=args.preview)
    if args.train:
        app.train()
    if args.calibrate:
        app.calibrate_pointing()
    if args.embedded:
        run_embedded(app)
    else:
        run_tray(app)


if __name__ == "__main__":
    main()
