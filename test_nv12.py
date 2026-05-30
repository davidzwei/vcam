#!/usr/bin/env python3
"""
test_nv12.py - Verify NV12 output from vcam.

Writes solid RGB24 colors to the framebuffer input and verifies
the captured NV12 frame has the expected Y/U/V values.

Requirements:
  - vcam module loaded with allow_pix_conversion=1
    e.g.: sudo insmod vcam.ko allow_pix_conversion=1
  - ffmpeg installed
  - Run as root or with /dev/fb* write permission
"""

import os
import subprocess
import sys
import tempfile

W, H = 640, 480


def find_devices():
    """Return (fb_path, video_path) for the first vcam device."""
    try:
        out = subprocess.check_output(["./vcam-util", "-l"],
                                      text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        sys.exit("ERROR: vcam-util -l failed — is the module loaded?")

    for line in out.splitlines():
        if "->" not in line:
            continue
        left, right = line.split("->", 1)
        fb_name = left.strip().split()[1].split("(")[0]
        vid = right.strip()
        fb = fb_name if fb_name.startswith("/") else f"/dev/{fb_name}"
        return fb, vid

    sys.exit("ERROR: no vcam device found")


def nv12_supported(video):
    """Return True if the device lists NV12 in ENUM_FMT."""
    out = subprocess.check_output(
        ["v4l2-ctl", f"--device={video}", "--list-formats"],
        text=True, stderr=subprocess.DEVNULL,
    )
    return "NV12" in out.upper()


def rgb_to_yuv(r, g, b):
    """Mirror the kernel's rgb24_to_nv12 conversion (rgb_to_y/u/v)."""
    y = ((66 * r + 129 * g + 25 * b) >> 8) + 16
    u = ((-38 * r - 74 * g + 112 * b) >> 8) + 128
    v = ((112 * r - 94 * g - 18 * b) >> 8) + 128
    return (max(16, min(235, y)),
            max(16, min(240, u)),
            max(16, min(240, v)))


def capture_nv12(video, fb_file, r, g, b):
    """Write solid color, capture one NV12 frame, return raw bytes."""
    pixel = bytes([r, g, b]) * (W * H)
    fb_file.write(pixel)
    fb_file.flush()

    with tempfile.NamedTemporaryFile(suffix=".nv12", delete=False) as tmp:
        path = tmp.name
    try:
        subprocess.run(
            ["ffmpeg", "-y",
             "-f", "v4l2", "-input_format", "nv12",
             "-video_size", f"{W}x{H}",
             "-i", video,
             "-frames:v", "1",
             "-f", "rawvideo", "-pix_fmt", "nv12", path],
            capture_output=True, check=True,
        )
        with open(path, "rb") as f:
            return f.read()
    finally:
        os.unlink(path)


def sample(data, x, y):
    """Read (Y, U, V) for pixel (x, y) from NV12 frame."""
    Y = data[y * W + x]
    uv = W * H + (y // 2) * W + (x // 2) * 2
    return Y, data[uv], data[uv + 1]


def check(label, data, x, y, r, g, b, tol=3):
    ey, eu, ev = rgb_to_yuv(r, g, b)
    ay, au, av = sample(data, x, y)
    ok = (abs(ay - ey) <= tol and
          abs(au - eu) <= tol and
          abs(av - ev) <= tol)
    status = "PASS" if ok else "FAIL"
    print(f"  [{status}] {label:20s}  "
          f"Y {ay:3d}(exp {ey:3d})  "
          f"U {au:3d}(exp {eu:3d})  "
          f"V {av:3d}(exp {ev:3d})")
    return ok


# ── test cases: (label, R, G, B) ──────────────────────────────────────────────
CASES = [
    ("red",    255,   0,   0),
    ("green",    0, 255,   0),
    ("blue",     0,   0, 255),
    ("white",  255, 255, 255),
    ("black",    0,   0,   0),
    ("gray",   128, 128, 128),
]


def main():
    fb, video = find_devices()
    print(f"fb: {fb}   video: {video}")

    if not nv12_supported(video):
        sys.exit(
            "NV12 not listed in ENUM_FMT.\n"
            "Reload module with: sudo insmod vcam.ko allow_pix_conversion=1"
        )

    passed = 0
    with open(fb, "wb") as fb_file:
        for label, r, g, b in CASES:
            try:
                data = capture_nv12(video, fb_file, r, g, b)
            except subprocess.CalledProcessError as e:
                print(f"  [FAIL] {label}: ffmpeg error\n{e.stderr.decode()}")
                continue

            if len(data) != W * H * 3 // 2:
                print(f"  [FAIL] {label}: unexpected frame size {len(data)}")
                continue

            cx, cy = W // 2, H // 2
            if check(label, data, cx, cy, r, g, b):
                passed += 1

    total = len(CASES)
    print(f"\n{passed}/{total} passed")
    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()