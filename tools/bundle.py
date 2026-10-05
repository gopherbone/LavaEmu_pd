"""Fill games/ (gitignored; the Makefile copies it into the pdx as Bundled/)
with the English LAVA games built by wqx_tl, each in its own folder:

    games/<Folder>/game.txt        title, programs, key profile, credit
    games/<Folder>/<Program>.lav
    games/<Folder>/LavaData/*.dat

    python3 tools/bundle.py [--wqx-tl ~/wqx_tl] [--originals] [--build] games

--build rebuilds each game first (python3 -m <key>.build in wqx_tl).
--originals also bundles the Chinese originals from wqx_tl/games/lava (for
testing the VM's own fonts); they are not part of a release.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

GAMES = [
    # key, folder, title, Chinese title, programs [(file in build, name in bundle, label)], credit
    ("frog", "FrogMonopoly", "Frog Monopoly", "蛙蛙大富翁",
     [("Lava/FrogMonopoly.lav", "FrogMonopoly.lav", "")],
     "version 1.1.2, (c) 2004 Hao Xinli (Computer Frog), DATE Soft Studio; "
     "maps by the authors in its map list."),
    ("ace", "AceAttorney", "Phoenix Wright: Ace Attorney", "逆转裁判",
     [("LAVA/逆转裁判.lav", "AceAttorney.lav", "")],
     "(c) 2004 Ninja Eric, JunctionSoft: a LAVA port of the first case of "
     "Capcom's Ace Attorney (2001)."),
    ("newhero", "NewHeroesAltar", "New Heroes' Altar", "新英雄坛说",
     [("LAVA/Hero.lav", "Hero.lav", "")],
     "public test final version, (c) 2007 Fanqinlue, Summer Loft."),
    ("shushan", "HeroesOfMountShu", "Heroes of Mount Shu", "蜀山群侠传",
     [("Lava/ShuHeroes.lav", "ShuHeroes.lav", ""),
      ("Lava/ShuRegister.lav", "ShuRegister.lav", "Register an account (first)")],
     "(c) 2006 Shi Zehuan, FlySoft."),
    ("seal", "SkyLand2", "Sky & Land II: The Sealing Stone", "幕天席地2：封印之石",
     [("SkyLand2.lav", "SkyLand2.lav", "")],
     "version 1.3, (c) 2007 LeeStorm, Molang Team; mini-games by Yoshinhwa."),
]

# The Chinese originals: key -> (folder in games/lava, [(program, label)], data files)
ORIGINALS = {
    "frog": ("wawarich", [("蛙蛙大富翁.lav", "")], ["RichMap.dat", "RichPic.dat"]),
    "ace": ("逆转裁判/《逆转裁判》 忍者Eric制作", None, None),     # a .pac
    "newhero": ("新英雄坛说(公测最终版)/NewHero/分开的文件", [("Hero.lav", "")], ["Hero.dat", "HeroSkin.dat"]),
    "shushan": ("蜀山群侠传/RPG游戏《蜀山群侠传》游戏文件", [("蜀山群侠传.lav", ""), ("蜀山注册.lav", "注册")],
                ["SM-Map.dat", "SM-Pic.dat"]),
    "seal": ("幕天席地2：封印之石1.3/幕天席地2：封印之石1.3", [("幕天席地2.lav", "")], ["SLmaps.dat", "SLpics.dat"]),
}


def gb_hex(s: str) -> str:
    return s.encode("gb2312").hex()


def write_txt(path: str, fields: list[tuple[str, str]]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        for k, v in fields:
            f.write(f"{k}={v}\n")


def bundle_english(wqx: str, out: str) -> None:
    for key, folder, title, zh, programs, credit in GAMES:
        src = os.path.join(wqx, "work", key, "build")
        if not os.path.isdir(src):
            print(f"skip {key}: no build in {src} (python3 -m {key}.build)")
            continue
        dst = os.path.join(out, folder)
        shutil.rmtree(dst, ignore_errors=True)
        os.makedirs(os.path.join(dst, "LavaData"))
        fields = [("title", title), ("title_gb", gb_hex(zh)), ("profile", key)]
        for i, (rel, name, label) in enumerate(programs):
            shutil.copy(os.path.join(src, rel), os.path.join(dst, name))
            fields.append(("program", f"{name}|{label}" if label else name))
        for n in sorted(os.listdir(os.path.join(src, "LavaData"))):
            shutil.copy(os.path.join(src, "LavaData", n), os.path.join(dst, "LavaData", n))
        fields.append(("credit", credit + " English fan translation: wqx_tl."))
        write_txt(os.path.join(dst, "game.txt"), fields)
        print(f"{folder}: {', '.join(p[1] for p in programs)} + {len(os.listdir(os.path.join(dst, 'LavaData')))} data files")


def bundle_originals(wqx: str, out: str) -> None:
    sys.path.insert(0, wqx)
    for key, folder, title, zh, programs, credit in GAMES:
        sub, progs, data = ORIGINALS[key]
        src = os.path.join(wqx, "games", "lava", sub)
        dst = os.path.join(out, folder + "-zh")
        shutil.rmtree(dst, ignore_errors=True)
        os.makedirs(os.path.join(dst, "LavaData"))
        fields = [("title", title + " (Chinese)"), ("title_gb", gb_hex(zh)), ("profile", key)]
        if progs is None:
            from lavaemu import pac
            pacfile = next(n for n in os.listdir(src) if n.endswith(".pac"))
            for name, body in pac.read(open(os.path.join(src, pacfile), "rb").read()):
                if name.lower().endswith(".lav"):
                    open(os.path.join(dst, folder + ".lav"), "wb").write(body)
                    fields.append(("program", folder + ".lav"))
                else:
                    open(os.path.join(dst, "LavaData", os.path.basename(name)), "wb").write(body)
        else:
            for i, (p, label) in enumerate(progs):
                name = f"{folder}{i or ''}.lav"
                shutil.copy(os.path.join(src, p), os.path.join(dst, name))
                fields.append(("program", f"{name}|{label}" if label else name))
            for d in data:
                shutil.copy(os.path.join(src, d), os.path.join(dst, "LavaData", d))
        fields.append(("credit", credit))
        write_txt(os.path.join(dst, "game.txt"), fields)
        print(f"{folder}-zh: original")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--wqx-tl", default=os.path.expanduser("~/wqx_tl"))
    ap.add_argument("--originals", action="store_true")
    ap.add_argument("--build", action="store_true")
    a = ap.parse_args(argv)
    if a.build:
        for key, *_ in GAMES:
            subprocess.run([sys.executable, "-m", f"{key}.build"], cwd=a.wqx_tl, check=True)
    os.makedirs(a.out, exist_ok=True)
    bundle_english(a.wqx_tl, a.out)
    if a.originals:
        bundle_originals(a.wqx_tl, a.out)


if __name__ == "__main__":
    main(sys.argv[1:])
