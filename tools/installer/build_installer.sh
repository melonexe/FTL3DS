#!/bin/sh
# Packages the CIA builder as one Windows program: dist/FTL2CIA.exe.
# Players run it, pick their ftl.dat and get an installable CIA. It carries the engine
# (ftl3ds.elf), makerom, bannertool and the generated (non-FTL) banner and icon: nothing of
# Subset Games'.
#
#   make                                  (devkitPro shell: builds ftl3ds.elf)
#   sh tools/installer/build_installer.sh (needs Python with pyinstaller, pefile and pillow)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CIA="$ROOT/tools/cia"
cd "$HERE"

[ -f "$ROOT/ftl3ds.elf" ] || { echo "error: run make first" >&2; exit 1; }
[ -f "$CIA/makerom.exe" ] && [ -f "$CIA/bt/windows-x86_64/bannertool.exe" ] ||
    { echo "error: put makerom.exe in tools/cia/ and bannertool in tools/cia/bt/ (see README)" >&2; exit 1; }

rm -rf payload build
mkdir -p payload
(cd "$CIA" && python make_art.py)  # the generated starfield icon, banner and jingle
"$CIA/bt/windows-x86_64/bannertool.exe" makebanner -i "$CIA/banner.png" -a "$CIA/banner.wav" -o payload/banner.bnr
cp "$CIA/icon.png" payload/icon.png
cp "$ROOT/ftl3ds.elf" "$CIA/ftl3ds.rsf" payload/
cp "$CIA/makerom.exe" payload/makerom.exe
cp "$CIA/bt/windows-x86_64/bannertool.exe" payload/bannertool.exe

python -m PyInstaller --noconfirm --onefile --windowed --name FTL2CIA \
    --add-data "$(cygpath -w "$HERE/payload");payload" --hidden-import pefile \
    --distpath "$ROOT/dist" --workpath build --specpath build "$(cygpath -w "$HERE/ftl2cia.py")"
rm -rf build
ls -l "$ROOT/dist/FTL2CIA.exe"
