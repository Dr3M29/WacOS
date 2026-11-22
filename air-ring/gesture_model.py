"""
Personal gesture model: learns YOUR hand from recordings made with trainer.py.

Two small nearest-neighbour classifiers (numpy only, no training time):
  * pose model   - one frame of hand shape -> open / peace / three / rock / thumbs_up / fist / point
  * motion model - the last 0.6 s of movement -> swipe_left/right/up/down, side_tap, or none

The "none" class is recorded from you just moving normally, which is what
teaches it to ignore everyday hand movement.
"""

import json
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
CLIPS = DATA / "clips.jsonl"
MODEL = DATA / "model.npz"
CALIB = DATA / "calibration.json"

WINDOW_S = 0.6          # motion window length
STEPS = 10              # motion window resampled to this many steps
K = 5


def lm_array(lms):
    return np.array([[p.x, p.y, getattr(p, "z", 0.0)] for p in lms], dtype=np.float32)


# ---------------------------------------------------------------- features

def canonical(w):
    """World landmarks rotated into the hand's own frame (wrist origin, palm size 1),
    so the same hand shape gives the same numbers whichever way it faces."""
    w = w - w[0]
    y = w[9]
    s = np.linalg.norm(y) or 1e-6
    y = y / s
    x = w[5] - w[17]
    x = x - y * np.dot(x, y)
    x = x / (np.linalg.norm(x) or 1e-6)
    z = np.cross(x, y)
    return (w @ np.stack([x, y, z]).T) / s


def pose_features(lm, wl):
    c = canonical(wl)[1:].ravel()                       # 60: shape
    w = wl - wl[0]
    up = w[9] / (np.linalg.norm(w[9]) or 1e-6)          # where the hand points (camera frame)
    th = w[4] - w[2]
    th = th / (np.linalg.norm(th) or 1e-6)              # where the thumb points (thumbs up vs fist)
    return np.concatenate([c, up * 2, th * 2]).astype(np.float32)


def side_dist(c):
    """Thumb tip to the side of the index finger (MCP..DIP), canonical units."""
    p, a, b = c[4], c[5], c[7]
    ab = b - a
    t = np.clip(np.dot(p - a, ab) / (np.dot(ab, ab) or 1e-9), 0, 1)
    return float(np.linalg.norm(p - (a + t * ab)))


def motion_features(frames):
    """frames: list of (t, lm(21,3), wl(21,3)) covering ~WINDOW_S. Returns None if too sparse."""
    if len(frames) < 6:
        return None
    t = np.array([f[0] for f in frames])
    if t[-1] - t[0] < WINDOW_S * 0.6:
        return None
    lm = np.stack([f[1] for f in frames])
    palm_px = np.median(np.linalg.norm(lm[:, 0, :2] - lm[:, 9, :2], axis=1)) or 1e-6
    centre = (lm[:, 0, :2] + lm[:, 9, :2]) / 2
    cans = [canonical(f[2]) for f in frames]
    side = np.array([side_dist(c) for c in cans])
    tips = np.array([np.linalg.norm(c[4] - c[8]) for c in cans])
    ts = np.linspace(t[-1] - WINDOW_S, t[-1], STEPS)
    cx = np.interp(ts, t, centre[:, 0])
    cy = np.interp(ts, t, centre[:, 1])
    traj = np.stack([cx - cx[0], cy - cy[0]], 1) / palm_px          # in palm widths
    sd = np.interp(ts, t, side)
    tp = np.interp(ts, t, tips)
    return np.concatenate([traj.ravel(), sd, tp]).astype(np.float32)


# ---------------------------------------------------------------- kNN

POSE_GROUPS = [(slice(0, 60), 1.0), (slice(60, 66), 1.0)]                    # shape, orientation
MOTION_GROUPS = [(slice(0, 20), 1.0), (slice(20, 30), 1.6), (slice(30, 40), 0.6)]  # path, thumb-side, tips


class KNN:
    def __init__(self, X, y, classes, groups=None):
        # Scale each feature GROUP as a whole (not per number), then weight it, so a
        # group's internal shape survives and its importance is what we choose.
        self.mu = X.mean(0)
        self.sd = np.ones(X.shape[1], np.float32)
        for sl, wgt in groups or [(slice(None), 1.0)]:
            self.sd[sl] = (X[:, sl].std() + 1e-3) / wgt
        self.X = (X - self.mu) / self.sd
        self.y = y
        self.classes = list(classes)
        # Reject threshold: how far a sample may be from its nearest training
        # neighbour and still count (95th percentile of leave-one-out distances).
        d = self._dists(self.X)
        np.fill_diagonal(d, np.inf)
        nn = d.min(1)
        self.thr = float(np.percentile(nn, 95) * 1.5) if len(nn) else 1e9

    def _dists(self, Z):
        return np.sqrt(((Z[:, None, :] - self.X[None, :, :]) ** 2).sum(-1))

    def _decide(self, d):
        """Per-class score = mean distance to that class's 3 nearest examples, so a
        class with few examples (a gesture) isn't outvoted by one with hundreds
        ("none"). Confidence comes from how clearly the best class beats the next."""
        scores = np.full(len(self.classes), np.inf)
        for ci in range(len(self.classes)):
            dc = d[self.y == ci]
            if len(dc):
                scores[ci] = np.sort(dc)[:3].mean()
        order = np.argsort(scores)
        best, second = scores[order[0]], scores[order[1]] if len(order) > 1 else np.inf
        ratio = second / max(best, 1e-9)
        conf = float(min(1.0, 0.5 + 1.2 * (ratio - 1)))    # ratio 1.25 -> 0.8
        return int(order[0]), conf

    def predict(self, x):
        z = (x - self.mu) / self.sd
        d = np.sqrt(((self.X - z) ** 2).sum(1))
        if d.min() > self.thr:
            return None, 0.0
        c, conf = self._decide(d)
        return self.classes[c], conf

    def loo_report(self):
        """Leave-one-out accuracy per class + top confusions."""
        d = self._dists(self.X)
        np.fill_diagonal(d, np.inf)
        pred = np.array([self._decide(row)[0] for row in d])
        out = {}
        for ci, c in enumerate(self.classes):
            m = self.y == ci
            if m.any():
                wrong = Counter_like(pred[m][pred[m] != ci], self.classes)
                out[c] = (float((pred[m] == ci).mean()), int(m.sum()), wrong)
        return out


def Counter_like(arr, classes):
    if len(arr) == 0:
        return ""
    v, n = np.unique(arr, return_counts=True)
    j = n.argmax()
    return f"mixed up with {classes[v[j]]} x{n[j]}"


# ---------------------------------------------------------------- dataset

POSES = ["open", "peace", "three", "rock", "thumbs_up", "fist", "point"]
MOTIONS = ["swipe_left", "swipe_right", "swipe_up", "swipe_down", "side_tap"]


def load_clips():
    clips = []
    if CLIPS.exists():
        with open(CLIPS, encoding="utf-8") as f:
            for line in f:
                c = json.loads(line)
                c["frames"] = [(fr[0], np.array(fr[1], np.float32).reshape(21, 3),
                                np.array(fr[2], np.float32).reshape(21, 3)) for fr in c["frames"]]
                clips.append(c)
    return clips


def gesture_centre(clip):
    """Time of the actual gesture inside a recorded clip."""
    fr = clip["frames"]
    t = np.array([f[0] for f in fr])
    if clip["label"] == "side_tap":
        side = np.array([side_dist(canonical(f[2])) for f in fr])
        return float(t[side.argmin()])
    lm = np.stack([f[1] for f in fr])
    c = (lm[:, 0, :2] + lm[:, 9, :2]) / 2
    dt = np.diff(t)
    sp = np.r_[0, np.linalg.norm(np.diff(c, axis=0), axis=1) / np.maximum(dt, 1e-3)]
    sp[1:][dt > 0.12] = 0          # jumps across tracking dropouts aren't real speed
    sp = np.convolve(sp, np.ones(3) / 3, "same")
    return float(t[sp.argmax()])


def window(frames, t_end):
    return [f for f in frames if t_end - WINDOW_S - 0.05 <= f[0] <= t_end]


def build():
    clips = load_clips()
    PX, Py, MX, My = [], [], [], []
    mclasses = MOTIONS + ["none"]
    for c in clips:
        fr = c["frames"]
        if not fr:
            continue
        if c["kind"] == "pose":
            for f in fr[3::2]:                          # skip the first frames (still moving in)
                PX.append(pose_features(f[1], f[2]))
                Py.append(POSES.index(c["label"]))
        elif c["kind"] == "motion":
            tc = gesture_centre(c)
            for shift in (-0.07, 0.0, 0.07):            # small time shifts = more examples
                x = motion_features(window(fr, tc + WINDOW_S / 2 + shift))
                if x is not None:
                    MX.append(x)
                    My.append(mclasses.index(c["label"]))
            # parts of the clip well away from the gesture (getting ready, bringing the
            # hand back) are exactly the movements that should NOT fire
            for te in np.arange(fr[0][0] + WINDOW_S, fr[-1][0], 0.1):
                if abs((te - WINDOW_S / 2) - tc) > 0.55:
                    x = motion_features(window(fr, te))
                    if x is not None:
                        MX.append(x)
                        My.append(mclasses.index("none"))
        elif c["kind"] == "none":
            for te in np.arange(fr[0][0] + WINDOW_S, fr[-1][0], 0.1):
                x = motion_features(window(fr, te))
                if x is not None:
                    MX.append(x)
                    My.append(mclasses.index("none"))
    out = {}
    if PX:
        out.update(pose_X=np.stack(PX), pose_y=np.array(Py))
    if MX:
        out.update(mot_X=np.stack(MX), mot_y=np.array(My))
    DATA.mkdir(exist_ok=True)
    np.savez(MODEL, **out)
    return load()


class Model:
    def __init__(self, pose, motion):
        self.pose_knn, self.motion_knn = pose, motion

    def pose(self, lm, wl):
        if not self.pose_knn:
            return None
        if not isinstance(wl, np.ndarray):
            lm, wl = lm_array(lm), lm_array(wl)
        label, conf = self.pose_knn.predict(pose_features(lm, wl))
        return label if conf >= 0.6 else None

    def motion(self, frames):
        if not self.motion_knn:
            return None, 0.0
        x = motion_features(frames)
        if x is None:
            return None, 0.0
        return self.motion_knn.predict(x)

    def report(self):
        lines = []
        for name, knn in (("hand signs", self.pose_knn), ("motions", self.motion_knn)):
            if not knn:
                continue
            lines.append(f"{name}:")
            for c, (acc, n, wrong) in knn.loo_report().items():
                lines.append(f"  {c:12} {acc * 100:5.0f}%  ({n} samples) {wrong}")
        return lines


def load():
    if not MODEL.exists():
        return None
    z = np.load(MODEL)
    pose = KNN(z["pose_X"], z["pose_y"], POSES, POSE_GROUPS) if "pose_X" in z else None
    motion = KNN(z["mot_X"], z["mot_y"], MOTIONS + ["none"], MOTION_GROUPS) if "mot_X" in z else None
    if not pose and not motion:
        return None
    return Model(pose, motion)


def load_calibration():
    try:
        return json.loads(CALIB.read_text())
    except Exception:
        return {}
