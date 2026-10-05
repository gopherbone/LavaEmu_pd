"""Lockstep test: every frame lavaemu runs (the reference VM in ~/wqx_tl), the
C VM runs too, and the two are compared: all of memory (so the LCD and the
off-screen buffer), registers, the stack, keys, open files and the file
table.

The frames come from each translated game's own QA routes (frog, ace,
newhero, shushan, seal), on both the English build and the Chinese original,
driven by the games' own qa.py modules. Those drivers sometimes reach into
the VM between frames (cheats that poke memory, snapshots restored, keys
pressed): the harness notices a state the C VM didn't produce and copies it
across ("syncs"). Some also run Python callbacks inside a frame (breakpoints,
system-call hooks); when one of those changes the VM's state the frame can't
be compared and is counted as "tainted" instead.

    python3 tests/lockstep.py [GAME ...] [--routes a,b] [--max-div N]

Needs ~/wqx_tl (or WQX_TL=path) and `make host`. Exit status 1 on any divergence.
Divergent frames are saved to host/lockstep/<game>-<n>.pkl for
tests/stepdiff.py, which finds the first instruction that differs.
"""
from __future__ import annotations

import os
import pickle
import sys
import tempfile
import time
import weakref
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lavac  # noqa: E402

WQX = os.environ.get("WQX_TL", os.path.expanduser("~/wqx_tl"))
sys.path.insert(0, WQX)
os.chdir(WQX)

from lavaemu import vm as lvm  # noqa: E402

OUT = os.path.join(lavac.ROOT, "host", "lockstep")
os.makedirs(OUT, exist_ok=True)

twins: "weakref.WeakKeyDictionary" = weakref.WeakKeyDictionary()
files_seen: "weakref.WeakKeyDictionary" = weakref.WeakKeyDictionary()


class Stats:
    def __init__(self):
        self.frames = self.syncs = self.tainted = self.vms = 0
        self.divs = []
        self.label = ""
        self.max_div = 5
        self.keys = None          # --keys: {game: {(kind, value): count}}


S = Stats()


def fingerprint(vm):
    return (zlib.crc32(vm.mem), vm.pc, vm.last, vm.fb, vm.fe, len(vm.stack), tuple(vm.stack[-8:]),
            frozenset(vm.held), vm.latched, vm.autorelease, vm.us, vm.seed, vm.strp)


class Watch:
    """Wraps a frame's Python callbacks and notes whether any changed the VM."""

    def __init__(self, vm):
        self.vm = vm
        self.tainted = False
        self.saved_tc = vm.trace_calls
        self.wrapped_tc = None
        if vm.trace_calls:
            self.wrapped_tc = self.wrap(vm.trace_calls, sys_hook=True)
            vm.trace_calls = self.wrapped_tc
        self.bps = dict(vm.breakpoints)
        for pc, cb in self.bps.items():
            vm.breakpoints[pc] = self.wrap(cb)

    def wrap(self, cb, sys_hook=False):
        def w(*args):
            before = fingerprint(self.vm)
            r = cb(*args)
            if fingerprint(self.vm) != before or r is False:
                self.tainted = True
            return r
        w._orig = cb
        return w

    def done(self):
        vm = self.vm
        if vm.trace_calls is self.wrapped_tc:
            vm.trace_calls = self.saved_tc
        for pc in list(vm.breakpoints):
            cb = vm.breakpoints[pc]
            if getattr(cb, "_orig", None) is not None:
                vm.breakpoints[pc] = cb._orig
        if vm.breakpoints.keys() != self.bps.keys():
            self.tainted = True


def light_state(vm):
    return dict(mem=bytes(vm.mem), stack=list(vm.stack), held=set(vm.held),
                regs={k: getattr(vm, k) for k in lavac.PY_REGS},
                files={k: bytes(v) for k, v in vm.files.items()} if vm.fp or True else None,
                fp={i: (f.name, bytes(f.data), f.r, f.w, f.pos) for i, f in vm.fp.items()},
                dirs=set(vm.dirs), cwd=vm.cwd, code=vm.code)


_orig_run_frames = lvm.LavaVM.run_frames


def run_frames(self, n=1):
    for _ in range(n):
        cv = twins.get(self)
        if cv is None:
            cv = lavac.CVM(self.code)
            if S.keys is not None:
                cv.set_probe(True)
            twins[self] = cv
            files_seen[self] = None
            S.vms += 1
        if not self.ended:
            self._apply_pending()
        # copy anything changed outside a frame (pokes, restores, keys)
        lavac.sync_inputs(self, cv)
        sig = lavac.files_sig(self)
        check_files = sig != files_seen.get(self)
        if lavac.diff(self, cv, files=check_files):
            lavac.full_sync(self, cv, files=check_files)
            S.syncs += 1
        files_seen[self] = sig
        pre = None
        if len(S.divs) < S.max_div:
            pre = (self.files.copy(), bytes(self.mem), list(self.stack), set(self.held),
                   {k: getattr(self, k) for k in lavac.PY_REGS},
                   {i: (f.name, bytes(f.data), f.r, f.w, f.pos) for i, f in self.fp.items()},
                   set(self.dirs), self.cwd)
        gen0 = cv.regs()["files_gen"]
        w = Watch(self)
        try:
            _orig_run_frames(self, 1)
        finally:
            w.done()
        cv.run_frame()
        S.frames += 1
        if S.keys is not None:
            g = S.keys.setdefault(S.label.split()[0], {})
            for kind, v in cv.probes():
                g.setdefault((kind, v), set()).add(S.label.split()[1])
        sig = lavac.files_sig(self)
        check_files = sig != files_seen.get(self) or cv.regs()["files_gen"] != gen0
        d = lavac.diff(self, cv, files=check_files)
        files_seen[self] = sig
        if d:
            if w.tainted:
                S.tainted += 1
            else:
                k = len(S.divs)
                S.divs.append((S.label, self.frame, d))
                print(f"  DIVERGENCE {S.label} frame {self.frame}: {'; '.join(d)[:400]}", flush=True)
                if pre is not None:
                    files, mem, stack, held, regs, fp, dirs, cwd = pre
                    blob = dict(code=self.code, files={k2: bytes(v) for k2, v in files.items()}, mem=mem,
                                stack=stack, held=held, regs=regs, fp=fp, dirs=dirs, cwd=cwd, label=S.label,
                                frame=self.frame, diff=d)
                    with open(os.path.join(OUT, f"{S.label.split()[0]}-{k}.pkl"), "wb") as f:
                        pickle.dump(blob, f)
            lavac.full_sync(self, cv)
            files_seen[self] = lavac.files_sig(self)


lvm.LavaVM.run_frames = run_frames


# ---------------------------------------------------------------------------
# Game drivers

def scratch(game):
    d = os.path.join(tempfile.gettempdir(), "lava_lockstep", game)
    os.makedirs(d, exist_ok=True)
    return d


def route_names(game):
    d = os.path.join(WQX, "docs", game, "routes")
    return sorted(n[:-5] for n in os.listdir(d) if n.endswith(".json"))


def run_game(game, only=None):
    import importlib
    import io
    import contextlib
    qa = importlib.import_module(f"{game}.qa")
    qa.QA = scratch(game)
    if game == "ace":
        names = only or list(qa.ROUTES)
        out = qa.build.build("en", out_dir=None, quiet=True)
        for n in names:
            for english in (True, False):
                S.label = f"{game} {n} {'en' if english else 'zh'}"
                yield S.label, (lambda n=n, english=english: qa.run_route(n, english, False, out if english else None))
    elif game == "newhero":
        for n in only or route_names(game):
            S.label = f"{game} {n}"
            yield S.label, (lambda n=n: qa.routes([n]))
    else:
        for n in only or route_names(game):
            S.label = f"{game} {n}"
            yield S.label, (lambda n=n: qa.run_route(n))


def main(argv):
    games = [a for a in argv if not a.startswith("--") and a in ("frog", "ace", "newhero", "shushan", "seal")]
    only = None
    if "--routes" in argv:
        only = argv[argv.index("--routes") + 1].split(",")
    if "--max-div" in argv:
        S.max_div = int(argv[argv.index("--max-div") + 1])
    games = games or ["frog", "ace", "newhero", "shushan", "seal"]
    if "--keys" in argv:
        S.keys = {}
    import contextlib
    import io
    total_fail = 0
    for g in games:
        for label, fn in run_game(g, only):
            S.label = label
            f0, s0, t0, d0 = S.frames, S.syncs, S.tainted, len(S.divs)
            t = time.time()
            buf = io.StringIO()
            try:
                with contextlib.redirect_stdout(buf):
                    fn()
                err = ""
            except Exception as e:  # a route that fails on lavaemu itself is reported, not counted
                err = f" (route error: {type(e).__name__}: {e})"
            print(f"{label:34s} frames {S.frames - f0:7d}  syncs {S.syncs - s0:5d}  tainted {S.tainted - t0:4d}  "
                  f"divergent {len(S.divs) - d0:3d}  {time.time() - t:6.1f}s{err}", flush=True)
            total_fail += len(S.divs) - d0
    if S.keys is not None:
        import json
        for g, d in S.keys.items():
            rows = sorted(([k, v, sorted(r)] for (k, v), r in d.items()), key=lambda x: (x[0], x[1]))
            with open(os.path.join(lavac.ROOT, "host", f"keys_{g}.json"), "w") as f:
                json.dump(rows, f)
    print(f"TOTAL frames {S.frames}  VMs {S.vms}  syncs {S.syncs}  tainted {S.tainted}  divergent {len(S.divs)}")
    return 1 if S.divs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
