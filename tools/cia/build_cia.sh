#!/bin/sh
# Builds ftl3ds.cia for testing on a 3DS, with your own ftl.dat inside its romfs.
#   tools/cia/build_cia.sh "<path to ftl.dat>"
# Run `make` first (it uses ftl3ds.elf). The CIA contains your ftl.dat and FTL's art: keep it to
# yourself.
#
# Home-menu art: with Python's pillow, pefile, gltflib, numpy and soundfile installed, the icon is
# FTL's own (from FTLGame.exe next to ftl.dat) and the banner is 3D, built from the game's art
# (make_banner3d.py, converted with pycgfx, fetched on first use): the FTL logo spinning in front of
# a nebula. BANNER3D=full builds the whole scene instead (still crashes a real 3DS's home menu) and
# BANNER3D=tested uses the static logo banner first tested on hardware. Without those Python
# modules a generated starfield icon and flat banner are used (make_art.py).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DAT=${1:-"F:/SteamLibrary/steamapps/common/FTL Faster Than Light/ftl.dat"}
EXE="$(dirname "$DAT")/FTLGame.exe"
cd "$HERE"
BT=./bt/windows-x86_64/bannertool.exe

mkdir -p romfs
if [ ! -f romfs/ftl.dat ] || [ "$DAT" -nt romfs/ftl.dat ]; then cp "$DAT" romfs/ftl.dat; fi

python make_art.py
ICON=icon.png
BANNER="-i banner.png -a banner.wav"
if [ ! -d pycgfx ]; then
    curl -sL https://github.com/skyfloogle/pycgfx/archive/refs/heads/main.zip -o pycgfx.zip &&
        unzip -q -o pycgfx.zip && mv pycgfx-main pycgfx && rm -f pycgfx.zip || true
fi
if [ "$BANNER3D" = full ]; then
    VARIANT=full LAYERS=planet,rebels,logo,kestrel
else
    VARIANT=spin LAYERS=logo  # two materials, as in the banner tested on hardware
fi
if [ -f "$EXE" ] && [ -d pycgfx ] &&
    BANNER_VARIANT=$VARIANT BANNER_LAYERS=$LAYERS python make_banner3d.py romfs/ftl.dat "$EXE" pycgfx banner3d; then
    ICON=banner3d/icon.png
    if [ "$BANNER3D" = tested ] && [ -f banner3d/banner_logo_tested.cgfx ]; then
        BANNER="-ci banner3d/banner_logo_tested.cgfx -a banner.wav"
    elif [ "$BANNER3D" = full ]; then
        BANNER="-ci banner3d/banner.cgfx -a banner3d/banner.wav"
    else
        BANNER="-ci banner3d/banner.cgfx -a banner.wav"  # the short jingle known to work
    fi
else
    echo "3D banner not built: using the generated art"
fi

$BT makebanner $BANNER -o banner.bnr
$BT makesmdh -s "FTL 3DS" -l "FTL: Faster Than Light (engine port, test build)" -p "Subset Games / FTL-port" \
    -i "$ICON" -o icon.smdh

./makerom.exe -f cia -o "$ROOT/ftl3ds.cia" -elf "$ROOT/ftl3ds.elf" -rsf ftl3ds.rsf \
    -icon icon.smdh -banner banner.bnr -target t -DAPP_ROMFS=romfs -major 0 -minor 1 -micro 0
ls -l "$ROOT/ftl3ds.cia"
