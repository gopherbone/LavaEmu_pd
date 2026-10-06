"""Save-state round trip on the C VM: run a game, save a state, keep playing;
load the state into a fresh VM, replay the same keys, and compare every frame.
Also checks that game saves written through fclose survive in the state."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lavac  # noqa: E402

GAMES = os.path.join(lavac.ROOT, "games")


def load(folder):
    d = os.path.join(GAMES, folder)
    txt = dict(l.split("=", 1) for l in open(os.path.join(d, "game.txt"), encoding="utf-8").read().splitlines() if "=" in l)
    prog = [l.split("=", 1)[1] for l in open(os.path.join(d, "game.txt"), encoding="utf-8").read().splitlines()
            if l.startswith("program=")][-1].split("|")[0]
    pace = int(txt.get("pace", 4))
    files = {}
    for n in os.listdir(os.path.join(d, "LavaData")):
        files["/LavaData/" + n] = open(os.path.join(d, "LavaData", n), "rb").read()
    return open(os.path.join(d, prog), "rb").read(), files, pace


def script(n):
    # a fixed pseudo-random key sequence: (frame, key, down)
    keys = [13, 13, 21, 13, 20, 22, 23, 27, 13, 13, 21, 21, 13]
    ev = []
    for i in range(n):
        f = 30 + i * 25
        k = keys[i % len(keys)]
        ev += [(f, k, True), (f + 6, k, False)]
    return ev


def run(vm, events, start, end, lcds=None):
    for f in range(start, end):
        for ef, k, down in events:
            if ef == f:
                (vm.key_down if down else vm.key_up)(k)
        vm.run_frame()
        if lcds is not None:
            lcds.append(vm.lcd())


def main():
    ok = True
    for folder in ("FrogMonopoly", "SkyLand2", "NewHeroesAltar", "AceAttorney", "Worms", "PocketMonsters", "MarioPipes",
                   "PowerSki", "PhantomFighter", "BilliardsMaster"):
        if not os.path.isdir(os.path.join(GAMES, folder)):
            print(f"skip {folder} (make games)")
            continue
        code, files, pace = load(folder)
        ev = script(60)
        a = lavac.CVM(code, files, pace)
        run(a, ev, 0, 900)
        st = a.state_save()
        la = []
        run(a, ev, 900, 1600, la)
        b = lavac.CVM(code, files, pace)
        if not b.state_load(st):
            print(f"{folder}: state didn't load")
            ok = False
            continue
        lb = []
        run(b, ev, 900, 1600, lb)
        same = la == lb and a.mem() == b.mem() and a.regs()["pc"] == b.regs()["pc"] and a.px() == b.px()
        print(f"{folder}: state {len(st)} bytes, replay after load {'matches' if same else 'DIFFERS'}")
        ok &= same
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
