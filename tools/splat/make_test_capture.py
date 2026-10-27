#!/usr/bin/env python3
# SculptForge - open source digital sculpting
# SPDX-License-Identifier: GPL-3.0-or-later
"""Renders a tiny synthetic phone capture for splat training tests.

A painted clay "maquette" (a head with ears and a nose on a plinth) is
ray-traced from VIEWS positions on a ring around it, the way someone walks
around an object with a phone. Writes PNG photos and a capture.json in the
phone-link layout (kind, device, files, and the camera pose of every photo as
an OpenGL camera-to-world transform_matrix with fl_x / w / h), so training
needs no COLMAP. The output is generated, so it is CC0 / public domain.

  python3 tools/splat/make_test_capture.py tests/data/splat_capture
  python3 tools/splat/make_test_capture.py big --views 60 --size 640x480

Pure Python (no numpy), a few seconds at the default size.
"""
import argparse
import json
import math
import os
import struct
import zlib


def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a):
    l = math.sqrt(dot(a, a))
    return (a[0] / l, a[1] / l, a[2] / l)


# The maquette: spheres (centre, radius, base colour) and a plinth (a box).
SPHERES = [((0.0, 0.55, 0.0), 0.45, (0.80, 0.52, 0.40)),   # head
           ((-0.42, 0.75, 0.0), 0.14, (0.72, 0.42, 0.33)),  # ears
           ((0.42, 0.75, 0.0), 0.14, (0.72, 0.42, 0.33)),
           ((0.0, 0.52, 0.43), 0.09, (0.86, 0.45, 0.35)),   # nose
           ((-0.16, 0.66, 0.36), 0.06, (0.15, 0.15, 0.18)), # eyes
           ((0.16, 0.66, 0.36), 0.06, (0.15, 0.15, 0.18))]
BOX = ((-0.5, -0.1, -0.5), (0.5, 0.1, 0.5))
LIGHT = norm((0.4, 0.8, 0.5))


def pattern(p):
    # Painted texture (training needs detail to lock on to): soft stripes and dots.
    s = 0.5 + 0.5 * math.sin(p[1] * 22.0) * math.cos(p[0] * 9.0 + p[2] * 7.0)
    return 0.75 + 0.25 * s


def trace(o, d):
    best, hit = 1e9, None
    for c, r, col in SPHERES:
        oc = sub(o, c)
        b = dot(oc, d)
        disc = b * b - (dot(oc, oc) - r * r)
        if disc > 0:
            t = -b - math.sqrt(disc)
            if 1e-4 < t < best:
                p = add(o, mul(d, t))
                best, hit = t, (p, norm(sub(p, c)), col)
    # Box (slab test).
    tmin, tmax, axis = -1e9, 1e9, 0
    for k in range(3):
        if abs(d[k]) < 1e-12:
            if o[k] < BOX[0][k] or o[k] > BOX[1][k]:
                tmin = 1e9
            continue
        t1, t2 = (BOX[0][k] - o[k]) / d[k], (BOX[1][k] - o[k]) / d[k]
        if t1 > t2:
            t1, t2 = t2, t1
        if t1 > tmin:
            tmin, axis = t1, k
        tmax = min(tmax, t2)
    if tmin <= tmax and 1e-4 < tmin < best:
        p = add(o, mul(d, tmin))
        n = [0.0, 0.0, 0.0]
        n[axis] = -1.0 if d[axis] > 0 else 1.0
        checker = (int(math.floor(p[0] * 8)) + int(math.floor(p[2] * 8))) % 2
        best, hit = tmin, (p, tuple(n), (0.35, 0.36, 0.40) if checker else (0.55, 0.56, 0.60))
    return hit


def shade(o, d):
    hit = trace(o, d)
    if not hit:  # a soft studio backdrop
        g = 0.55 + 0.25 * d[1]
        return (g, g, g * 1.04)
    p, n, col = hit
    lit = 0.35 + 0.65 * max(0.0, dot(n, LIGHT))
    if trace(add(p, mul(n, 1e-3)), LIGHT):
        lit = 0.35
    k = pattern(p) * lit
    return (col[0] * k, col[1] * k, col[2] * k)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--views", type=int, default=20)
    ap.add_argument("--size", default="128x96")
    a = ap.parse_args()
    w, h = (int(v) for v in a.size.split("x"))
    os.makedirs(a.out, exist_ok=True)
    fov = math.radians(50.0)
    fl = 0.5 * w / math.tan(0.5 * fov)
    target = (0.0, 0.4, 0.0)
    frames, files = [], []
    for i in range(a.views):
        ang = 2 * math.pi * i / a.views
        height = 0.9 + 0.5 * math.sin(3 * ang)  # walking round, the phone goes up and down a little
        eye = (2.6 * math.sin(ang), height, 2.6 * math.cos(ang))
        f = norm(sub(target, eye))
        r = norm(cross(f, (0.0, 1.0, 0.0)))
        u = cross(r, f)
        rgb = bytearray(w * h * 3)
        for y in range(h):
            for x in range(w):
                px, py = (x + 0.5 - w / 2) / fl, -(y + 0.5 - h / 2) / fl
                d = norm(add(add(f, mul(r, px)), mul(u, py)))
                c = shade(eye, d)
                k = (y * w + x) * 3
                for j in range(3):
                    rgb[k + j] = max(0, min(255, int(round(255 * (c[j] ** (1 / 2.2))))))
        name = "IMG_%04d.png" % i
        png(os.path.join(a.out, name), w, h, rgb)
        files.append(name)
        # OpenGL camera-to-world: columns right, up, -forward, eye (written row by row).
        m = [[r[0], u[0], -f[0], eye[0]], [r[1], u[1], -f[1], eye[1]], [r[2], u[2], -f[2], eye[2]], [0, 0, 0, 1]]
        frames.append({"file": name, "t": i * 500,
                       "transform_matrix": [[round(v, 6) for v in row] for row in m]})
    capture = {"kind": "splat", "device": "SculptForge synthetic capture (tools/splat/make_test_capture.py)",
               "files": files, "fl_x": round(fl, 4), "fl_y": round(fl, 4), "w": w, "h": h, "frames": frames}
    with open(os.path.join(a.out, "capture.json"), "w") as f:
        json.dump(capture, f, indent=1)
    with open(os.path.join(a.out, "LICENSE.txt"), "w") as f:
        f.write("Generated by tools/splat/make_test_capture.py. CC0 1.0 / public domain.\n")
    print("wrote %d views (%dx%d) to %s" % (a.views, w, h, a.out))


if __name__ == "__main__":
    main()
