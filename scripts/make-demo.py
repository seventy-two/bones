#!/usr/bin/env python3
"""Makes the mock "Media" drive for README screenshots (see demo.ps1).

Writes into OUT: procedural landscape photos, a few short video clips
(needs ffmpeg), small documents, and sparse.txt, a list of big files
(films, games, backups) that demo.ps1 makes as sparse files, so they
show their full size without taking any disk space. Everything is made
up and the same every run.

    python3 make-demo.py OUT
"""
import math
import os
import random
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter

GB = 1000**3
MB = 1000**2


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# Palettes: sky top, sky bottom, sun, far hills, near hills.
PALETTES = [
    ((28, 36, 84), (246, 146, 96), (255, 226, 160), (120, 86, 120), (36, 28, 52)),      # dusk
    ((86, 156, 222), (204, 228, 244), (255, 250, 220), (112, 150, 170), (46, 82, 70)),  # day
    ((14, 18, 40), (64, 54, 112), (236, 232, 255), (60, 58, 100), (18, 18, 34)),        # night
    ((242, 168, 108), (252, 222, 160), (255, 244, 210), (186, 120, 92), (98, 52, 48)),  # desert
    ((150, 186, 200), (226, 236, 236), (255, 255, 250), (150, 170, 180), (70, 92, 104)),  # snow
    ((40, 70, 110), (150, 190, 200), (255, 236, 200), (60, 100, 110), (20, 50, 50)),    # fjord
]


def ridge(rng, w, base, rough):
    """A mountain line: summed sines with random phases."""
    waves = [(rng.uniform(0.5, 3.5) / w * math.tau, rng.uniform(0, math.tau), rng.uniform(0.3, 1.0)) for _ in range(6)]
    pts = []
    for x in range(0, w + 8, 8):
        y = base
        for i, (f, ph, a) in enumerate(waves):
            y -= math.sin(x * f * (i + 1) + ph) * a * rough / (i + 1)
        pts.append((x, y))
    return pts


def landscape(seed, w=1600, h=1067):
    rng = random.Random(seed)
    top, bottom, sun, far, near = PALETTES[seed % len(PALETTES)]
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        d.line([(0, y), (w, y)], fill=lerp(top, bottom, min(1, y / (h * 0.7))))
    # Sun (or moon), with a soft glow.
    sx, sy, r = rng.uniform(0.2, 0.8) * w, rng.uniform(0.2, 0.45) * h, rng.uniform(40, 80)
    glow = Image.new("L", (w, h), 0)
    ImageDraw.Draw(glow).ellipse([sx - r * 3, sy - r * 3, sx + r * 3, sy + r * 3], fill=110)
    glow = glow.filter(ImageFilter.GaussianBlur(70))
    img.paste(Image.new("RGB", (w, h), sun), mask=glow)
    d = ImageDraw.Draw(img)
    d.ellipse([sx - r, sy - r, sx + r, sy + r], fill=sun)
    # Layers of hills, from far (pale) to near (dark).
    layers = rng.randint(3, 5)
    for i in range(layers):
        t = i / max(1, layers - 1)
        col = lerp(lerp(far, bottom, 0.35), near, t)
        base = h * (0.5 + 0.12 * i)
        pts = ridge(rng, w, base, h * (0.16 - 0.025 * i))
        d.polygon(pts + [(w, h), (0, h)], fill=col)
    # Water on some: a reflective band at the bottom.
    if seed % 3 == 0:
        wy = int(h * 0.82)
        band = img.crop((0, wy - (h - wy), w, wy)).transpose(Image.FLIP_TOP_BOTTOM)
        band = Image.blend(band, Image.new("RGB", band.size, lerp(top, bottom, 0.6)), 0.45)
        img.paste(band, (0, wy))
    return img.filter(ImageFilter.SMOOTH)


def abstract(seed, w=1400, h=1400):
    rng = random.Random(seed * 7919)
    pal = PALETTES[seed % len(PALETTES)]
    img = Image.new("RGB", (w, h), pal[0])
    d = ImageDraw.Draw(img)
    for _ in range(18):
        r = rng.uniform(80, 420)
        x, y = rng.uniform(0, w), rng.uniform(0, h)
        d.ellipse([x - r, y - r, x + r, y + r], fill=pal[rng.randrange(1, 5)])
    return img.filter(ImageFilter.GaussianBlur(24))


def write(path, data=b""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main(out):
    rng = random.Random(72)
    photos = os.path.join(out, "Photos")
    n = 0
    for i in range(18):
        os.makedirs(photos, exist_ok=True)
        landscape(i).save(os.path.join(photos, f"IMG_{2041 + i * 3}.jpg"), quality=88)
        n += 1
    for i in range(10):
        p = os.path.join(photos, "Iceland 2025")
        os.makedirs(p, exist_ok=True)
        landscape(100 + i * 6 + 4).save(os.path.join(p, f"DSC0{4410 + i}.jpg"), quality=88)
        n += 1
    for i in range(8):
        p = os.path.join(photos, "Wallpapers")
        os.makedirs(p, exist_ok=True)
        abstract(i).save(os.path.join(p, f"blur-{i + 1:02}.png"))
        n += 1
    print(f"{n} pictures")

    # Clips: a slow pan over a landscape each, 3 s.
    clips = os.path.join(out, "Videos", "Clips")
    os.makedirs(clips, exist_ok=True)
    for i, name in enumerate(["harbour", "ridge walk", "night drive", "dunes", "first snow"]):
        src = os.path.join(out, f".clip{i}.png")
        landscape(200 + i).resize((1920, 1280)).save(src)
        dst = os.path.join(clips, f"{name}.mp4")
        subprocess.run(
            ["nice", "-n", "19", "ffmpeg", "-y", "-loglevel", "error", "-threads", "2", "-loop", "1", "-i", src,
             "-vf", "crop=1280:720:x='(in_w-1280)*t/3':y=200,format=yuv420p", "-t", "3", "-r", "24",
             "-c:v", "libx264", "-preset", "veryfast", dst],
            check=True)
        os.remove(src)
    print("5 clips")

    docs = {
        "Documents/Budget 2026.xlsx": 48 * 1000,
        "Documents/Car insurance.pdf": 220 * 1000,
        "Documents/Notes.txt": 3 * 1000,
        "Documents/Recipes/Bread.md": 2 * 1000,
        "Documents/Recipes/Dal.md": 2 * 1000,
        "Documents/Letters/Landlord.docx": 31 * 1000,
        "Documents/Letters/Council tax.docx": 28 * 1000,
        "Desktop/todo.txt": 1000,
        "Desktop/screenshot.png": None,
        "Downloads/setup-tool-2.4.1.exe": 84 * MB,
        "Downloads/fonts.zip": 12 * MB,
        "Downloads/receipt-1182.pdf": 90 * 1000,
        "Projects/bones/README.md": 6 * 1000,
        "Projects/bones/build.bat": 2 * 1000,
        "Projects/site/index.html": 4 * 1000,
    }
    for rel, size in docs.items():
        path = os.path.join(out, rel)
        if size is None:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            landscape(300).resize((800, 533)).save(path)
        else:
            write(path, bytes(rng.randrange(32, 127) for _ in range(min(size, 4096))) * (size // 4096 + 1))
            with open(path, "r+b") as f:
                f.truncate(size)

    # The big things, as sparse files (demo.ps1 makes them).
    big = []
    films = [("The Long Road (2019)", 14.2), ("Northern Lights (2021)", 21.7), ("Glass Harbour (2016)", 9.8),
             ("A Quiet Year (2023)", 17.3), ("Paper Moons (2012)", 6.4), ("Field Notes (2020)", 11.9),
             ("The Last Ferry (2018)", 24.6), ("Saltwater (2022)", 8.1)]
    for name, gb in films:
        big.append((f"Films/{name}/{name}.mkv", int(gb * GB)))
    for show, eps, gb in [("Coastline", 8, 2.1), ("The Workshop", 10, 1.4), ("Night Shift", 6, 3.2)]:
        for e in range(1, eps + 1):
            big.append((f"Shows/{show}/Season 1/{show} S01E{e:02}.mkv", int(gb * GB * rng.uniform(0.85, 1.15))))
    for game, gb in [("Skyline Racer", 86), ("Deep Orbit", 142), ("Hollow Keep", 38), ("Tiny Farm", 3.2)]:
        for part in range(1, 4):
            big.append((f"Games/{game}/data/pack{part}.pak", int(gb * GB / 3)))
        big.append((f"Games/{game}/{game}.exe", int(0.12 * GB)))
    big += [("Backups/2026-10 laptop.img", 238 * GB), ("Backups/2026-11 laptop.img", 241 * GB),
            ("Backups/phone/2026-11-02.tar", 61 * GB)]
    for artist, album, tracks in [("Low Tide", "Harbour Lights", 11), ("The Pines", "Second Winter", 9),
                                  ("Mara Quill", "Paper Boats", 12)]:
        for t in range(1, tracks + 1):
            big.append((f"Music/{artist}/{album}/{t:02} Track {t}.flac", int(rng.uniform(22, 48) * MB)))
    big += [("Downloads/ubuntu-26.04-desktop-amd64.iso", int(6.1 * GB)), ("Videos/Clips/raw/drone-0412.mov", int(18.4 * GB))]
    with open(os.path.join(out, "sparse.txt"), "w", newline="\r\n") as f:
        for rel, size in big:
            f.write(f"{rel.replace('/', chr(92))}\t{size}\n")
    print(f"{len(big)} sparse files listed, {sum(s for _, s in big) / GB:.0f} GB")


if __name__ == "__main__":
    main(sys.argv[1])
