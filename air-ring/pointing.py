"""
Pointing: works out where on your monitors you're pointing.

Rather than guessing angles, it learns your setup: calibration shows dots across
both screens and you point at each one. From that it fits a mapping from

    pointer finger (tip position + direction in 3D) + your head position + hand size

to a spot on the screens. Head and hand size are in there so it keeps working
when you lean or sit back a bit.

    python pointing.py              # calibrate (about a minute), then a live test
    python pointing.py --test       # just the live test with the saved calibration
"""

import argparse
import ctypes
import time
import urllib.request
from ctypes import wintypes
from pathlib import Path

import cv2
import mediapipe as mp
import numpy as np
from mediapipe.tasks.python import BaseOptions, vision

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
POINT_MODEL = DATA / "pointing.npz"
FACE_MODEL = ROOT / "models" / "blaze_face_short_range.tflite"
FACE_URL = ("https://storage.googleapis.com/mediapipe-models/face_detector/"
            "blaze_face_short_range/float16/latest/blaze_face_short_range.tflite")


def dpi_aware():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        pass


# ---------------------------------------------------------------- monitors & windows

class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]


def monitors():
    """[(x, y, w, h), ...] in physical pixels, left to right."""
    out = []

    def cb(h, dc, r, l):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(MONITORINFO)
        ctypes.windll.user32.GetMonitorInfoW(h, ctypes.byref(mi))
        rc = mi.rcMonitor
        out.append((rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top))
        return True
    proto = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
    ctypes.windll.user32.EnumDisplayMonitors(0, 0, proto(cb), 0)
    return sorted(out)


def monitor_at(x, y, mons=None):
    mons = mons or monitors()
    for i, (mx, my, mw, mh) in enumerate(mons):
        if mx <= x < mx + mw and my <= y < my + mh:
            return i
    # off-screen: nearest
    d = [abs(x - (mx + mw / 2)) + abs(y - (my + mh / 2)) for mx, my, mw, mh in mons]
    return int(np.argmin(d))


def clamp_to_screens(x, y, mons):
    i = monitor_at(x, y, mons)
    mx, my, mw, mh = mons[i]
    return min(max(x, mx), mx + mw - 1), min(max(y, my), my + mh - 1)


def move_foreground_to(mon_index):
    """Move the active window onto a monitor, keeping it maximised if it was."""
    u = ctypes.windll.user32
    hwnd = u.GetForegroundWindow()
    if not hwnd:
        return False
    mons = monitors()
    rc = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(rc))
    cur = monitor_at((rc.left + rc.right) // 2, (rc.top + rc.bottom) // 2, mons)
    if cur == mon_index:
        return False
    was_max = bool(u.IsZoomed(hwnd))
    if was_max:
        u.ShowWindow(hwnd, 9)                      # SW_RESTORE
        u.GetWindowRect(hwnd, ctypes.byref(rc))
    w, h = rc.right - rc.left, rc.bottom - rc.top
    mx, my, mw, mh = mons[mon_index]
    w, h = min(w, mw), min(h, mh)
    u.SetWindowPos(hwnd, 0, mx + (mw - w) // 2, my + (mh - h) // 2, w, h, 0x0004 | 0x0010)  # NOZORDER|NOACTIVATE
    if was_max:
        u.ShowWindow(hwnd, 3)                      # SW_MAXIMIZE
    return True


def foreground_hwnd():
    return ctypes.windll.user32.GetForegroundWindow()


# ---------------------------------------------------------------- face

class FaceTracker:
    """Cheap face detector, only for where your eyes are (pointing goes eye -> fingertip)."""

    def __init__(self):
        if not FACE_MODEL.exists():
            FACE_MODEL.parent.mkdir(exist_ok=True)
            urllib.request.urlretrieve(FACE_URL, FACE_MODEL)
        self.det = vision.FaceDetector.create_from_options(vision.FaceDetectorOptions(
            base_options=BaseOptions(model_asset_path=str(FACE_MODEL)),
            running_mode=vision.RunningMode.VIDEO, min_detection_confidence=0.5))
        self.last, self.last_t = None, -1e9
        self.ts = 0

    def update(self, image, now):
        """image: mp.Image. Returns (eye_x, eye_y, eye_dist) - last known for up to 2 s
        (the pointing hand often covers the face)."""
        self.ts = max(self.ts + 1, int(now * 1000))
        res = self.det.detect_for_video(image, self.ts)
        if res.detections:
            k = res.detections[0].keypoints          # 0 right eye, 1 left eye
            ex, ey = (k[0].x + k[1].x) / 2, (k[0].y + k[1].y) / 2
            self.last = (ex, ey, float(np.hypot(k[0].x - k[1].x, k[0].y - k[1].y)))
            self.last_t = now
        return self.last if now - self.last_t < 2.0 else None


# ---------------------------------------------------------------- features & model

def point_features(lm, wl, face):
    """lm, wl: (21,3) arrays. face: (ex, ey, eye_dist) or None."""
    tip, mcp = lm[8], lm[5]
    d = wl[8] - wl[5]
    d = d / (np.linalg.norm(d) or 1e-6)
    palm = float(np.linalg.norm(lm[0, :2] - lm[9, :2]))
    ex, ey, ed = face if face else (0.5, 0.35, 0.08)
    return np.array([tip[0], tip[1], mcp[0], mcp[1], d[0], d[1], d[2], palm,
                     ex, ey, ed, tip[0] - ex, tip[1] - ey], np.float32)


QUAD = [0, 1, 4, 5, 11, 12]          # features that get squared / crossed terms


def expand(Z):
    q = Z[:, QUAD]
    cross = [q[:, i] * q[:, j] for i in range(len(QUAD)) for j in range(i, len(QUAD))]
    return np.column_stack([np.ones(len(Z)), Z] + cross)


class PointMap:
    def __init__(self, mu, sd, W):
        self.mu, self.sd, self.W = mu, sd, W

    @classmethod
    def fit(cls, X, Y, groups):
        """Ridge regression; strength picked by leave-one-target-out error."""
        mu, sd = X.mean(0), X.std(0) + 1e-6
        A = expand((X - mu) / sd)
        best = None
        for lam in (0.03, 0.1, 0.3, 1, 3, 10, 30):
            errs = []
            for g in np.unique(groups):
                tr, te = groups != g, groups == g
                W = cls._solve(A[tr], Y[tr], lam)
                errs.append(np.median(np.linalg.norm(A[te] @ W - Y[te], axis=1)))
            e = float(np.median(errs))
            if best is None or e < best[0]:
                best = (e, lam)
        W = cls._solve(A, Y, best[1])
        m = cls(mu, sd, W)
        m.cv_error = best[0]
        return m

    @staticmethod
    def _solve(A, Y, lam):
        reg = lam * np.eye(A.shape[1])
        reg[0, 0] = 0
        return np.linalg.solve(A.T @ A + reg, A.T @ Y)

    def predict(self, x):
        return (expand(((x - self.mu) / self.sd)[None]) @ self.W)[0]

    def save(self):
        DATA.mkdir(exist_ok=True)
        np.savez(POINT_MODEL, mu=self.mu, sd=self.sd, W=self.W)

    @classmethod
    def load(cls):
        if not POINT_MODEL.exists():
            return None
        z = np.load(POINT_MODEL)
        return cls(z["mu"], z["sd"], z["W"])


class OneEuro:
    """Smooths jitter when you hold still, stays responsive when you move."""

    def __init__(self, min_cutoff=0.8, beta=0.02, d_cutoff=1.0):
        self.mc, self.beta, self.dc = min_cutoff, beta, d_cutoff
        self.x = self.dx = None
        self.t = None

    @staticmethod
    def _a(cutoff, dt):
        r = 2 * np.pi * cutoff * dt
        return r / (r + 1)

    def __call__(self, x, t):
        x = np.asarray(x, float)
        if self.x is None or t - self.t > 0.5:
            self.x, self.dx, self.t = x, np.zeros_like(x), t
            return x
        dt = max(t - self.t, 1e-3)
        dx = (x - self.x) / dt
        self.dx = self.dx + self._a(self.dc, dt) * (dx - self.dx)
        cutoff = self.mc + self.beta * np.linalg.norm(self.dx)
        self.x = self.x + self._a(cutoff, dt) * (x - self.x)
        self.t = t
        return self.x


def is_pointing(wl):
    """Pointer finger straight, middle/ring/pinky curled (rule-based, no training needed)."""
    import air_ring as ar
    st = ar.finger_states(wl)
    return st[1] and not st[2] and not st[3] and not st[4]


# ---------------------------------------------------------------- calibration UI

class Screens:
    """One borderless full-screen OpenCV window per monitor."""

    def __init__(self, mons):
        self.mons = mons
        self.names = []
        for i, (x, y, w, h) in enumerate(mons):
            n = f"air-ring-point-{i}"
            cv2.namedWindow(n, cv2.WINDOW_NORMAL)
            cv2.moveWindow(n, x, y)
            cv2.resizeWindow(n, w, h)
            cv2.setWindowProperty(n, cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
            self.names.append(n)

    def show(self, target=None, text=(), progress=None, dot=None, colour=(0, 230, 255)):
        """target/dot: virtual-desktop pixel coords."""
        for i, (n, (x, y, w, h)) in enumerate(zip(self.names, self.mons)):
            img = np.full((h, w, 3), 18, np.uint8)
            if i == 0:
                for j, line in enumerate(text):
                    cv2.putText(img, line, (40, 60 + j * 38), cv2.FONT_HERSHEY_SIMPLEX, 0.9,
                                (255, 230, 0) if j == 0 else (230, 230, 230), 2, cv2.LINE_AA)
            for pt, big in ((target, True), (dot, False)):
                if pt is None or monitor_at(*pt, self.mons) != i:
                    continue
                cx, cy = int(pt[0] - x), int(pt[1] - y)
                if big:
                    cv2.circle(img, (cx, cy), 26, colour, 3, cv2.LINE_AA)
                    cv2.circle(img, (cx, cy), 6, colour, -1, cv2.LINE_AA)
                    if progress is not None:
                        cv2.ellipse(img, (cx, cy), (40, 40), -90, 0, int(360 * progress), (255, 255, 255), 4)
                else:
                    cv2.circle(img, (cx, cy), 14, (200, 40, 255), -1, cv2.LINE_AA)
            cv2.imshow(n, img)
        return cv2.waitKey(1) & 0xFF

    def close(self):
        for n in self.names:
            cv2.destroyWindow(n)


def targets(mons, cols=4, rows=3, margin=0.12):
    pts = []
    for mi, (x, y, w, h) in enumerate(mons):
        for r in range(rows):
            for c in range(cols):
                pts.append((x + w * (margin + (1 - 2 * margin) * c / (cols - 1)),
                            y + h * (margin + (1 - 2 * margin) * r / (rows - 1))))
    return pts


class Tracker:
    """Camera + hand + face, shared by calibration and the live test."""

    def __init__(self):
        import air_ring as ar
        cfg = ar.load_config()
        idx, name = ar.pick_camera(cfg.get("camera", "auto"))
        if idx is None:
            raise SystemExit(f"Camera: {name}")
        self.cap = cv2.VideoCapture(idx, cv2.CAP_DSHOW)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
        self.cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        self.cap.set(cv2.CAP_PROP_FPS, 60)
        if cfg.get("exposure") is not None:
            self.cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.25)
            self.cap.set(cv2.CAP_PROP_EXPOSURE, cfg["exposure"])
        self.hands = vision.HandLandmarker.create_from_options(vision.HandLandmarkerOptions(
            base_options=BaseOptions(model_asset_path=ar.ensure_model()),
            running_mode=vision.RunningMode.VIDEO, num_hands=1,
            min_hand_detection_confidence=0.5, min_hand_presence_confidence=0.4,
            min_tracking_confidence=0.4))
        self.face = FaceTracker()
        self.t0 = time.monotonic()

    def read(self):
        """-> (now, features or None, pointing?)"""
        import gesture_model as gm
        ok, img = self.cap.read()
        if not ok:
            raise SystemExit("camera read failed")
        img = cv2.flip(img, 1)
        now = time.monotonic()
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
        res = self.hands.detect_for_video(image, int((now - self.t0) * 1000))
        face = self.face.update(image, now)
        if not res.hand_landmarks:
            return now, None, False
        lm, wl = gm.lm_array(res.hand_landmarks[0]), gm.lm_array(res.hand_world_landmarks[0])
        return now, point_features(lm, wl, face), is_pointing(res.hand_world_landmarks[0])

    def close(self):
        self.cap.release()


def live_test(tr, scr, model, mons, seconds=None):
    euro = OneEuro()
    start = time.monotonic()
    while seconds is None or time.monotonic() - start < seconds:
        now, f, pointing = tr.read()
        dot = None
        if f is not None and pointing:
            p = euro(model.predict(f), now)
            dot = clamp_to_screens(p[0], p[1], mons)
        k = scr.show(text=["LIVE TEST: point around - the pink dot follows your finger.",
                           "ESC = done   R = redo calibration"], dot=dot)
        if k == 27:
            return False
        if k in (ord("r"), ord("R")):
            return True
    return False


def calibrate(tr, scr, mons):
    pts = targets(mons)
    X, Y, G = [], [], []
    while True:
        k = scr.show(text=["POINTING CALIBRATION",
                           "Sit how you normally sit. A dot appears on each screen in turn:",
                           "point your pointer finger right at it and hold until the ring fills.",
                           "Point naturally - like showing someone where something is.",
                           f"{len(pts)} dots, about a minute.   SPACE = start   ESC = cancel"])
        if k == 27:
            return None
        if k == 32:
            break
    for gi, t in enumerate(pts):
        settle_until = time.monotonic() + 0.9
        got, give_up = [], time.monotonic() + 6
        while len(got) < 22 and time.monotonic() < give_up:
            now, f, pointing = tr.read()
            settling = now < settle_until
            if f is not None and pointing and not settling:
                got.append(f)
            msg = "point at the dot" if f is not None else "can't see your hand"
            if f is not None and not pointing:
                msg = "make a pointing hand (only the pointer finger out)"
            if scr.show(target=t, progress=len(got) / 22,
                        text=[f"dot {gi + 1} of {len(pts)}", msg]) == 27:
                return None
        for f in got:
            X.append(f)
            Y.append(t)
            G.append(gi)
    if len(set(G)) < len(pts) * 0.6:
        return None
    model = PointMap.fit(np.array(X), np.array(Y, float), np.array(G))
    model.save()
    return model


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test", action="store_true", help="skip calibration, just test")
    args = ap.parse_args()
    dpi_aware()
    mons = monitors()
    tr = Tracker()
    scr = Screens(mons)
    try:
        model = PointMap.load() if args.test else None
        while True:
            if model is None:
                model = calibrate(tr, scr, mons)
                if model is None:
                    return
                err = model.cv_error
                print(f"pointing accuracy: about {err:.0f} px off (typical)")
                t_end = time.monotonic() + 3
                while time.monotonic() < t_end:
                    scr.show(text=["Calibrated!", f"Typical error: about {err:.0f} px "
                                   f"({'great' if err < 120 else 'ok' if err < 250 else 'rough - try R to redo'})"])
            if live_test(tr, scr, model, mons):
                model = None
                continue
            return
    finally:
        scr.close()
        tr.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
