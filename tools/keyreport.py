"""Summarise host/keys_<game>.json (from `tests/lockstep.py --keys`): which
key codes each game compares a key it read against, or polls with CheckKey."""
import json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAMES = {13: "ENTER", 14: "PGDN", 18: "CAPS", 19: "PGUP", 20: "UP", 21: "DOWN", 22: "RIGHT", 23: "LEFT",
         25: "HELP", 26: "SHIFT", 27: "ESC", 28: "F1", 29: "F2", 30: "F3", 31: "F4", 32: "SPACE"}
def name(v):
    if v in NAMES: return NAMES[v]
    if 33 <= v < 127: return repr(chr(v))
    return str(v)
for g in sys.argv[1:] or ["frog", "ace", "newhero", "shushan", "seal"]:
    p = os.path.join(ROOT, "host", f"keys_{g}.json")
    if not os.path.exists(p): continue
    rows = json.load(open(p))
    print(f"== {g}")
    for kind, label in ((1, "CheckKey"), (2, "compared =="), (3, "range")):
        vs = [(v, r) for k, v, r in rows if k == kind and (kind == 1 or 0 < v < 128)]
        if vs:
            print(f"  {label}: " + ", ".join(f"{name(v)}[{len(r)}]" for v, r in vs))
