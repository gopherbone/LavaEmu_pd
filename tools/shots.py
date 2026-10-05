"""Copy the Simulator autotest's PBM screenshots to PNGs (default: host/shots/),
and make a contact sheet."""
import os, sys
from PIL import Image
SRC = os.path.expanduser("~/Developer/PlaydateSDK/Disk/Data/com.gopherbone.lavaemu/autotest")
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "host", "shots")
os.makedirs(out, exist_ok=True)
names = sorted(n for n in os.listdir(SRC) if n.endswith(".pbm"))
ims = []
for n in names:
    im = Image.open(os.path.join(SRC, n)).convert("L")
    im.save(os.path.join(out, n[:-4] + ".png"))
    ims.append(im)
cols = 3
sheet = Image.new("L", (cols * 406, ((len(ims) + cols - 1) // cols) * 246), 128)
for i, im in enumerate(ims):
    sheet.paste(im, ((i % cols) * 406, (i // cols) * 246))
sheet.save(os.path.join(out, "_sheet.png"))
print(len(ims), "shots ->", out)
