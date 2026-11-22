# Generates Liquid's black-and-white Mac-style lock screen wallpaper (in the spirit of the
# "MacOs M3" serpentine): a glossy dark tube folding back and forth with soft silver light.
#   python tools\make-wallpaper.py            -> wallpapers\lock.png (3840x2160)
import os, sys
import numpy as np
from PIL import Image

W, H = 3840, 2160
if len(sys.argv) > 2:
    W, H = int(sys.argv[1]), int(sys.argv[2])
out = os.path.join(os.path.dirname(__file__), "..", "wallpapers", "lock.png")
os.makedirs(os.path.dirname(out), exist_ok=True)

ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
S = H / 2160.0
pitch = 640 * S          # distance between the horizontal runs
r_tube = 175 * S         # tube half-thickness
turn_r = pitch / 2       # radius of the U-turns
right_x = W * 0.80       # where the tube turns on the right
left_x = W * 0.12        # ...and on the left (only some runs come back on screen)
y0 = H * 0.5 - 1.5 * pitch

# serpentine path: run k goes along y = y0 + k*pitch; turns alternate right / left
runs = 5
d = np.full((H, W), 1e9, np.float32)
ox = np.zeros((H, W), np.float32)   # offset from the nearest point on the centre line
oy = np.zeros((H, W), np.float32)

def take(dist, dx, dy):
    global d, ox, oy
    m = dist < d
    d = np.where(m, dist, d)
    ox = np.where(m, dx, ox)
    oy = np.where(m, dy, oy)

def seg(xa, xb, y):
    cx = np.clip(xs, min(xa, xb), max(xa, xb))
    take(np.hypot(xs - cx, ys - y), xs - cx, ys - y)

def arc(cx, cy, right):
    ang = np.arctan2(ys - cy, xs - cx)
    ok = (np.cos(ang) >= 0) if right else (np.cos(ang) <= 0)
    px, py = cx + np.cos(ang) * turn_r, cy + np.sin(ang) * turn_r
    dist = np.where(ok, np.hypot(xs - px, ys - py), 1e9)
    take(dist, xs - px, ys - py)

for k in range(runs):
    y = y0 + k * pitch
    if k % 2 == 0:
        seg(-W, right_x, y)
        arc(right_x, y + turn_r, True)
    else:
        seg(left_x, right_x, y)
        arc(left_x, y + turn_r, False)

# light comes from below and a little to the right; side = how much this point faces it
L = np.array([0.35, 1.0]); L /= np.linalg.norm(L)
nlen = np.maximum(np.hypot(ox, oy), 1e-3)
side = (ox * L[0] + oy * L[1]) / nlen

t = np.clip(d / r_tube, 0, 4)            # 0 = tube centre, 1 = tube edge
inside = t < 1

# tube body: glossy charcoal, a broad soft highlight toward the light and a crisp rim
lit = np.clip(side, 0, 1)
body = 0.022 + 0.17 * lit * t ** 1.4 + 0.02 * (1 - t) ** 2
rim = np.exp(-((t - 0.965) / 0.035) ** 2) * (0.08 + 0.55 * lit ** 1.5)
tube = body + rim

# soft silver glow spilling out from the lit edge
glow = np.exp(-np.clip(t - 1, 0, None) * 3.2) * (0.005 + 0.11 * lit ** 1.5)
bg = 0.006 + glow * (~inside)

img = np.where(inside, tube, bg)
# vignette + a cool, very slightly blue silver tone
vx = (xs / W - 0.55) ** 2 + (ys / H - 0.45) ** 2
img = img * (1.05 - 0.55 * vx)
img = np.clip(img, 0, 1) ** 0.9
rgb = np.stack([img * 0.97, img * 0.985, img * 1.03], -1)
rgb = (np.clip(rgb, 0, 1) * 255 + np.random.uniform(-0.5, 0.5, rgb.shape)).clip(0, 255).astype(np.uint8)  # dither: no banding
Image.fromarray(rgb, "RGB").save(out, optimize=True)
print("saved", os.path.abspath(out))
