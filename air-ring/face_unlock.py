"""Face unlock for Liquid's lock screen, run inside Air Ring (it already owns the webcam).

Liquid keeps the named event Local\\LiquidLocked set while its PIN screen is waiting. Only then
do we look for faces: YuNet finds the face, SFace turns it into a 128-number embedding, and we
compare it (cosine similarity) to the samples saved by enrollment. Two good matches in a row
set Local\\LiquidFaceUnlock and Liquid unlocks. A photo of you could fool it (no IR camera);
the PIN stays as the fallback.

Enrollment (Liquid Apple menu > "Set Up Face Unlock...", which sets Local\\LiquidFaceEnroll) opens
a small window, takes ~20 samples while you
look at the camera and turn your head a little, and saves them to data/face.npz.
"""
import collections
import ctypes
import logging
import time
from pathlib import Path

import cv2
import numpy as np

log = logging.getLogger("air_ring")
ROOT = Path(__file__).resolve().parent
DET_PATH = ROOT / "models" / "face_detection_yunet_2023mar.onnx"
REC_PATH = ROOT / "models" / "face_recognition_sface_2021dec.onnx"
DATA_PATH = ROOT / "data" / "face.npz"
POSE_PATH = ROOT / "models" / "pose_landmarker_lite.task"

DEFAULTS = {
    "enabled": True,
    "fps": 5,              # recognition rate while locked (CPU)
    "match": 0.45,         # cosine similarity needed (SFace's own same-person line is 0.363)
    "hits": 2,             # matches in a row before unlocking
    "min_face_px": 70,     # ignore faces smaller than this (too far away)
    "samples": 20,
    "presence": True,      # tell Liquid whether someone is at the desk (walk-away pause/lock)
    "presence_fps": 1,
    # Bent over the desk (fixing something, writing) your face and shoulders drop out of view, so
    # hands in view and movement in the picture count as "someone here" too.
    "presence_hands": 3,       # hand-tracker hits in the last 2 s that mean a hand is really there
    # Share of the picture that changed in the last second. Someone bent over work on the desk, face
    # and shoulders out of view, measured 0.9-6% a second; a still, empty room is close to 0 (the
    # frame is averaged down to 64x36, which removes the camera's grain).
    "presence_motion": 0.005,
    "presence_log": False,     # write what each check saw to the log every 10 s (for tuning)
}

SYNCHRONIZE = 0x00100000
EVENT_MODIFY_STATE = 0x0002
k32 = ctypes.windll.kernel32
k32.OpenEventW.restype = ctypes.c_void_p
k32.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
k32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
k32.SetEvent.argtypes = [ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]


def liquid_locked():
    """True while Liquid's PIN screen is up. Opened fresh each time so a dead Liquid reads as unlocked."""
    h = k32.OpenEventW(SYNCHRONIZE, False, "Local\\LiquidLocked")
    if not h:
        return False
    try:
        return k32.WaitForSingleObject(h, 0) == 0
    finally:
        k32.CloseHandle(h)


def liquid_verifying():
    """True while LiquidKeys.exe (the passkey vault) waits for your face before signing in to a site."""
    h = k32.OpenEventW(SYNCHRONIZE, False, "Local\\LiquidFaceVerifying")
    if not h:
        return False
    try:
        return k32.WaitForSingleObject(h, 0) == 0
    finally:
        k32.CloseHandle(h)


def liquid_running():
    h = k32.OpenEventW(SYNCHRONIZE, False, "Local\\LiquidLocked")
    if h:
        k32.CloseHandle(h)
    return bool(h)


def brighten(frame):
    """The room is dim and the camera runs at a fixed short exposure for sharp hands, so faces
    come out dark. Lift the shadows with a gamma curve before detecting (same for enroll + unlock)."""
    m = float(cv2.resize(frame, (80, 60), interpolation=cv2.INTER_AREA).mean())
    if m >= 100:
        return frame
    gam = float(np.clip(np.log(110 / 255) / np.log(max(m, 4) / 255), 0.35, 1.0))
    lut = (np.power(np.arange(256) / 255.0, gam) * 255).astype(np.uint8)
    return cv2.LUT(frame, lut)


def take_event(name):
    """True if Liquid set this auto-reset event since we last looked (reading it clears it)."""
    h = k32.OpenEventW(SYNCHRONIZE, False, name)
    if not h:
        return False
    try:
        return k32.WaitForSingleObject(h, 0) == 0
    finally:
        k32.CloseHandle(h)


def signal_unlock(name="Local\\LiquidFaceUnlock"):
    h = k32.OpenEventW(EVENT_MODIFY_STATE, False, name)
    if not h:
        return False
    try:
        return bool(k32.SetEvent(h))
    finally:
        k32.CloseHandle(h)


class FaceUnlock:
    def __init__(self, cfg=None):
        self.apply_config(cfg or {})
        self.det = self.rec = None
        self.det_size = None
        self.samples = self._load()
        self.enroll_req = False
        self.enrolling = False
        self.enroll_got = []
        self.enroll_last = 0.0
        self.enroll_done_at = 0.0
        self.enroll_msg = ""
        self.last_check = 0.0
        self.last_lock_poll = 0.0
        self.last_menu_poll = 0.0
        self.misses = 0
        self.last_fail = 0.0
        self.locked = False           # watching for your face: Liquid locked, or a passkey waiting
        self.verifying = False        # ...and it's for a passkey (LiquidKeys.exe), not the lock screen
        self.streak = 0
        self.last_log = 0.0
        self.best = 0.0
        self.pose = None
        self.last_presence = 0.0
        self.present = None
        self._last_here = time.monotonic()   # no "desk empty" log in the first 30 s after starting
        self.hand_times = collections.deque(maxlen=64)
        self.prev_gray = None
        self.last_stat_log = 0.0
        log.info("face unlock: %s", f"{len(self.samples)} samples enrolled" if self.enrolled
                 else "not set up (Liquid Apple menu > Set Up Face Unlock...)")

    def apply_config(self, cfg):
        self.opt = dict(DEFAULTS, **(cfg.get("face_unlock") or {}))

    @property
    def enrolled(self):
        return self.samples is not None and len(self.samples) > 0

    def busy(self):
        """True when the camera loop should not drop to its idle rate."""
        return self.enrolling or self.enroll_req

    # ------------------------------------------------------------ models

    def _models(self, w, h):
        if self.rec is None:
            self.rec = cv2.FaceRecognizerSF.create(str(REC_PATH), "")
        if self.det is None or self.det_size != (w, h):
            self.det = cv2.FaceDetectorYN.create(str(DET_PATH), "", (w, h), 0.8, 0.3, 5)
            self.det_size = (w, h)

    def _largest_face(self, frame):
        """Detect on a half-size copy (cheap), return the biggest face in full-frame coordinates."""
        h, w = frame.shape[:2]
        small = cv2.resize(frame, (w // 2, h // 2), interpolation=cv2.INTER_AREA)
        self._models(w // 2, h // 2)
        _, faces = self.det.detect(small)
        if faces is None or len(faces) == 0:
            return None
        f = max(faces, key=lambda r: r[2] * r[3]).copy()
        f[:14] *= 2                                   # box + 5 landmarks back to full size
        if f[2] < self.opt["min_face_px"]:
            return None
        return f

    def _embed(self, frame, face):
        crop = self.rec.alignCrop(frame, face)
        e = self.rec.feature(crop).flatten().astype(np.float32)
        return e / (np.linalg.norm(e) + 1e-9)

    def score(self, emb):
        return float(np.max(self.samples @ emb)) if self.enrolled else 0.0

    # ------------------------------------------------------------ storage

    def _load(self):
        try:
            return np.load(DATA_PATH)["emb"].astype(np.float32)
        except FileNotFoundError:
            return None
        except Exception:
            log.exception("face unlock: could not read %s", DATA_PATH)
            return None

    def forget(self):
        self.samples = None
        if DATA_PATH.exists():
            DATA_PATH.unlink()
        log.info("face unlock: enrolled face deleted")

    # ------------------------------------------------------------ per frame

    def process(self, frame, now):
        """Called by Air Ring for every camera frame (already mirrored)."""
        if now - self.last_menu_poll > 0.25:          # Liquid's Apple menu items
            self.last_menu_poll = now
            if take_event("Local\\LiquidFaceEnroll") and not self.enrolling:
                self.request_enroll()
            if take_event("Local\\LiquidFaceForget"):
                self.forget()
        if self.enroll_req or self.enrolling or self.enroll_done_at:
            self._enroll_step(brighten(frame), now)
            return
        if not self.locked and self.opt["presence"] and now - self.last_presence > 1.0 / self.opt["presence_fps"]:
            self.last_presence = now
            if liquid_running():
                why, stats = self._someone_here(frame, now)
                here = bool(why)
                signal_unlock("Local\\LiquidPresent" if here else "Local\\LiquidAbsent")
                if here:
                    self._last_here = now
                if here != self.present and (here or now - getattr(self, "_last_here", 0) > 30):
                    self.present = here         # log "empty" only after 30 s of no one (dim room flickers)
                    log.info("presence: %s (%s)", f"someone at the desk, {why}" if here else "desk empty", stats)
                if self.opt["presence_log"] and now - self.last_stat_log >= 10:
                    self.last_stat_log = now
                    log.info("presence check: %s -> %s", stats, why or "nobody")
        if not self.opt["enabled"] or not self.enrolled:
            return
        if now - self.last_lock_poll > 0.25:
            self.last_lock_poll = now
            was = self.locked
            self.verifying = liquid_verifying()
            self.locked = liquid_locked() or self.verifying
            if self.locked != was:
                self.streak = 0
                self.best = 0.0
                log.info("face unlock: %s", "stopped" if not self.locked
                         else "a passkey is waiting, watching for your face" if self.verifying
                         else "Liquid locked, watching for your face")
        if not self.locked or now - self.last_check < 1.0 / self.opt["fps"]:
            return
        self.last_check = now
        frame = brighten(frame)
        face = self._largest_face(frame)
        if face is None:
            self.streak = 0
            if now - self.last_log > 5:
                self.last_log = now
                log.info("face unlock: no face in view (look at the camera, close enough: %d px)", self.opt["min_face_px"])
            return
        signal_unlock("Local\\LiquidFaceSeen")       # the island shows its Face ID square
        s = self.score(self._embed(frame, face))
        self.best = max(self.best, s)
        if s >= self.opt["match"]:
            self.misses = 0
            self.streak += 1
            if self.streak >= self.opt["hits"]:
                if self.verifying:            # a passkey sign-in: tell the vault, and the island
                    ok = signal_unlock("Local\\LiquidFaceVerified")
                    signal_unlock("Local\\LiquidFaceVerifiedUi")
                    log.info("face unlock: match %.2f -> %s", s, "passkey approved" if ok else "vault not listening")
                else:
                    ok = signal_unlock()
                    log.info("face unlock: match %.2f -> %s", s, "unlock sent" if ok else "Liquid not listening")
                self.streak = 0
                self.locked = False                   # wait for Liquid to re-arm the event
                self.last_lock_poll = now + 1.5
        else:
            self.streak = 0
            self.misses += 1
            if self.misses >= 3 and now - self.last_fail > 1.5:   # island shakes: not you
                self.misses = 0
                self.last_fail = now
                signal_unlock("Local\\LiquidFaceNoMatch")
            if now - self.last_log > 5:
                self.last_log = now
                log.info("face unlock: face seen, not a match (%.2f < %.2f)", s, self.opt["match"])

    def saw_hand(self, now):
        """Air Ring's hand tracker found a hand in this frame."""
        self.hand_times.append(now)

    def _motion(self, frame):
        """Share of the picture that changed since the last check (a second ago). Each frame is
        shrunk to 64x36 (averaging away the dim room's grain) and evened out to the same overall
        brightness and contrast, so lamps and screens shifting the room's light don't
        count; a person shifting, leaning or using their hands does."""
        g = cv2.cvtColor(cv2.resize(frame, (64, 36), interpolation=cv2.INTER_AREA),
                         cv2.COLOR_BGR2GRAY).astype(np.float32)
        g = (g - g.mean()) / (g.std() + 4.0)
        prev, self.prev_gray = self.prev_gray, g
        if prev is None:
            return 0.0
        return float((np.abs(g - prev) > 0.35).mean())

    def _someone_here(self, frame, now):
        """Why someone is at the desk ("" = nobody), plus what each check saw, for the log.
        Any one is enough: a face, a head or shoulders or arms, a hand, or movement."""
        moved = self._motion(frame)
        hands = sum(1 for t in self.hand_times if now - t < 2)
        bright = brighten(frame)
        h, w = bright.shape[:2]
        small = cv2.resize(bright, (w // 2, h // 2), interpolation=cv2.INTER_AREA)
        self._models(w // 2, h // 2)
        _, faces = self.det.detect(small)
        face = faces is not None and len(faces) > 0
        body = 0.0
        if not face:
            if self.pose is None:
                import mediapipe as mp
                from mediapipe.tasks.python import BaseOptions, vision
                self._mp = mp
                self.pose = vision.PoseLandmarker.create_from_options(vision.PoseLandmarkerOptions(
                    base_options=BaseOptions(model_asset_path=str(POSE_PATH)),
                    running_mode=vision.RunningMode.IMAGE, num_poses=1,
                    min_pose_detection_confidence=0.4, min_pose_presence_confidence=0.4))
            img = self._mp.Image(image_format=self._mp.ImageFormat.SRGB,
                                 data=cv2.cvtColor(small, cv2.COLOR_BGR2RGB))
            res = self.pose.detect(img)
            if res.pose_landmarks:
                # head, shoulders, elbows or wrists clearly in view (bent forward, the head often
                # reads poorly but the shoulders and arms still do)
                body = max(res.pose_landmarks[0][i].visibility for i in range(17))
        stats = f"face {'yes' if face else 'no'}, body {body:.2f}, hands {hands}, moved {moved * 100:.1f}%"
        # Above half the picture changing at once is the room's light (or the camera re-adjusting),
        # not a person.
        why = ("face" if face else "body" if body > 0.5
               else "hands" if hands >= self.opt["presence_hands"]
               else "moving" if self.opt["presence_motion"] <= moved < 0.5 else "")
        return why, stats

    # ------------------------------------------------------------ enrollment

    def request_enroll(self):
        self.enroll_req = True

    # Guided like the iPhone: straight on, then left, right, up and down, so it still knows you
    # when you're leaning back or looking down at your phone.
    # The last step is for passkeys and walk-up unlocks: sitting back, your face is about half the
    # size, and samples taken only up close matched it at 0.35-0.38 (0.45 needed).
    STEPS = [("Look straight at the camera", "center", 6),
             ("Slowly turn your head to the LEFT", "left", 4),
             ("Now turn your head to the RIGHT", "right", 4),
             ("Tilt your head UP", "up", 4),
             ("Tilt your head DOWN", "down", 4),
             ("Now SIT BACK the way you usually sit, and look at the camera", "far", 6)]

    @staticmethod
    def _head_pose(face):
        """Rough yaw / pitch from YuNet's 5 points: where the nose sits between the eyes."""
        rx, ry, lx, ly, nx, ny = face[4], face[5], face[6], face[7], face[8], face[9]
        mx, my = (rx + lx) / 2, (ry + ly) / 2
        d = max(float(np.hypot(lx - rx, ly - ry)), 1.0)
        return (nx - mx) / d, (ny - my) / d

    def _pose_ok(self, kind, yaw, pitch):
        base = self.enroll_pitch0 if self.enroll_pitch0 is not None else 0.55
        if kind == "center":
            return abs(yaw) < 0.12
        if kind == "far":
            return abs(yaw) < 0.2
        if kind == "left":
            return yaw < -0.18
        if kind == "right":
            return yaw > 0.18
        if kind == "up":
            return pitch < base - 0.1 and abs(yaw) < 0.25
        return pitch > base + 0.1 and abs(yaw) < 0.25

    def _enroll_step(self, frame, now):
        win = "Face Unlock Setup"
        if self.enroll_req:
            self.enroll_req = False
            self.enrolling = True
            self.enroll_done_at = 0.0
            self.enroll_got = []
            self.enroll_msg = ""
            self.enroll_stage = 0
            self.enroll_stage_got = 0
            self.enroll_stage_t = now
            self.enroll_pitch0 = None
            self.enroll_pitches = []
            log.info("face unlock: enrollment started")
        face = None
        hint = ""
        if self.enrolling:
            label, kind, need = self.STEPS[self.enroll_stage]
            face = self._largest_face(frame)
            if face is None:
                hint = "No face found. Sit in front of the camera."
            else:
                yaw, pitch = self._head_pose(face)
                min_score = 0.88 if kind == "center" else 0.7 if kind == "far" else 0.75   # far faces score lower
                if self._pose_ok(kind, yaw, pitch) and face[14] > min_score and now - self.enroll_last > 0.2:
                    self.enroll_got.append(self._embed(frame, face))
                    self.enroll_last = now
                    self.enroll_stage_got += 1
                    if kind == "center":
                        self.enroll_pitches.append(pitch)
            stuck = now - self.enroll_stage_t > 10 and (kind != "center" or self.enroll_stage_got >= 2)
            if self.enroll_stage_got >= need or stuck:
                if kind == "center" and self.enroll_pitches:
                    self.enroll_pitch0 = float(np.median(self.enroll_pitches))
                self.enroll_stage += 1
                self.enroll_stage_got = 0
                self.enroll_stage_t = now
                if self.enroll_stage >= len(self.STEPS):
                    self._finish_enroll(now)

        vh = int(800 * frame.shape[0] / frame.shape[1])      # keep the camera's shape (16:9 at 720p)
        view = cv2.resize(frame, (800, vh))
        k = 800 / frame.shape[1]
        if face is not None:
            x, y, fw, fh = [int(v * k) for v in face[:4]]
            cv2.rectangle(view, (x, y), (x + fw, y + fh), (255, 220, 0), 2)
        cv2.rectangle(view, (0, 0), (800, 92), (0, 0, 0), -1)

        def put(t, y, scale, color=(255, 255, 255), th=2):
            cv2.putText(view, t, (20, y), cv2.FONT_HERSHEY_SIMPLEX, scale, color, th, cv2.LINE_AA)
        if self.enrolling:
            label, kind, need = self.STEPS[self.enroll_stage]
            put(f"Step {self.enroll_stage + 1} of {len(self.STEPS)}:  {label}", 40, 0.85)
            put(hint or f"{self.enroll_stage_got} / {need} for this step", 76, 0.6, (180, 180, 180), 1)
            total = sum(n for _, _, n in self.STEPS)
            cv2.rectangle(view, (0, vh - 8), (int(800 * len(self.enroll_got) / total), vh), (255, 220, 0), -1)
        else:
            put(self.enroll_msg, 40, 0.85, (120, 230, 140))
            put("This window closes by itself.", 76, 0.6, (180, 180, 180), 1)
        cv2.imshow(win, view)
        cv2.setWindowProperty(win, cv2.WND_PROP_TOPMOST, 1)
        cv2.waitKey(1)
        closed = cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1
        if closed or (self.enroll_done_at and now - self.enroll_done_at > 2.5):
            if self.enrolling:
                log.info("face unlock: enrollment cancelled")
            self.enrolling = False
            self.enroll_done_at = 0.0
            try:
                cv2.destroyWindow(win)
                cv2.waitKey(1)
            except cv2.error:
                pass

    def _finish_enroll(self, now):
        emb = np.stack(self.enroll_got)
        sims = emb @ emb.T
        consistency = float(np.median(sims[np.triu_indices(len(emb), 1)]))
        DATA_PATH.parent.mkdir(exist_ok=True)
        np.savez(DATA_PATH, emb=emb, when=time.time())
        self.samples = emb
        self.enrolling = False
        self.enroll_done_at = now
        self.enroll_msg = "Done! Face unlock is on."
        log.info("face unlock: enrolled %d samples (median self-match %.2f)", len(emb), consistency)
