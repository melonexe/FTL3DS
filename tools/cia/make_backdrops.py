"""Pre-encodes FTL's beacon backdrops (backgrounds and planets) as ETC1 textures for a personal CIA.

The game would otherwise decode each 1280x720 PNG on the 3DS, shrink it and convert it for the GPU
on every jump. Here that's done once on the PC: each image is shrunk exactly as the game would
(texture.c texture_decode_png: the largest whole factor that keeps it at least the size it's drawn,
premultiplied box averaging), then encoded by devkitPro's tex3ds as ETC1 (opaque backgrounds, 4
bits a pixel) or ETC1A4 (planets, with their soft edges: 8 bits a pixel).

Usage: python make_backdrops.py <ftl.dat> <out.pak>
FTL2CIA calls build_pack() with the tex3ds.exe it carries.

The pack (little-endian):
    "FTLBDP01"                  magic and version
    u32 count, u32 datSize      entries; the size of the ftl.dat it was made from (a stale pack is
                                ignored by the game)
    count x { char path[64]; u32 offset; u32 size; }   path as the game asks for it ("img/...")
    the entries' .t3x files (uncompressed), each at its offset from the start of the file
"""
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from ftldat import FtlDat  # noqa: E402

TOP_W, TOP_H = 400, 240  # the 3DS top screen
MAGIC = b"FTLBDP01"
PATH_LEN = 64

# Backdrops the game picks outside the image lists (run.c: the sun and pulsar beacons), with the
# planetSize it draws them at.
EXTRA_PLANETS = {"img/stars/planet_sun1.png": 804, "img/effects/pulsar_white.png": 226}


def find_tex3ds():
    dkp = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    for cand in (os.path.join(dkp, "tools", "bin", "tex3ds"), os.path.join(dkp, "tools", "bin", "tex3ds.exe"),
                 r"C:\devkitPro\tools\bin\tex3ds.exe", shutil.which("tex3ds") or ""):
        if cand and os.path.isfile(cand):
            return cand
    raise RuntimeError("tex3ds not found: install devkitPro's 3ds-dev (it's in $DEVKITPRO/tools/bin)")


def backdrops(dat):
    """{path: (minW, minH)} for every backdrop the game can show, at the biggest size it's asked
    for (a planet in several lists is drawn at several sizes)."""
    xml = dat.read("data/events_imageList.xml").decode("utf-8", "replace")
    out = {}

    def want(path, w, h):
        old = out.get(path, (0, 0))
        out[path] = (max(old[0], w), max(old[1], h))

    for lst, body in re.findall(r'<imageList name="([^"]+)"[^>]*>(.*?)</imageList>', xml, re.S):
        for attrs, path in re.findall(r"<img([^>]*)>([^<]+)</img>", body):
            path = "img/" + path.strip()
            if lst.startswith("PLANET"):
                m = re.search(r'w="(\d+)"', attrs)
                px = int(m.group(1)) * TOP_W // 1280 if m else 0  # main.c backdrop_sync
                want(path, px, px)
            else:
                want(path, TOP_W, TOP_H)
    for path, size in EXTRA_PLANETS.items():
        px = size * TOP_W // 1280
        want(path, px, px)
    return out


def shrink(im, min_w, min_h):
    """texture_decode_png's downscale: the largest whole factor f with w/f >= minW and h/f >= minH
    (and at most 1024 a side), averaging f x f blocks premultiplied by alpha."""
    w, h = im.size
    f = 1
    if min_w > 0 and min_h > 0:
        while w // (f + 1) >= min_w and h // (f + 1) >= min_h:
            f += 1
    while w // f > 1024 or h // f > 1024:
        f += 1
    if f == 1:
        return im
    nw, nh = w // f, h // f
    pm = im.crop((0, 0, nw * f, nh * f)).convert("RGBa")
    return pm.resize((nw, nh), Image.BOX).convert("RGBA")


def encode(tex3ds, png_path, t3x_path, has_alpha):
    p = subprocess.run([tex3ds, "-f", "etc1a4" if has_alpha else "etc1", "-q", "high", "-z", "none",
                        "-o", t3x_path, png_path], capture_output=True, text=True,
                       creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))  # no console from a GUI
    if p.returncode != 0:
        raise RuntimeError(f"tex3ds failed (code {p.returncode}): {(p.stderr or p.stdout).strip()}")


def build_pack(dat_path, out_path, tex3ds=None, log=print):
    """Writes the pack for this ftl.dat. Raises on failure (the game works without a pack)."""
    tex3ds = tex3ds or find_tex3ds()
    dat = FtlDat(dat_path)
    entries = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (path, (min_w, min_h)) in enumerate(sorted(backdrops(dat).items())):
            try:
                raw = dat.read(path)
            except Exception:
                log(f"  {path}: not in ftl.dat, skipped")
                continue
            im = shrink(Image.open(io.BytesIO(raw)).convert("RGBA"), min_w, min_h)
            has_alpha = im.getextrema()[3][0] < 255
            png, t3x = os.path.join(tmp, f"{i}.png"), os.path.join(tmp, f"{i}.t3x")
            im.save(png)
            encode(tex3ds, png, t3x, has_alpha)
            with open(t3x, "rb") as f:
                entries.append((path, f.read()))
            log(f"  {path}: {im.size[0]}x{im.size[1]} {'ETC1A4' if has_alpha else 'ETC1'}")

    head = len(MAGIC) + 8 + len(entries) * (PATH_LEN + 8)
    index, blobs, off = b"", b"", head
    for path, data in entries:
        name = path.encode()
        assert len(name) < PATH_LEN, path
        index += name.ljust(PATH_LEN, b"\0") + struct.pack("<II", off, len(data))
        blobs += data
        off += len(data)
    with open(out_path, "wb") as f:
        f.write(MAGIC + struct.pack("<II", len(entries), os.path.getsize(dat_path)) + index + blobs)
    log(f"{out_path}: {len(entries)} backdrops, {off // 1024} KB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build_pack(sys.argv[1], sys.argv[2])
