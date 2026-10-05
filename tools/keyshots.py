"""What does each key do? From a mid-game state (reached with the game's own
QA driver in ~/wqx_tl), press each candidate key and screenshot the result:
build/keyshots/<game>.png, one tile per key, before/after.

    python3 tools/keyshots.py frog|seal|shushan|newhero|ace
"""
from __future__ import annotations

import json
import os
import sys

from PIL import Image, ImageDraw

WQX = os.environ.get("WQX_TL", os.path.expanduser("~/wqx_tl"))
sys.path.insert(0, WQX)
os.chdir(WQX)
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "keyshots")
os.makedirs(OUT, exist_ok=True)


def img(vm):
    px = vm.screen.pixels()
    im = Image.new("L", (160, 80))
    im.putdata([0x24 if p else 0xC8 for row in px for p in row])
    return im.resize((320, 160), Image.NEAREST)


def sheet(game, tiles):
    cols = 3
    w, h = 320, 176
    rows = (len(tiles) + cols - 1) // cols
    out = Image.new("L", (cols * (w + 6), rows * (h + 6)), 255)
    d = ImageDraw.Draw(out)
    for i, (label, im) in enumerate(tiles):
        x, y = (i % cols) * (w + 6), (i // cols) * (h + 6)
        out.paste(im, (x, y + 16))
        d.text((x + 2, y + 2), label, fill=0)
    p = os.path.join(OUT, f"{game}.png")
    out.save(p)
    print(p)


def probe(game, g, keys, step, settle=None):
    vm = g.vm
    tiles = [("start", img(vm))]
    base = vm.snapshot()
    for k in keys:
        vm.restore(base)
        try:
            step(k)
            if settle:
                settle()
        except Exception as e:  # noqa
            tiles.append((f"{k}: {e}", img(vm)))
            continue
        tiles.append((k, img(vm)))
    vm.restore(base)
    sheet(game, tiles)


def route(game, name):
    d = json.load(open(os.path.join(WQX, "docs", game, "routes", name + ".json")))
    return d


def main(game, extra=None):
    if game == "frog":
        from frog import qa
        g = qa.Game("en")
        for st in route("frog", "ingame")[:15]:
            if not st.startswith("@"):
                g.step(st)
        keys = extra or ["F2", "HELP", "PGUP", "PGDN", "p", "q", "w", "x", "z", "s", "r", "h", "d", "CAPS", "SHIFT",
                         "ESC", "y", "n"]
        probe(game, g, keys, g.step)
    elif game == "seal":
        from seal import qa
        g = qa.Game("en")
        steps = route("seal", "menus")
        for st in steps[:steps.index("@14-village")]:
            if not st.startswith("@"):
                g.step(st)
        keys = extra or ["F1", "F2", "CAPS", "SHIFT", "SPACE", "b", "m", "n", "d", "o", "p", "a", "y", "1", "PGUP",
                         "ESC", "ENTER"]
        probe(game, g, keys, g.step)
    elif game == "shushan":
        from shushan import qa
        r = route("shushan", "menus")
        g = qa.Game("en", r["start"])
        for st in r["steps"][:r["steps"].index("@02_map")]:
            if not st.startswith("@"):
                g.step(st)
        keys = extra or ["F1", "F2", "F3", "F4", "p", "CAPS", "SHIFT", "ESC", "ENTER", "y", "n", "3", "5"]
        probe(game, g, keys, g.step)
    elif game == "newhero":
        from newhero import qa
        g = qa.at_map("en")
        keys = extra or ["ESC", "ENTER", "b", "c", "d", "f", "h", "k", "n", "r", "y", "F1", "F2", "F3", "F4", "HELP",
                         "PGUP", "PGDN", "SPACE"]

        def step(k):
            g.key(k)
            g.vm.run_frames(40)
        probe(game, g, keys, step)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2].split(",") if len(sys.argv) > 2 else None)
