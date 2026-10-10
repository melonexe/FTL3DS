#!/usr/bin/env python3
"""Builds the CIA's 3D home-menu banner and icon from your own FTL files.

    python make_banner3d.py <ftl.dat> <FTLGame.exe> <pycgfx dir> <out dir>

Writes into <out dir>:
    banner.cgfx    - the 3D banner scene (for `bannertool makebanner -ci`)
    icon.png       - 48x48 home-menu icon, FTL's own icon from FTLGame.exe
    banner.wav     - the banner jingle: the opening of FTL's title theme
    banner_preview_N.png - rough software renders of the scene, for checking the layout

The scene, back to front: a nebula sky, the populated planet from FTL's title screen, two rebel
fighters, and the FTL logo with an extruded 3D Kestrel slowly orbiting it on a tilted ring
(in front of the logo at the bottom of the ring, behind it at the top), banking to keep its deck
towards the viewer. The home menu draws it with its own camera: at (0, 1, 44.786) looking down -z,
30 degrees vertical field of view, so the z = 0 plane shows 40 x 24 units (10 px per unit).

Needs Pillow, pefile, gltflib, numpy and soundfile (pip install pillow pefile gltflib numpy soundfile) and pycgfx
(github.com/skyfloogle/pycgfx). The output contains Subset Games' art: keep it to yourself.
"""
import base64
import io
import json
import math
import os
import struct
import sys

from PIL import Image, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from ftldat import FtlDat  # noqa: E402

# Never set glTF's doubleSided: pycgfx's double-sided materials freeze a real 3DS's home menu
# (tested on hardware). Faces that must show from behind get a reversed copy here instead.
CLAMP = 33071

CAM_Z = 44.786
CAM_Y = 1.0
PERIOD = 16.0  # seconds for one orbit; everything else loops on the same period
KEYS = 64      # animation keys per loop
SPIN_PERIOD = 8.0  # seconds for one turn of the logo (the "spin" variant)

# Ring the Kestrel flies on.
RING_CENTRE = (0.0, 1.0, 0.0)
RING_R = 13.0
RING_TILT = math.radians(68)   # away from facing the camera: the top of the ring goes back
SHIP_LEN = 7.0
SHIP_THICK = 0.7
SHIP_BANK = math.radians(45)   # roll towards the viewer at the bottom and top of the ring


# ---- images -----------------------------------------------------------------------------

def load(dat, path):
    return Image.open(io.BytesIO(dat.read(path))).convert("RGBA")


def cropped(im, threshold=0):
    a = im.getchannel("A").point(lambda v: 255 if v > threshold else 0)
    return im.crop(a.getbbox())


def exe_icon(exe_path):
    """The largest 32-bit icon in FTLGame.exe."""
    import pefile
    pe = pefile.PE(exe_path)
    best = None
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id != 3:  # RT_ICON
            continue
        for e in t.directory.entries:
            for lang in e.directory.entries:
                d = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
                if d[:8] == b"\x89PNG\r\n\x1a\n":
                    im = Image.open(io.BytesIO(d))
                    w, bpp = im.width, 32
                else:
                    w, = struct.unpack("<i", d[4:8])
                    bpp, = struct.unpack("<H", d[14:16])
                    ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", w % 256, w % 256, 0, 0, 1, bpp, len(d), 22) + d
                    im = Image.open(io.BytesIO(ico))
                if bpp == 32 and (best is None or w > best.width):
                    best = im.convert("RGBA")
    return best


# ---- glTF building ----------------------------------------------------------------------

class Gltf:
    def __init__(self, out_dir):
        self.out_dir = out_dir
        self.buf = bytearray()
        self.j = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [],
                  "materials": [], "textures": [], "images": [], "samplers": [
                      {"magFilter": 9729, "minFilter": 9729, "wrapS": CLAMP, "wrapT": CLAMP}],
                  "accessors": [], "bufferViews": [], "buffers": [], "animations": [{"channels": [], "samplers": []}]}

    def _view(self, data, target=None):
        while len(self.buf) % 4:
            self.buf.append(0)
        v = {"buffer": 0, "byteOffset": len(self.buf), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.buf += data
        self.j["bufferViews"].append(v)
        return len(self.j["bufferViews"]) - 1

    def accessor(self, values, kind, comp=5126, target=None):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[kind]
        flat = [x for v in values for x in (v if n > 1 else [v])]
        fmt = {5126: "f", 5123: "H"}[comp]
        data = struct.pack("<%d%s" % (len(flat), fmt), *flat)
        acc = {"bufferView": self._view(data, target), "componentType": comp, "count": len(values), "type": kind}
        if comp == 5126 and kind != "SCALAR" or kind == "SCALAR" and comp == 5126:
            cols = list(zip(*[(v if n > 1 else [v]) for v in values]))
            acc["min"] = [min(c) for c in cols]
            acc["max"] = [max(c) for c in cols]
        self.j["accessors"].append(acc)
        return len(self.j["accessors"]) - 1

    def material(self, name, image, mode, cutoff=0.5):
        path = os.path.join(self.out_dir, name + ".png")
        image.save(path)
        self.j["images"].append({"uri": name + ".png", "name": name})
        self.j["textures"].append({"source": len(self.j["images"]) - 1, "sampler": 0})
        m = {"name": name, "alphaMode": mode,
             "pbrMetallicRoughness": {"baseColorTexture": {"index": len(self.j["textures"]) - 1},
                                      "metallicFactor": 0, "roughnessFactor": 1}}
        if mode == "MASK":
            m["alphaCutoff"] = cutoff
        self.j["materials"].append(m)
        return len(self.j["materials"]) - 1

    def mesh(self, name, prims):
        """prims: list of (material, positions, uvs, indices)."""
        out = []
        for mat, pos, uv, idx in prims:
            normals = [(0.0, 0.0, 1.0)] * len(pos)
            out.append({"attributes": {"POSITION": self.accessor(pos, "VEC3", target=34962),
                                       "NORMAL": self.accessor(normals, "VEC3", target=34962),
                                       "TEXCOORD_0": self.accessor(uv, "VEC2", target=34962)},
                        "indices": self.accessor(idx, "SCALAR", 5123, target=34963), "material": mat})
        self.j["meshes"].append({"name": name, "primitives": out})
        return len(self.j["meshes"]) - 1

    def node(self, name, parent=None, mesh=None, t=None, r=None):
        n = {"name": name}
        if mesh is not None:
            n["mesh"] = mesh
        if t:
            n["translation"] = list(t)
        if r:
            n["rotation"] = list(r)
        self.j["nodes"].append(n)
        i = len(self.j["nodes"]) - 1
        if parent is None:
            self.j["scenes"][0]["nodes"].append(i)
        else:
            self.j["nodes"][parent].setdefault("children", []).append(i)
        return i

    def animate(self, node, path, values):
        """values: one per key, KEYS + 1 of them evenly over PERIOD (the last equals the first)."""
        times = [PERIOD * k / KEYS for k in range(KEYS + 1)]
        a = self.j["animations"][0]
        a["samplers"].append({"input": self.accessor(times, "SCALAR"), "interpolation": "LINEAR",
                              "output": self.accessor(values, "VEC4" if path == "rotation" else "VEC3")})
        a["channels"].append({"sampler": len(a["samplers"]) - 1, "target": {"node": node, "path": path}})

    def save(self, name):
        self.j["buffers"] = [{"byteLength": len(self.buf),
                              "uri": "data:application/octet-stream;base64," + base64.b64encode(self.buf).decode()}]
        path = os.path.join(self.out_dir, name)
        with open(path, "w") as f:
            json.dump(self.j, f)
        return path


def quad(cx, cy, w, h, z=0.0, flip=False):
    """A camera-facing rectangle centred on (cx, cy)."""
    x0, x1, y0, y1 = cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2
    pos = [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]
    uv = [(0, 1), (1, 1), (1, 0), (0, 0)]
    if flip:
        uv = [(1, 1), (0, 1), (0, 0), (1, 0)]
    return pos, uv, [0, 1, 2, 0, 2, 3]


def qrot(axis, angle):
    s = math.sin(angle / 2)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle / 2))


def extruded_ship(img, length, thick, mat_face, mat_side, grid=72):
    """The sprite as a slab: front and back faces plus side walls round its outline. The walls
    take their colour from the hull pixel just inside the edge (from the darkened side texture)."""
    w = length
    h = length * img.height / img.width
    gw, gh = grid, max(1, round(grid * img.height / img.width))
    small = img.resize((gw, gh), Image.BOX)
    solid = [[small.getpixel((x, y))[3] > 150 for x in range(gw)] for y in range(gh)]
    cw, ch = w / gw, h / gh

    def at(x, y):
        return 0 <= x < gw and 0 <= y < gh and solid[y][x]

    def P(x, y):  # grid corner to model space (image y down -> model y up)
        return (-w / 2 + x * cw, h / 2 - y * ch)

    faces = []
    for z, flip in ((thick / 2, False), (-thick / 2, True)):
        pos, uv, idx = quad(0, 0, w, h, z)
        faces.append((mat_face, pos, uv, idx if not flip else [0, 2, 1, 0, 3, 2]))
    pos, uv, idx = [], [], []

    def wall(a, b, cell):
        (ax, ay), (bx, by) = P(*a), P(*b)
        u, v = (cell[0] + 0.5) / gw, (cell[1] + 0.5) / gh
        base = len(pos)
        pos.extend([(ax, ay, thick / 2), (bx, by, thick / 2), (bx, by, -thick / 2), (ax, ay, -thick / 2)])
        uv.extend([(u, v)] * 4)
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])  # both windings: the
        idx.extend([base, base + 2, base + 1, base, base + 3, base + 2])  # home menu culls back faces

    # Runs of boundary edges along each row and column, merged into long walls.
    for y in range(gh + 1):
        for side in (0, 1):  # 0: solid above / empty below, 1: the reverse
            x = 0
            while x < gw:
                def edge(x):
                    up, down = at(x, y - 1), at(x, y)
                    return (up and not down) if side == 0 else (down and not up)
                if edge(x):
                    x0 = x
                    while x < gw and edge(x):
                        x += 1
                    cell = ((x0 + x) // 2, y - 1 if side == 0 else y)
                    wall((x0, y), (x, y), cell)
                else:
                    x += 1
    for x in range(gw + 1):
        for side in (0, 1):
            y = 0
            while y < gh:
                def edge(y):
                    left, right = at(x - 1, y), at(x, y)
                    return (left and not right) if side == 0 else (right and not left)
                if edge(y):
                    y0 = y
                    while y < gh and edge(y):
                        y += 1
                    cell = (x - 1 if side == 0 else x, (y0 + y) // 2)
                    wall((x, y0), (x, y), cell)
                else:
                    y += 1
    assert len(pos) < 65536
    return faces + [(mat_side, pos, uv, idx)], len(pos)


# ---- the scene --------------------------------------------------------------------------

def build(dat, out_dir, variant="full", layers=("planet", "rebels", "logo", "kestrel"), sky_h=256):
    """variant: "static" (no animation, the Kestrel parked at the front of the ring), "full", or
    "spin" (just the sky and the FTL logo turning; use with layers=("logo",)).
    layers: which layers besides the sky ("kface" = the Kestrel's top face alone). Both are for
    finding what the home menu objects to."""
    g = Gltf(out_dir)
    animate = variant == "full"

    # Sky: FTL's dark nebula backdrop, far back, filling the view.
    sky = load(dat, "img/stars/bg_darknebula.png").convert("RGB").convert("RGBA").resize((256, sky_h), Image.LANCZOS)
    m = g.material("sky", sky, "OPAQUE")
    z = -80.0
    k = (CAM_Z - z) / CAM_Z
    pos, uv, idx = quad(0, CAM_Y, 42 * k * 1.78 / 1.67, 25 * k, z)
    g.node("Sky", mesh=g.mesh("Sky", [(m, pos, uv, idx)]))

    if "planet" in layers:  # the populated planet from the title screen, upper left
        ps = int(os.environ.get("BANNER_PLANET_TEX", "128"))
        planet = cropped(load(dat, "img/stars/planet_populated_dark.png"), 8).resize((ps, ps), Image.LANCZOS)
        m = g.material("planet", planet, os.environ.get("BANNER_PLANET_MODE", "BLEND"))
        centred = os.environ.get("BANNER_PLANET_CENTRE") == "1"
        pos, uv, idx = quad(0, 0, 16 if centred else 34, 16 if centred else 34, 0)
        g.node("Planet", mesh=g.mesh("Planet", [(m, pos, uv, idx)]), t=(0, 1, -40) if centred else (-25, 13, -40))

    if "rebels" in layers:  # two rebel fighters (FTL's art points up; turned to face right)
        rebel_src = cropped(load(dat, "img/ships_glow/rebel_long_base.png"), 8).rotate(-90, expand=True)
        rebel = rebel_src.resize((128, 64), Image.LANCZOS)
        m = g.material("rebel", rebel, "BLEND")
        rl = 8.0
        pos, uv, idx = quad(0, 0, rl, rl * rebel_src.height / rebel_src.width, 0)
        for name, base, amp, phase in (("RebelFar", (-30, 3, -34), 0.5, 0.0), ("RebelNear", (-21, -8, -24), 0.7, 2.1)):
            n = g.node(name, mesh=g.mesh(name, [(m, pos, uv, idx)]), t=base)
            if animate:
                g.animate(n, "translation", [(base[0] + 1.2 * math.sin(2 * math.pi * k_ / KEYS + phase),
                                              base[1] + amp * math.sin(4 * math.pi * k_ / KEYS + phase), base[2])
                                             for k_ in range(KEYS + 1)])

    if "logo" in layers:  # the FTL logo, at the centre of the ring
        logo_src = cropped(load(dat, "img/main_menus/main_FTL2.png"), 40)
        logo = logo_src.resize((256, 128), Image.LANCZOS)
        m = g.material("logo", logo, "MASK", 0.5)
        if variant == "spin":
            # Alone in front of the sky, turning about its upright axis. The back is a reversed copy
            # (never doubleSided, see CLAMP) mirrored so it reads the right way round from behind,
            # in the same primitive so the scene keeps the hardware-tested two materials.
            lw = 22.0
            pos, uv, idx = quad(0, 0, lw, lw * logo_src.height / logo_src.width, 0)
            back_uv = [(1 - u, v) for u, v in uv]
            prim = (m, pos + pos, uv + back_uv, idx + [4 + i for i in (0, 2, 1, 0, 3, 2)])
            n = g.node("Logo", mesh=g.mesh("Logo", [prim]), t=(0, CAM_Y, 0))
            turns = PERIOD / SPIN_PERIOD
            g.animate(n, "rotation", [qrot((0, 1, 0), 2 * math.pi * turns * k_ / KEYS) for k_ in range(KEYS + 1)])
        else:
            lw = 15.0
            pos, uv, idx = quad(0, 0, lw, lw * logo_src.height / logo_src.width, 0)
            g.node("Logo", mesh=g.mesh("Logo", [(m, pos, uv, idx)]), t=(0, RING_CENTRE[1] + 2.6, 0))

    if "kestrel" in layers or "kface" in layers:  # the Kestrel, extruded from its hull sprite
        ship_src = cropped(load(dat, "img/ship/kestral_base.png"), 150)
        ship = ship_src.resize((256, 128), Image.LANCZOS)
        mf = g.material("kestrel", ship, "MASK", 0.5)
        if "kestrel" in layers:
            side = ImageEnhance.Brightness(ship_src.resize((128, 64), Image.LANCZOS)).enhance(0.55)
            ms = g.material("kestrel_side", side, "MASK", 0.5)
            prims, nverts = extruded_ship(ship_src, SHIP_LEN, SHIP_THICK, mf, ms)
            print(f"ship: {nverts} wall vertices")
        else:
            pos, uv, idx = quad(0, 0, SHIP_LEN, SHIP_LEN * ship_src.height / ship_src.width, 0)
            prims = [(mf, pos, uv, idx)]
        if not animate:  # parked at the front of the ring, no hierarchy
            g.node("Kestrel", mesh=g.mesh("Kestrel", prims), t=(0, RING_CENTRE[1] - 5, 8))
        else:
            ring = g.node("Ring", t=RING_CENTRE, r=qrot((1, 0, 0), -RING_TILT))
            spin = g.node("Spin", parent=ring)
            arm = g.node("Arm", parent=spin, t=(RING_R, 0, 0), r=qrot((0, 0, 1), math.pi / 2))
            roll = g.node("Roll", parent=arm)
            g.node("Kestrel", parent=roll, mesh=g.mesh("Kestrel", prims))
            phis = [2 * math.pi * k_ / KEYS for k_ in range(KEYS + 1)]
            g.animate(spin, "rotation", [qrot((0, 0, 1), p) for p in phis])
            g.animate(roll, "rotation", [qrot((1, 0, 0), -SHIP_BANK * math.sin(p)) for p in phis])
    if not g.j["animations"][0]["channels"]:
        del g.j["animations"]
    return g


def jingle(dat, path, seconds=2.9, fade=0.8):
    """The title theme's opening at 22050 Hz, normalised and faded out. Stereo and under 3 s, as
    the home menu requires of banner sounds (3dbrew: CBMD)."""
    import numpy as np
    import soundfile as sf
    pcm, rate = sf.read(io.BytesIO(dat.read("audio/music/bp_MUS_TitleScreen.ogg")))
    if pcm.ndim > 1:
        pcm = pcm.mean(axis=1)
    pcm = pcm[: int(seconds * rate)]
    if rate == 44100:  # halve the rate (averaging pairs filters a little first)
        pcm = (pcm[0::2][: len(pcm) // 2] + pcm[1::2][: len(pcm) // 2]) / 2
        rate = 22050
    pcm = pcm / max(1e-6, float(np.abs(pcm).max())) * 0.9
    n = int(fade * rate)
    pcm[-n:] *= np.linspace(1.0, 0.0, n)
    import wave
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.repeat((pcm * 32767).astype("<i2"), 2).tobytes())


# ---- after pycgfx: flat, unlit sprites --------------------------------------------------

def unlit(cgfx):
    """pycgfx lights every material (texture x diffuse + specular). Banner art should show its
    own colours: texture straight through the combiners."""
    for name in cgfx.data.models["COMMON"].materials:
        mt = cgfx.data.models["COMMON"].materials[name]
        tc = mt.fragment_shader.texture_combiners
        tc[0].src_rgb = tc[0].src_alpha = 0x003  # texture 0
        tc[0].combine_rgb = tc[0].combine_alpha = 0  # replace
        for c in tc[1:]:
            c.src_rgb = c.src_alpha = 0x00F  # previous
            c.combine_rgb = c.combine_alpha = 0


# ---- 128-byte alignment ----------------------------------------------------------------

ALIGN = 128


def align_imag_128():
    """pycgfx pads texture and vertex data to 16 bytes. Nintendo's CGFX files (bannertool's
    template included) put them on 128-byte boundaries, and pycgfx banners with 16-byte-aligned
    data crashed or froze a real 3DS home menu depending on where the data happened to land."""
    from cgfx.shared import StringTable

    def correct(s):
        if isinstance(s, str):
            return s.encode() + b"\0"
        return s + b"\0" * (-len(s) % ALIGN)
    StringTable.correct = staticmethod(correct)


def write_aligned(pycgfx, cgfx):
    """pycgfx.write, with the strings padded so the IMAG data starts on a 128-byte boundary."""
    from cgfx.shared import StringTable
    strings, imag = StringTable(), StringTable()
    offset = cgfx.prepare(0, strings, imag)
    offset = strings.prepare(offset)
    if not imag.empty():
        extra = -(offset + 8) % ALIGN  # IMAG header is 8 bytes
        strings.padding += extra
        strings.total += extra
        offset += extra
    cgfx.data.section_size = offset - cgfx.data.offset
    if not imag.empty():
        cgfx.header.nr_blocks = 2
        offset += 8
    offset = imag.prepare(offset)
    cgfx.header.file_size = offset
    data = cgfx.write(strings, imag)
    data += strings.write()
    if not imag.empty():
        start = len(data) + 8
        assert start % ALIGN == 0 and all((start + o) % ALIGN == 0 for o in imag.table.values())
        data += b"IMAG" + imag.size().to_bytes(4, "little") + imag.write()
    return data


# ---- preview ----------------------------------------------------------------------------

def preview(j, out_dir, t, path):
    """Rough render at time t of the camera-facing quads and the ship's top face, painter's
    order by depth. Only for checking the layout."""
    W, H = 400, 240
    f = (H / 2) / math.tan(math.radians(15))
    nodes = j["nodes"]

    def mat_mul(a, b):
        return [[sum(a[i][k] * b[k][jj] for k in range(4)) for jj in range(4)] for i in range(4)]

    def trs(tr, q):
        x, y, z, w = q
        r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), tr[0]],
             [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), tr[1]],
             [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), tr[2]], [0, 0, 0, 1]]
        return r

    def sample(node_i, path, default):
        for ch in j.get("animations", [{"channels": []}])[0]["channels"]:
            if ch["target"]["node"] == node_i and ch["target"]["path"] == path:
                return anim_vals[ch["sampler"]](t)
        return default

    anim_vals = []
    raw = base64.b64decode(j["buffers"][0]["uri"].split(",", 1)[1])

    def acc_data(ai):
        a = j["accessors"][ai]
        v = j["bufferViews"][a["bufferView"]]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a["type"]]
        fmt = "f" if a["componentType"] == 5126 else "H"
        flat = struct.unpack_from("<%d%s" % (a["count"] * n, fmt), raw, v["byteOffset"])
        return [flat[i * n:(i + 1) * n] for i in range(a["count"])] if n > 1 else list(flat)

    for s in j.get("animations", [{"samplers": []}])[0]["samplers"]:
        ts, vs = acc_data(s["input"]), acc_data(s["output"])

        def fn(t, ts=ts, vs=vs):
            t = t % PERIOD
            for i in range(len(ts) - 1):
                if ts[i] <= t <= ts[i + 1]:
                    u = (t - ts[i]) / (ts[i + 1] - ts[i])
                    out = [a + (b - a) * u for a, b in zip(vs[i], vs[i + 1])]
                    if len(out) == 4:
                        n = math.sqrt(sum(c * c for c in out))
                        out = [c / n for c in out]
                    return out
            return vs[0]
        anim_vals.append(fn)

    draws = []

    def walk(i, parent):
        n = nodes[i]
        m = mat_mul(parent, trs(sample(i, "translation", n.get("translation", [0, 0, 0])),
                                sample(i, "rotation", n.get("rotation", [0, 0, 0, 1]))))
        if "mesh" in n:
            for p in j["meshes"][n["mesh"]]["primitives"]:
                pos = acc_data(p["attributes"]["POSITION"])
                if len(pos) == 8:
                    pos = pos[:4]  # a two-sided quad (the spinning logo): its back is mirrored below
                elif len(pos) != 4:
                    continue  # ship walls
                world = [[sum(m[r][c] * (list(v) + [1])[c] for c in range(4)) for r in range(3)] for v in pos]
                mat = j["materials"][p["material"]]
                img = Image.open(os.path.join(out_dir, j["images"][j["textures"][mat["pbrMetallicRoughness"]["baseColorTexture"]["index"]]["source"]]["uri"]))
                pts = []
                for x, y, z in world:
                    d = CAM_Z - z
                    pts.append((W / 2 + f * x / d, H / 2 - f * (y - CAM_Y) / d))
                depth = sum(w[2] for w in world) / 4
                draws.append((depth, pts, img.convert("RGBA")))
        for c in n.get("children", []):
            walk(c, m)

    ident = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    for r in j["scenes"][0]["nodes"]:
        walk(r, ident)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    for depth, pts, img in sorted(draws, key=lambda d: d[0]):
        # pts: corners for uv (0,1) (1,1) (1,0) (0,0) -> image bottom-left, bottom-right, top-right, top-left
        src = [(0, img.height), (img.width, img.height), (img.width, 0), (0, 0)]
        area = sum(pts[i][0] * pts[(i + 1) % 4][1] - pts[(i + 1) % 4][0] * pts[i][1] for i in range(4))
        if abs(area) < 1.0:
            continue  # edge on
        if area > 0:  # seen from behind (screen y is down): the back face shows, mirrored
            src = [src[1], src[0], src[3], src[2]]
        coeffs = _perspective(pts, src)
        layer = img.transform((W, H), Image.PERSPECTIVE, coeffs, Image.BILINEAR)
        canvas.alpha_composite(layer)
    canvas.save(path)


def _perspective(dst, src):
    """Coefficients mapping output (dst) points back to input (src) points."""
    import numpy as np  # only for the preview
    A, B = [], []
    for (x, y), (u, v) in zip(dst, src):
        A.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); B.append(u)
        A.append([0, 0, 0, x, y, 1, -v * x, -v * y]); B.append(v)
    return list(np.linalg.solve(np.array(A, float), np.array(B, float)))


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 1
    dat, exe, pycgfx_dir, out_dir = FtlDat(argv[1]), argv[2], argv[3], argv[4]
    variant = os.environ.get("BANNER_VARIANT", "full")
    flat_lit = os.environ.get("BANNER_LIT") == "1"  # skip the unlit patch (testing)
    os.makedirs(out_dir, exist_ok=True)

    icon = exe_icon(exe)
    if icon is None:
        print("no icon found in", exe)
        return 1
    icon.resize((48, 48), Image.LANCZOS).save(os.path.join(out_dir, "icon.png"))

    jingle(dat, os.path.join(out_dir, "banner.wav"))
    layers = os.environ.get("BANNER_LAYERS", "planet,rebels,logo,kestrel").split(",")
    g = build(dat, out_dir, variant, layers, int(os.environ.get("BANNER_SKY_H", "256")))
    gltf_path = g.save("banner.gltf")
    times = (0.0, SPIN_PERIOD * 0.125, SPIN_PERIOD * 0.375, SPIN_PERIOD * 0.5) if variant == "spin" else         (0.0, PERIOD * 0.25, PERIOD * 0.5, PERIOD * 0.75)
    for i, t in enumerate(times):
        if "animations" not in g.j and i:
            break
        try:
            preview(g.j, out_dir, t, os.path.join(out_dir, f"banner_preview_{i}.png"))
        except ImportError:
            break

    sys.path.insert(0, pycgfx_dir)
    import gltflib
    import main as pycgfx  # pycgfx's main.py
    if os.environ.get("BANNER_ALIGN16") != "1":
        align_imag_128()
    model = gltflib.GLTF.load(gltf_path, load_file_resources=True)
    cgfx = pycgfx.convert_gltf(model)
    if not flat_lit:
        unlit(cgfx)
    data = write_aligned(pycgfx, cgfx)
    with open(os.path.join(out_dir, "banner.cgfx"), "wb") as f:
        f.write(data)
    print(f"banner.cgfx: {len(data)} bytes (limit {0x80000})")
    return 0 if len(data) <= 0x80000 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
