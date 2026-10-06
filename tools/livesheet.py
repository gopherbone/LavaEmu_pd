"""Contact sheet of the live key sets a lockstep --live run saw: the first screen
of each distinct set per game, captioned with the set and what the UI would do."""
import json, glob, os, sys
from PIL import Image, ImageDraw
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BTN = {'Enter', 'Esc', 'Up', 'Down', 'Left', 'Right'}
games = sys.argv[1:] or sorted(f.split('_')[-1][:-5] for f in glob.glob(os.path.join(ROOT, 'host', 'live_*.json')))
for g in games:
    rows = json.load(open(os.path.join(ROOT, 'host', f'live_{g}.json')))
    seen, tiles = set(), []
    for r in rows:
        nb = [k for k in r['keys'] if k not in BTN]
        arrows = r['arrows']
        key = (tuple(r['keys']), r['text'])
        if key in seen or not r['shot'] or not os.path.exists(r['shot']):
            continue
        seen.add(key)
        if r['text']:
            act = 'KEYBOARD'
        elif not nb:
            act = 'profile'
        elif len(nb) > 8:
            act = f'profile ({len(nb)} keys)'
        else:
            act = 'live first'
            if len(nb) <= 2 and not r['open']:
                if 'Y' in nb: nb = ['Y'] + [x for x in nb if x != 'Y']
                if len(nb) == 2 and not arrows: act += f' <{nb[0]} {nb[1]}>'
                if 'Enter' not in r['keys'] and nb[0] == 'Y': act += ' A=Y'
                if 'Esc' not in r['keys'] and 'N' in nb: act += ' B=N'
        cap = (' '.join(r['keys']) if len(r['keys']) <= 12 else f"{len(r['keys'])} keys") + ' | ' + act
        tiles.append((r['label'], cap, Image.open(r['shot']).convert('L')))
    cols = 4
    W, H = 320, 160 + 26
    rowsn = (len(tiles) + cols - 1) // cols
    out = Image.new('L', (cols * (W + 4), max(1, rowsn) * (H + 4)), 255)
    d = ImageDraw.Draw(out)
    for i, (lab, cap, im) in enumerate(tiles):
        x, y = (i % cols) * (W + 4), (i // cols) * (H + 4)
        out.paste(im.resize((320, 160)), (x, y + 26))
        d.text((x + 2, y), lab[:50], fill=0)
        d.text((x + 2, y + 12), cap[:60], fill=0)
    p = os.path.join(ROOT, 'host', f'livesheet_{g}.png')
    out.save(p)
    print(p, len(tiles))
