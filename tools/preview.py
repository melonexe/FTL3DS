#!/usr/bin/env python3
"""Renders ships the same way source/main.c does, as a local HTML page, to check
layout maths on PC. Images come from your own ftl.dat and stay on your machine.

    python preview.py <ftl.dat> [out.html]
"""
import base64
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ftldat import FtlDat  # noqa: E402

COUNTS = {"X_OFFSET": 1, "Y_OFFSET": 1, "VERTICAL": 1, "HORIZONTAL": 1, "ELLIPSE": 4, "ROOM": 5, "DOOR": 5}


def strip_comments(xml):
    return re.sub(r"<!--.*?-->", "", xml, flags=re.S)


def attr(tag, name):
    m = re.search(r'\s' + name + r'\s*=\s*"([^"]*)"', tag)
    return m.group(1) if m else None


def blueprints(dat):
    # Mirrors ship_list_blueprints(): the first definition of a layout wins, except that
    # dlcBlueprintsOverwrite.xml replaces earlier ones.
    seen, out = set(), []
    for f in ("data/blueprints.xml", "data/dlcBlueprints.xml", "data/dlcBlueprintsOverwrite.xml"):
        if f not in dat.entries:
            continue
        xml = strip_comments(dat.read(f).decode("utf8", "replace"))
        for tag in re.findall(r"<shipBlueprint[\s/>][^>]*>", xml):
            name, layout, img = attr(tag, "name"), attr(tag, "layout"), attr(tag, "img")
            if not (name and layout and img) or not name.startswith("PLAYER_SHIP_") or name == "PLAYER_SHIP_TUTORIAL":
                continue
            if layout in seen:
                if f.endswith("Overwrite.xml"):
                    out = [(name, layout, img) if o[1] == layout else o for o in out]
                continue
            if f"data/{layout}.txt" not in dat.entries:
                continue
            seen.add(layout)
            out.append((name, layout, img))
    return out


def load_ship(dat, name, layout, img):
    s = {"name": name, "xOffset": 0, "yOffset": 0, "rooms": [], "doors": [], "hullX": 0, "hullY": 0, "floorX": 0, "floorY": 0}
    toks = dat.read(f"data/{layout}.txt").decode("latin1").split()
    i = 0
    while i < len(toks):
        k = toks[i]
        i += 1
        if k not in COUNTS:
            continue
        v = [int(x) for x in toks[i:i + COUNTS[k]]]
        i += COUNTS[k]
        if k == "X_OFFSET": s["xOffset"] = v[0]
        elif k == "Y_OFFSET": s["yOffset"] = v[0]
        elif k == "ROOM": s["rooms"].append(v[1:])
        elif k == "DOOR": s["doors"].append(v)
    xml = strip_comments(dat.read(f"data/{layout}.xml").decode("utf8", "replace"))
    m = re.search(r"<img[\s/>][^>]*>", xml)
    if m:
        s["hullX"], s["hullY"] = int(attr(m.group(0), "x") or 0), int(attr(m.group(0), "y") or 0)
    m = re.search(r"<floor[\s/>][^>]*>", xml)
    if m:
        s["floorX"], s["floorY"] = int(attr(m.group(0), "x") or 0), int(attr(m.group(0), "y") or 0)
    for key, suffix in (("hull", "base"), ("floor", "floor")):
        p = f"img/ship/{img}_{suffix}.png"
        if p in dat.entries:
            s[key] = "data:image/png;base64," + base64.b64encode(dat.read(p)).decode()
    return s


PAGE = """<!doctype html><meta charset=utf-8><title>Ship preview</title>
<style>body{background:#222;color:#ddd;font:14px sans-serif;margin:16px}
canvas{background:#080a14;image-rendering:auto;margin:4px}div{display:inline-block;vertical-align:top}</style>
<p>Each canvas is the 400x240 3DS top screen, drawn with the same maths as source/main.c.</p>
<div id=out></div><script>
const SHIPS=__SHIPS__, TILE=35;
for (const s of SHIPS) {
  const wrap=document.createElement('div'), c=document.createElement('canvas');
  c.width=400; c.height=240; wrap.append(c, document.createElement('br'), s.name);
  document.getElementById('out').append(wrap);
  const ox=s.xOffset*TILE, oy=s.yOffset*TILE;
  const imgs={}, keys=['hull','floor'].filter(k=>s[k]);
  let pending=keys.length;
  const go=()=>{ if(--pending>0) return; draw(); };
  if(!pending) draw();
  for(const k of keys){ const im=new Image(); im.onload=go; im.src=s[k]; imgs[k]=im; }
  function draw(){
    let x0=1e9,y0=1e9,x1=-1e9,y1=-1e9;
    for(const [x,y,w,h] of s.rooms){x0=Math.min(x0,ox+x*TILE);y0=Math.min(y0,oy+y*TILE);x1=Math.max(x1,ox+(x+w)*TILE);y1=Math.max(y1,oy+(y+h)*TILE);}
    if(imgs.hull){const hx=ox+s.hullX,hy=oy+s.hullY;x0=Math.min(x0,hx);y0=Math.min(y0,hy);x1=Math.max(x1,hx+imgs.hull.width);y1=Math.max(y1,hy+imgs.hull.height);}
    const z=Math.min(392/(x1-x0),232/(y1-y0)), cx=(x0+x1)/2, cy=(y0+y1)/2;
    const g=c.getContext('2d');
    g.setTransform(z,0,0,z,200-cx*z,120-cy*z);
    if(imgs.hull) g.drawImage(imgs.hull, ox+s.hullX, oy+s.hullY);
    if(imgs.floor) g.drawImage(imgs.floor, ox+s.hullX+s.floorX, oy+s.hullY+s.floorY);
    for(const [x,y,w,h] of s.rooms){
      const X=ox+x*TILE,Y=oy+y*TILE;
      g.fillStyle='rgb(230,226,219)'; g.fillRect(X,Y,w*TILE,h*TILE);
      g.fillStyle='rgb(180,176,170)';
      for(let i=1;i<w;i++) g.fillRect(X+i*TILE-.5,Y,1,h*TILE);
      for(let i=1;i<h;i++) g.fillRect(X,Y+i*TILE-.5,w*TILE,1);
      g.strokeStyle='rgb(40,40,40)'; g.lineWidth=2; g.strokeRect(X,Y,w*TILE,h*TILE);
    }
    g.fillStyle='rgb(230,150,40)';
    const L=TILE-10,T=6;
    for(const [x,y,a,b,v] of s.doors){
      const X=ox+x*TILE,Y=oy+y*TILE;
      if(v) g.fillRect(X-T/2,Y+(TILE-L)/2,T,L); else g.fillRect(X+(TILE-L)/2,Y-T/2,L,T);
    }
  }
}
</script>"""


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    dat = FtlDat(argv[1])
    out = Path(argv[2] if len(argv) > 2 else Path(__file__).parent.parent / "build" / "preview.html")
    out.parent.mkdir(parents=True, exist_ok=True)
    ships = [load_ship(dat, *bp) for bp in blueprints(dat)]
    out.write_text(PAGE.replace("__SHIPS__", json.dumps(ships)), encoding="utf8")
    print(f"{len(ships)} ships -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
