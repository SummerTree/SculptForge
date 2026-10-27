#!/usr/bin/env python3
# SculptForge - open source digital sculpting
# SPDX-License-Identifier: GPL-3.0-or-later
"""Renders every ARKit facial shape Generate Facial Shapes makes, one tile per shape, into a labelled grid
(docs/guide/images/face-shapes-<head>.jpg, 1400 pixels wide), to look at them all at once.

    python3 tools/face/shape_grid.py --app build/sculptforge --out docs/guide/images [--heads head,bust]

Heads: `head` is the template head (new_head), `bust` the CC0 marble bust in tests/data. Needs ImageMagick
(`magick`). It writes a UI test script per head into a temporary folder, runs the app on it and crops and labels
the screenshots.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "face"))
from build_ict_template import ARKIT  # noqa: E402

HEADS = {
    # name: (commands that load it, camera look-at point, zoom, crop WxH+X+Y of the 1600x960 shot)
    "head": ("run new_head", "0 0.0 0.08", "2.8", "600x700+400+150"),
    "bust": ("run open path=%s" % os.path.join(ROOT, "tests", "data", "marble-bust.glb"), "-0.03 0.335 0.1", "2.8",
             "600x700+400+150"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", default=os.path.join(ROOT, "build", "sculptforge"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "guide", "images"))
    ap.add_argument("--heads", default="head,bust")
    ap.add_argument("--tile", type=int, default=220)
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="sf_face_grid_")
    for head in a.heads.split(","):
        load, look, zoom, crop = HEADS[head]
        lines = [load, "camera 0 0", "key f", "wait 0.5", "look " + look, "zoom " + zoom,
                 "run face_shapes_generate", "run face_preset name=Neutral", "wait 0.2", "shot 00-neutral"]
        for i, name in enumerate(ARKIT):
            # ARKit's mouthClose closes the lips while the jaw is open: on its own it pushes them through each other.
            weights = '{"%s":1%s}' % (name, ',"jawOpen":1' if name == "mouthClose" else "")
            lines += ["run face_preset name=Neutral", "run face_weights json=" + weights, "wait 0.1",
                      "shot %02d-%s" % (i + 1, name)]
        script = os.path.join(work, head + ".sfui")
        with open(script, "w") as f:
            f.write("\n".join(lines) + "\n")
        shots = os.path.join(work, head)
        subprocess.run([a.app, "--ui-test", script, "--ui-out", shots], check=True)
        tiles = []
        for i, name in enumerate(["neutral"] + ARKIT):
            src = os.path.join(shots, "%02d-%s.png" % (i, name))
            tile = os.path.join(work, "%s-%02d.png" % (head, i))
            subprocess.run(["magick", src, "-crop", crop, "+repage", "-resize", "%dx" % a.tile, tile], check=True)
            tiles.append((name, tile))
        os.makedirs(a.out, exist_ok=True)
        out = os.path.join(work, "face-shapes-%s.png" % head)
        cmd = ["magick", "montage"]
        for name, tile in tiles:
            cmd += ["-label", name + (" + jawOpen" if name == "mouthClose" else ""), tile]
        cmd += ["-tile", "9x", "-geometry", "+4+4", "-pointsize", "14", "-background", "#1b1d22", "-fill", "#e8e8e8", out]
        subprocess.run(cmd, check=True)
        jpg = os.path.join(a.out, "face-shapes-%s.jpg" % head)
        subprocess.run(["magick", out, "-resize", "1400x", "-quality", "82", jpg], check=True)
        print("wrote", jpg)


if __name__ == "__main__":
    main()
