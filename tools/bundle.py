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

# key, folder, title, Chinese title, programs [(file in build, name in bundle, label)], credit, extras.
# pace = virtual us per op: 27 is an NC3000/TC1000 (lavaemu MACHINE_US_PER_OP), 19 a TC800 (Worms'
# own presets), 4 a PC emulator. Games made for the 20K LAVA machines get 27 (at lavaemu's
# default 4 they run several times too fast); Snowman was made for a PC emulator.
GAMES = [
    ("frog", "FrogMonopoly", "Frog Monopoly", "蛙蛙大富翁",
     [("Lava/FrogMonopoly.lav", "FrogMonopoly.lav", "")],
     "version 1.1.2, (c) 2004 Hao Xinli (Computer Frog), DATE Soft Studio; "
     "maps by the authors in its map list.", {"pace": 27}),
    ("ace", "AceAttorney", "Phoenix Wright: Ace Attorney", "逆转裁判",
     [("LAVA/逆转裁判.lav", "AceAttorney.lav", "")],
     "(c) 2004 Ninja Eric, JunctionSoft: a LAVA port of the first case of "
     "Capcom's Ace Attorney (2001).", {"pace": 27}),
    ("newhero", "NewHeroesAltar", "New Heroes' Altar", "新英雄坛说",
     [("LAVA/Hero.lav", "Hero.lav", "")],
     "public test final version, (c) 2007 Fanqinlue, Summer Loft.", {"pace": 27}),
    ("shushan", "HeroesOfMountShu", "Heroes of Mount Shu", "蜀山群侠传",
     [("Lava/ShuRegister.lav", "ShuRegister.lav", "Register an account (first)"),
      ("Lava/ShuHeroes.lav", "ShuHeroes.lav", "")],
     "(c) 2006 Shi Zehuan, FlySoft.", {"pace": 27}),
    ("seal", "SkyLand2", "Sky & Land II: The Sealing Stone", "幕天席地2：封印之石",
     [("SkyLand2.lav", "SkyLand2.lav", "")],
     "version 1.3, (c) 2007 LeeStorm, LastWave team; mini-games by Yoshinhwa.", {"pace": 27}),
    ("skyland", "SkyLand", "Sky & Land", "幕天席地",
     [("SkyLand.lav", "SkyLand.lav", ""), ("SLTool.lav", "SLTool.lav", "Sky & Land Helper (accounts, items)")],
     "version 1.10, (c) 2006 LeeStorm, LastWave team.", {"pace": 27}),
    ("mario", "MarioPipes", "Mario Pipes", "水管马里奥",
     [("MarioPipes.lav", "MarioPipes.lav", "")],
     "version 1.2, (c) 2006 Cloty, Emsky Studio: a Mario Bros. homage (the build for real machines).",
     {"pace": 27}),
    ("fujia", "Millionaire3K", "The Millionaire of 3 Kingdoms", "富甲天下",
     [("Lava/Millionaire3K.lav", "Millionaire3K.lav", "")],
     "(c) 2006 You Shunhang, Hang'Studio.", {"pace": 27}),
    ("sanguo", "ThreeKingdoms", "Three Kingdoms", "三国志",
     [("Lava/三国志.lav", "ThreeKingdoms.lav", "")],
     "(c) 2003 Bsxy and Lee (LeeSoft), art by Mumu and NBADong, LAVA Alliance.", {"pace": 27}),
    ("snowman", "Snowman", "The Story of the Snowman", "雪人传奇",
     [("Lava/Snowman.lav", "Snowman.lav", "")],
     "version 1.0 by Xu Jiajun (made for PC LAVA emulators).", {"pace": 4}),
    ("warcraft", "WarCraft", "WarCraft", "魔兽争霸",
     [("Lava/WarCraft.lav", "WarCraft.lav", "")],
     "version 1.01: code by Fanqinlue, art by Shaofan Daotong. WarCraft is a Blizzard trademark.", {"pace": 27}),
    ("pokemon", "PocketMonsters", "Pocket Monsters Grey", "口袋(灰度版)",
     [("Lava/pokemon.lav", "pokemon.lav", "")],
     "a Gold/Silver-style fan demo (author unnamed). Pokemon is a Nintendo trademark. "
     "Its grey comes from flicker, blended here.", {"pace": 27, "blend": 1}),
    ("school", "HighSchoolLegend", "High School Legend", "中学传奇",
     [("Lava/sch.lav", "sch.lav", "")],
     "version 1.05, (c) 2005 EPC, on FantasyDR's engine.", {"pace": 27}),
    ("jianghu", "Jianghu", "Jianghu", "江湖",
     [("Lava/world.lav", "world.lav", "")],
     "version 0.30, (c) 2005 JPG Studio (FantasyDR, gameboyLV, pheagle, CreamCake).", {"pace": 27}),
    ("worms", "Worms", "Worms", "百战天虫",
     [("Worms.lav", "Worms.lav", "")],
     "version 1.00, 2006, by Xiao Qiang (a LavaX game for the TC800). Worms is a Team17 trademark.",
     {"pace": 19, "data": "games/lava/survey/百战天虫/LavaData"}),
    ("mota", "MagicTower", "Magic Tower", "魔塔",
     [("MagicTower.lav", "MagicTower.lav", "")],
     "the combined edition, (c) 2005 NIpleX software: a port of the Tower of the Sorcerer.", {"pace": 27}),
    ("yongzhe", "LegendOfTheBrave", "Legend of the Brave", "勇者传说",
     [("Lava/Brave.lav", "Brave.lav", "")],
     "by Isword (D.M Studio), 2006; thanks to Wuyan Demeng and Song Fei.", {"pace": 27}),
    ("rushout", "RushOut", "Rush Out the Tunnel", "",
     [("Lava/Rush_Out.lav", "Rush_Out.lav", "")],
     "design by Anson, program by Jay, 2005, www.emsky.net (in English; two typos fixed).", {"pace": 27}),
    ("tetris", "Tetris", "Tetris", "俄罗斯方块",
     [("Tetris.lav", "Tetris.lav", "")],
     "version 1.3, (c) 2006 wqstar028 (Xue Shunjian), SevenStar. Tetris is a trademark of The Tetris Company.",
     {"pace": 27}),
    ("zuanshi", "DiamondBlocks", "Diamond Blocks", "钻石方块",
     [("DiamondBlocks.lav", "DiamondBlocks.lav", "")],
     "version 1.1, (c) 2006 wqstar028 (Xue Shunjian), SevenStar.", {"pace": 27}),
    ("huanying", "PhantomFighter", "Phantom Fighter", "幻影战机",
     [("PhantomFighter.lav", "PhantomFighter.lav", "")],
     "(c) 2007 Zhao Fei, CV soft; testing by Yan Zheng.", {"pace": 27}),
    ("mofa", "MagicBlocks", "Magic Blocks", "魔法方块",
     [("MagicBlocks.lav", "MagicBlocks.lav", "")],
     "(c) 2006 Pan Yufei, tested by Guogai.", {"pace": 27}),
    ("zhuangqiu", "BilliardsMaster", "Billiards Master", "撞球高手",
     [("Lava/BilliardsMaster.lav", "BilliardsMaster.lav", "")],
     "(c) 2005 han_yue.", {"pace": 27}),
    ("huaxue", "PowerSki", "Power Ski", "动力滑雪",
     [("Lava/PowerSki.lav", "PowerSki.lav", "")],
     "(c) 2001 Tiantian Lantian, BLUE-SKY soft; tested by Haikuo Tiankong and Xingxingzhe.", {"pace": 27}),
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
    for key, folder, title, zh, programs, credit, extra in GAMES:
        src = os.path.join(wqx, "work", key, "build")
        if not os.path.isdir(src):
            print(f"skip {key}: no build in {src} (python3 -m {key}.build)")
            continue
        dst = os.path.join(out, folder)
        shutil.rmtree(dst, ignore_errors=True)
        os.makedirs(os.path.join(dst, "LavaData"))
        fields = [("title", title)] + ([("title_gb", gb_hex(zh))] if zh else []) + [("profile", key)]
        for i, (rel, name, label) in enumerate(programs):
            shutil.copy(os.path.join(src, rel), os.path.join(dst, name))
            fields.append(("program", f"{name}|{label}" if label else name))
        data = os.path.join(wqx, extra["data"]) if "data" in extra else os.path.join(src, "LavaData")
        for n in sorted(os.listdir(data)) if os.path.isdir(data) else []:
            shutil.copy(os.path.join(data, n), os.path.join(dst, "LavaData", n))
        fields.append(("pace", str(extra.get("pace", 27))))
        if extra.get("blend"):
            fields.append(("blend", "1"))
        fields.append(("credit", credit + " English fan translation: wqx_tl."))
        write_txt(os.path.join(dst, "game.txt"), fields)
        print(f"{folder}: {', '.join(p[1] for p in programs)} + {len(os.listdir(os.path.join(dst, 'LavaData')))} data files")


def bundle_originals(wqx: str, out: str) -> None:
    sys.path.insert(0, wqx)
    for key, folder, title, zh, programs, credit, extra in GAMES:
        if key not in ORIGINALS:
            continue
        sub, progs, data = ORIGINALS[key]
        src = os.path.join(wqx, "games", "lava", sub)
        dst = os.path.join(out, folder + "-zh")
        shutil.rmtree(dst, ignore_errors=True)
        os.makedirs(os.path.join(dst, "LavaData"))
        fields = [("title", title + " (Chinese)"), ("title_gb", gb_hex(zh)), ("profile", key),
                  ("pace", str(extra.get("pace", 27)))]
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
            if key == "mario":
                continue    # mario's build_all is driven by its qa
            subprocess.run([sys.executable, "-m", f"{key}.build"], cwd=a.wqx_tl, check=True)
    os.makedirs(a.out, exist_ok=True)
    bundle_english(a.wqx_tl, a.out)
    if a.originals:
        bundle_originals(a.wqx_tl, a.out)


if __name__ == "__main__":
    main(sys.argv[1:])
