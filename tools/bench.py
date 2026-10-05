"""Host benchmark of the C VM (host/liblava.dylib): each bundled English game
for 3600 frames (one virtual minute) with Enter pressed every 2 s, then the
busiest case, a program spinning for a whole frame. Prints ops per frame
(the VM is time-budgeted: at most 4,166 ops per 1/60 s frame) and host ns per op."""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests"))
import lavac
import test_states as T

def bench(folder, prog=None):
    code, files = T.load(folder)
    if prog:
        code = open(os.path.join(T.GAMES, folder, prog), "rb").read()
    vm = lavac.CVM(code, files)
    t0 = time.perf_counter()
    ops0 = vm.regs()["ops"]
    worst = 0
    for f in range(3600):
        if f % 120 == 60: vm.key_down(13)
        if f % 120 == 66: vm.key_up(13)
        o = vm.regs()["ops"]
        vm.run_frame()
        worst = max(worst, vm.regs()["ops"] - o)
    dt = time.perf_counter() - t0
    ops = vm.regs()["ops"] - ops0
    return ops, worst, dt

print(f"{'game':28s} {'ops/frame avg':>14s} {'max':>6s} {'host ns/op':>10s} {'host us/frame':>13s}")
for folder, prog in [("FrogMonopoly", None), ("AceAttorney", None), ("NewHeroesAltar", None),
                     ("HeroesOfMountShu", "ShuHeroes.lav"), ("HeroesOfMountShu", "ShuRegister.lav"), ("SkyLand2", None)]:
    ops, worst, dt = bench(folder, prog)
    # time the VM alone on a full-budget workload for ns/op: rerun with timing only
    print(f"{folder + ('/' + prog if prog else ''):28s} {ops / 3600:14.0f} {worst:6d} {dt * 1e9 / max(ops, 1):10.1f} {dt * 1e6 / 3600:13.1f}")
