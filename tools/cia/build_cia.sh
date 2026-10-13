#!/bin/sh
# Builds a CIA. Run `make` first (it uses ftl3ds.elf).
#
#   PUBLIC=1 tools/cia/build_cia.sh
#       ftl3ds_public.cia, for sharing: nothing of FTL's inside (an empty romfs, the generated
#       starfield icon and banner from make_art.py). Players put their own ftl.dat on the SD card
#       at sdmc:/3ds/ftl3ds/ftl.dat.
#
#   tools/cia/build_cia.sh "<path to ftl.dat>"
#       ftl3ds.cia, a personal build: your ftl.dat inside its romfs and FTL's own icon (from
#       FTLGame.exe next to ftl.dat, needs Python's pefile and pillow). It contains Subset Games'
#       files: keep it to yourself.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
DAT=$1
if [ "$PUBLIC" != 1 ] && [ ! -f "$DAT" ]; then
    echo "usage: tools/cia/build_cia.sh <path to your ftl.dat>   (or PUBLIC=1 for a shareable CIA)" >&2
    exit 1
fi
EXE="$(dirname "$DAT")/FTLGame.exe"
cd "$HERE"
BT=./bt/windows-x86_64/bannertool.exe

if [ "$PUBLIC" = 1 ]; then
    ROMFS=romfs_public  # no ftl.dat: the game reads the player's own from the SD card
    OUT="$ROOT/ftl3ds_public.cia"
    mkdir -p "$ROMFS"
    rm -f "$ROMFS"/*
    echo "Public build: no ftl.dat or FTL art inside." > "$ROMFS/README.txt"
else
    ROMFS=romfs
    OUT="$ROOT/ftl3ds.cia"
    mkdir -p romfs
    if [ ! -f romfs/ftl.dat ] || [ "$DAT" -nt romfs/ftl.dat ]; then cp "$DAT" romfs/ftl.dat; fi
    # Backdrops pre-encoded as ETC1 (needs devkitPro's tex3ds): they load several times faster.
    # The game falls back to decoding the PNGs if the pack is missing or from another ftl.dat.
    if [ ! -f romfs/backdrops.pak ] || [ romfs/ftl.dat -nt romfs/backdrops.pak ] ||
        [ make_backdrops.py -nt romfs/backdrops.pak ]; then
        python make_backdrops.py romfs/ftl.dat romfs/backdrops.pak || rm -f romfs/backdrops.pak
    fi
fi

python make_art.py
ICON=icon.png
BANNER="-i banner.png -a banner.wav"
SMDH_FLAGS="visible,allow3d,recordusage"
if [ ! -d pycgfx ]; then
    curl -sL https://github.com/skyfloogle/pycgfx/archive/refs/heads/main.zip -o pycgfx.zip &&
        unzip -q -o pycgfx.zip && mv pycgfx-main pycgfx && rm -f pycgfx.zip || true
fi
# The home-menu banner is the flat one (banner.png from make_art.py); the icon is FTL's own, taken
# from FTLGame.exe by make_banner3d.py. 3D banners (make_banner3d.py) crashed a real 3DS's home
# menu and are shelved: BANNER3D=full / spin / tested still build them, for testing only.
if [ "$PUBLIC" = 1 ]; then
    :  # the generated art only
elif [ -n "$BANNER3D" ]; then
    if [ "$BANNER3D" = full ]; then VARIANT=full LAYERS=planet,rebels,logo,kestrel; else VARIANT=spin LAYERS=logo; fi
    if [ -f "$EXE" ] && [ -d pycgfx ] &&
        BANNER_VARIANT=$VARIANT BANNER_LAYERS=$LAYERS python make_banner3d.py romfs/ftl.dat "$EXE" pycgfx banner3d; then
        ICON=banner3d/icon.png
        SMDH_FLAGS="visible,allow3d,recordusage,extendedbanner"
        if [ "$BANNER3D" = tested ] && [ -f banner3d/banner_logo_tested.cgfx ]; then
            BANNER="-ci banner3d/banner_logo_tested.cgfx -a banner.wav"
        else
            BANNER="-ci banner3d/banner.cgfx -a banner.wav"
        fi
    fi
elif [ -f "$EXE" ] && BANNER_ICON_ONLY=1 python make_banner3d.py romfs/ftl.dat "$EXE" pycgfx banner3d; then
    ICON=banner3d/icon.png
fi

$BT makebanner $BANNER -o banner.bnr
$BT makesmdh -s "FTL3DS" -l "FTL: Faster Than Light (engine port, test build)" -p "FTL3DS contributors" \
    -f "$SMDH_FLAGS" -i "$ICON" -o icon.smdh

./makerom.exe -f cia -o "$OUT" -elf "$ROOT/ftl3ds.elf" -rsf ftl3ds.rsf \
    -icon icon.smdh -banner banner.bnr -target t -DAPP_ROMFS="$ROMFS" -major 0 -minor 1 -micro 0
ls -l "$OUT"
