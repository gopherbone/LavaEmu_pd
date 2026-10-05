"""ctypes binding of the C VM (host/liblava.dylib, `make host`) with state
transfer to and from lavaemu's LavaVM, for the lockstep test and tools."""
from __future__ import annotations

import ctypes as C
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "Source", "fonts")
MEM_LOGICAL = 0x10000 + 16

_lib = None


def lib():
    global _lib
    if _lib is None:
        path = os.path.join(ROOT, "host", "liblava.dylib")
        _lib = C.CDLL(path)
        L = _lib
        vp, u8p, i32p, i64p = C.c_void_p, C.POINTER(C.c_uint8), C.POINTER(C.c_int32), C.POINTER(C.c_int64)
        L.lh_new.restype = vp
        L.lh_new.argtypes = [C.c_char_p, C.c_uint32, C.c_char_p]
        for n in ("lh_free", "lh_run_frame", "lh_clear_files"):
            getattr(L, n).argtypes = [vp]
        L.lh_run_frames.argtypes = [vp, C.c_int]
        L.lh_set_probe.argtypes = [vp, C.c_int]
        L.lh_probes.argtypes = [vp, i32p, C.c_int]
        L.lh_mem.restype = u8p
        L.lh_mem.argtypes = [vp]
        L.lh_stack.restype = i32p
        L.lh_stack.argtypes = [vp]
        L.lh_held.restype = u8p
        L.lh_held.argtypes = [vp]
        L.lh_errmsg.restype = C.c_char_p
        L.lh_errmsg.argtypes = [vp]
        L.lh_get_regs.argtypes = [vp, i64p]
        L.lh_set_regs.argtypes = [vp, i64p]
        L.lh_nfiles.argtypes = [vp]
        L.lh_file_name.restype = C.c_char_p
        L.lh_file_name.argtypes = [vp, C.c_int]
        L.lh_file_len.restype = C.c_uint32
        L.lh_file_len.argtypes = [vp, C.c_int]
        L.lh_file_data.restype = u8p
        L.lh_file_data.argtypes = [vp, C.c_int]
        L.lh_add_file.argtypes = [vp, C.c_char_p, C.c_char_p, C.c_uint32]
        L.lh_handle.argtypes = [vp, C.c_int, i32p, C.POINTER(C.c_char_p), C.POINTER(u8p), C.POINTER(C.c_uint32)]
        L.lh_set_handle.argtypes = [vp, C.c_int, C.c_int, C.c_int, C.c_int, C.c_uint32, C.c_char_p, C.c_char_p,
                                    C.c_uint32]
        L.lh_ndirs.argtypes = [vp]
        L.lh_dir.restype = C.c_char_p
        L.lh_dir.argtypes = [vp, C.c_int]
        L.lh_set_dirs.argtypes = [vp, C.c_char_p]
        L.lh_cwd.restype = C.c_char_p
        L.lh_cwd.argtypes = [vp]
        L.lh_set_cwd.argtypes = [vp, C.c_char_p]
        L.lh_key_down.argtypes = [vp, C.c_int]
        L.lh_key_up.argtypes = [vp, C.c_int]
        L.lh_state_save.restype = C.c_uint32
        L.lh_state_save.argtypes = [vp, C.c_char_p, C.c_uint32]
        L.lh_state_load.argtypes = [vp, C.c_char_p, C.c_uint32]
        L.lh_ops.restype = C.c_uint64
        L.lh_ops.argtypes = [vp]
    return _lib


REGS = ["pc", "last", "fb", "fe", "sp", "us", "frame", "ops", "strp", "xorkey", "ended", "waiting", "error",
        "seed", "fe_max", "latched", "autorelease", "tbig", "trow", "tcol", "files_gen", "last_key"]
NREGS = len(REGS)


def _s32(v: int) -> int:
    return ((v + 0x80000000) & 0xFFFFFFFF) - 0x80000000


def _enc(name: str) -> bytes:
    return name.encode("gb2312", "replace")


class CVM:
    def __init__(self, code: bytes, files: dict | None = None):
        L = lib()
        self.code = bytes(code)
        self.h = L.lh_new(self.code, len(self.code), FONTS.encode())
        if not self.h:
            raise ValueError("not a LAV file")
        self.L = L
        self._mem = L.lh_mem(self.h)
        self._stack = L.lh_stack(self.h)
        self._held = L.lh_held(self.h)
        for k, v in (files or {}).items():
            self.add_file(k, v)

    def __del__(self):
        try:
            self.L.lh_free(self.h)
        except Exception:
            pass

    # -- basic -------------------------------------------------------------
    def run_frame(self):
        self.L.lh_run_frame(self.h)

    def run_frames(self, n):
        self.L.lh_run_frames(self.h, n)

    def mem(self, n: int = MEM_LOGICAL) -> bytes:
        return C.string_at(self._mem, n)

    def set_mem(self, data: bytes) -> None:
        C.memmove(self._mem, data, min(len(data), MEM_LOGICAL))

    def lcd(self) -> bytes:
        return C.string_at(C.addressof(self._mem.contents) + 0x100, 1600)

    def regs(self) -> dict:
        arr = (C.c_int64 * NREGS)()
        self.L.lh_get_regs(self.h, arr)
        return dict(zip(REGS, arr))

    def set_regs(self, r: dict) -> None:
        cur = self.regs()
        cur.update(r)
        arr = (C.c_int64 * NREGS)(*[cur[k] for k in REGS])
        self.L.lh_set_regs(self.h, arr)

    def stack(self) -> list[int]:
        sp = self.regs()["sp"]
        return [self._stack[i] for i in range(sp)]

    def set_stack(self, st: list[int]) -> None:
        for i, v in enumerate(st):
            self._stack[i] = _s32(v)
        self.set_regs({"sp": len(st)})

    def held(self) -> set:
        h = C.string_at(self._held, 128)
        return {i for i in range(128) if h[i]}

    def set_held(self, s: set) -> None:
        for i in range(128):
            self._held[i] = 1 if i in s else 0

    def key_down(self, c: int) -> None:
        self.L.lh_key_down(self.h, c)

    def key_up(self, c: int) -> None:
        self.L.lh_key_up(self.h, c)

    # -- files -------------------------------------------------------------
    def add_file(self, name: str, data: bytes) -> None:
        d = bytes(data)
        self.L.lh_add_file(self.h, _enc(name), d, len(d))

    def files(self) -> dict:
        out = {}
        for i in range(self.L.lh_nfiles(self.h)):
            n = self.L.lh_file_name(self.h, i).decode("gb2312", "replace")
            ln = self.L.lh_file_len(self.h, i)
            out[n] = C.string_at(self.L.lh_file_data(self.h, i), ln) if ln else b""
        return out

    def set_files(self, files: dict) -> None:
        self.L.lh_clear_files(self.h)
        for k, v in files.items():
            self.add_file(k, v)

    def handles(self) -> dict:
        out = {}
        for i in range(3):
            info = (C.c_int32 * 4)()
            name = C.c_char_p()
            data = C.POINTER(C.c_uint8)()
            ln = C.c_uint32()
            if self.L.lh_handle(self.h, i, info, C.byref(name), C.byref(data), C.byref(ln)):
                out[i] = (name.value.decode("gb2312", "replace"), C.string_at(data, ln.value) if ln.value else b"",
                          bool(info[1]), bool(info[2]), info[3])
        return out

    def set_handles(self, fp: dict) -> None:
        for i in range(3):
            f = fp.get(i)
            if f is None:
                self.L.lh_set_handle(self.h, i, 0, 0, 0, 0, b"", b"", 0)
            else:
                d = bytes(f.data)
                self.L.lh_set_handle(self.h, i, 1, int(f.r), int(f.w), f.pos, _enc(f.name), d, len(d))

    def dirs(self) -> set:
        return {self.L.lh_dir(self.h, i).decode("gb2312", "replace") for i in range(self.L.lh_ndirs(self.h))}

    def set_dirs(self, dirs) -> None:
        self.L.lh_set_dirs(self.h, "\n".join(sorted(dirs)).encode("gb2312", "replace"))

    def cwd(self) -> str:
        return self.L.lh_cwd(self.h).decode("gb2312", "replace")

    def set_cwd(self, c: str) -> None:
        self.L.lh_set_cwd(self.h, _enc(c))

    def errmsg(self) -> str:
        return self.L.lh_errmsg(self.h).decode()

    # -- probes ------------------------------------------------------------
    def set_probe(self, on: bool) -> None:
        self.L.lh_set_probe(self.h, 1 if on else 0)

    def probes(self) -> list[tuple[int, int]]:
        buf = (C.c_int32 * 8192)()
        n = self.L.lh_probes(self.h, buf, 4096)
        return [(buf[2 * i], buf[2 * i + 1]) for i in range(n)]

    # -- states ------------------------------------------------------------
    def state_save(self) -> bytes:
        n = self.L.lh_state_save(self.h, None, 0)
        buf = C.create_string_buffer(n)
        m = self.L.lh_state_save(self.h, buf, n)
        return buf.raw[:m]

    def state_load(self, data: bytes) -> bool:
        return bool(self.L.lh_state_load(self.h, data, len(data)))


# ---------------------------------------------------------------------------
# lavaemu <-> C

PY_REGS = ["pc", "last", "fb", "fe", "us", "frame", "ops", "strp", "xorkey", "ended", "waiting", "seed", "fe_max",
           "latched", "autorelease", "tbig", "trow", "tcol"]


def py_regs(vm) -> dict:
    r = {k: int(getattr(vm, k)) for k in PY_REGS}
    r["last"] = _s32(r["last"])
    r["sp"] = len(vm.stack)
    return r


def files_sig(vm):
    return tuple((k, len(v), id(v)) for k, v in vm.files.items())


def sync_inputs(vm, cv: CVM) -> None:
    cv.set_held(vm.held)
    cv.set_regs({"latched": vm.latched, "autorelease": vm.autorelease})


def full_sync(vm, cv: CVM, files: bool = True) -> None:
    cv.set_mem(bytes(vm.mem[:MEM_LOGICAL]))
    r = py_regs(vm)
    r.pop("sp")
    cv.set_regs(r)
    cv.set_stack(vm.stack)
    cv.set_held(vm.held)
    if files:
        cv.set_files(vm.files)
    cv.set_handles(vm.fp)
    cv.set_dirs(vm.dirs)
    cv.set_cwd(vm.cwd)


def diff(vm, cv: CVM, files: bool = True) -> list[str]:
    """Fields where the Python and C VMs differ (empty if equal)."""
    out = []
    pm = bytes(vm.mem[:MEM_LOGICAL])
    cm = cv.mem()
    if pm != cm:
        idx = [i for i in range(MEM_LOGICAL) if pm[i] != cm[i]]
        lcd = [i for i in idx if 0x100 <= i < 0x740]
        out.append(f"mem({len(idx)} bytes, first ${idx[0]:04x} py={pm[idx[0]]:02x} c={cm[idx[0]]:02x}"
                   + (f", LCD {len(lcd)}" if lcd else "") + ")")
    pr = py_regs(vm)
    cr = cv.regs()
    for k in PY_REGS + ["sp"]:
        if pr[k] != cr[k]:
            out.append(f"{k}: py={pr[k]} c={cr[k]}")
    pst = [_s32(v) for v in vm.stack]
    if pst != cv.stack():
        out.append("stack")
    if set(vm.held) != cv.held():
        out.append("held")
    if vm.cwd != cv.cwd():
        out.append("cwd")
    if set(vm.dirs) != cv.dirs():
        out.append(f"dirs py={sorted(vm.dirs)} c={sorted(cv.dirs())}")
    ch = cv.handles()
    ph = {i: (f.name, bytes(f.data), bool(f.r), bool(f.w), f.pos) for i, f in vm.fp.items()}
    if ch != ph:
        out.append(f"handles py={[(i, h[0], len(h[1]), h[2:]) for i, h in ph.items()]} "
                   f"c={[(i, h[0], len(h[1]), h[2:]) for i, h in ch.items()]}")
    if files:
        pf = {k: bytes(v) for k, v in vm.files.items()}
        cf = cv.files()
        if pf != cf:
            bad = sorted(set(pf) ^ set(cf)) + [k for k in pf if k in cf and pf[k] != cf[k]]
            out.append(f"files {bad[:4]}")
    return out
