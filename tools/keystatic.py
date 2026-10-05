"""Static complement to the key scan: for every function of a .lav that reads
keys (getchar, Inkey, GetWord, CheckKey), the key-like constants it compares
with eqi/nei. Prints one line per function: entry, key reads, constants."""
import os, sys
sys.path.insert(0, os.environ.get("WQX_TL", os.path.expanduser("~/wqx_tl")))
from lavaemu import lav
NAMES = {13: "ENTER", 14: "PGDN", 18: "CAPS", 19: "PGUP", 20: "UP", 21: "DOWN", 22: "RIGHT", 23: "LEFT",
         25: "HELP", 26: "SHIFT", 27: "ESC", 28: "F1", 29: "F2", 30: "F3", 31: "F4", 32: "SPACE"}
KEYISH = set(NAMES) | set(range(ord('a'), ord('z') + 1)) | set(range(ord('0'), ord('9') + 1)) | {ord('.')}
def name(v): return NAMES.get(v) or chr(v)
code = open(sys.argv[1], "rb").read()
funcs, cur, prev = [], None, None
for o in lav.ops(code):
    if o.op == 0xBC and prev is not None and prev.op in (0x01, 0x02) and cur is not None and prev.arg in KEYISH:
        cur[2].append(prev.arg)
    prev = o
    if o.op == 0x3E:
        cur = [o.pc, set(), []]
        funcs.append(cur)
    elif cur is not None:
        if o.op in (0x81, 0x93, 0xC4, 0xBC):
            cur[1].add(lav.NAMES[o.op])
        if o.op in (0x4C, 0x4D) and o.arg in KEYISH:
            cur[2].append(o.arg)
allk = set()
for pc, reads, consts in funcs:
    if reads and consts:
        allk |= set(consts)
        print(f"{pc:05x} {'/'.join(sorted(reads)):24s} {' '.join(name(c) for c in sorted(set(consts)))}")
print("ALL:", " ".join(name(c) for c in sorted(allk)))
