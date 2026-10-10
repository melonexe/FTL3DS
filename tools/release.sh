#!/bin/sh
# Packages a release for sharing: dist/ftl3ds-<version>.zip with the .3dsx, the public CIA and
# install notes. Nothing of FTL's goes in: players bring their own ftl.dat.
#
#   make clean && make            (in the devkitPro MSYS2 shell)
#   tools/release.sh [version]    (in a shell with Python, e.g. Git Bash)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${1:-$(date +%Y.%m.%d)}
cd "$ROOT"

if [ ! -f ftl3ds.elf ] || [ ! -f ftl3ds.3dsx ]; then
    echo "error: run make first" >&2
    exit 1
fi
PUBLIC=1 sh tools/cia/build_cia.sh
sh tools/installer/build_installer.sh  # dist/FTL2CIA.exe

OUT="dist/ftl3ds-$VERSION"
rm -rf "$OUT" "$OUT.zip"
mkdir -p "$OUT"
cp ftl3ds.3dsx ftl3ds_public.cia dist/FTL2CIA.exe THIRD_PARTY_NOTICES.md LICENSE "$OUT/"
mv "$OUT/ftl3ds_public.cia" "$OUT/ftl3ds.cia"
cat > "$OUT/INSTALL.txt" <<EOF
FTL3DS $VERSION - an engine for FTL: Faster Than Light on the Nintendo 3DS.

This contains no part of FTL. You need your own copy of FTL: Faster Than Light (version 1.6 or
later, e.g. Steam, GOG or Humble) for its data file, ftl.dat.

EASIEST (Windows): run FTL2CIA.exe. It finds FTL (or pick your ftl.dat), and makes
ftl3ds.cia with your game data inside. Install that CIA with FBI. Keep it to yourself: it contains
your copy of FTL's data.

OR, by hand:
1. Find ftl.dat in your FTL install folder (Steam: right-click FTL > Manage > Browse local files).
2. Copy it to your SD card as:   sdmc:/3ds/ftl3ds/ftl.dat
3. Install ftl3ds.cia with FBI (or another CIA installer), or copy ftl3ds.3dsx to sdmc:/3ds/ and
   start it from the Homebrew Launcher.

Your 3DS needs custom firmware: set it up with https://3ds.hacks.guide/ first. That guide also
installs FBI and the sound firmware homebrew needs (if the game says "No sound", see the guide).

Saves and progress are kept in sdmc:/3ds/ftl3ds/.

Help, questions and news: https://discord.gg/vP9u78qRs
EOF

# Nothing of Subset Games' may ride along: no ftl.dat, and the CIA's romfs must be the empty one.
if find "$OUT" -iname '*.dat' | grep -q .; then
    echo "error: a .dat file ended up in the release" >&2
    exit 1
fi
CIA_BYTES=$(wc -c < "$OUT/ftl3ds.cia")
if [ "$CIA_BYTES" -gt 20000000 ]; then  # ftl.dat is ~280 MB; the engine alone is a few MB
    echo "error: the CIA is $CIA_BYTES bytes: does its romfs hold an ftl.dat?" >&2
    exit 1
fi
(cd dist && python -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[1])" "ftl3ds-$VERSION")
ls -l "$OUT.zip"
