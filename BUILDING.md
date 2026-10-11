# Building FTL3DS

Everything you can build from this repository, how, and in what order. Players don't need any of
this: they download a release and run FTL2CIA (see the [README](README.md)).

```
source/ ──make──────────► ftl3ds.elf + ftl3ds.3dsx ─┬─► build_cia.sh <ftl.dat> ──► ftl3ds.cia         (personal: your ftl.dat inside)
                                                    ├─► PUBLIC=1 build_cia.sh ───► ftl3ds_public.cia  (nothing of FTL's)
                                                    ├─► build_installer.sh ──────► dist/FTL2CIA.exe   (players make their own CIA)
                                                    └─► release.sh <version> ───► dist/ftl3ds-<version>.zip
source/ + test/ ──make TEST=1──► ftl3ds_test.3dsx ──► run_scenario.ps1 / run_all.ps1  (automated tests in Azahar)
```

| Pipeline | Command | Output | Needs |
|---|---|---|---|
| [1. Game](#1-the-game-make) | `make` | `ftl3ds.elf`, `ftl3ds.3dsx` | devkitPro |
| [2. Test build](#2-the-test-build-make-test1) | `make TEST=1` | `ftl3ds_test.3dsx` | devkitPro, Azahar, your `ftl.dat` |
| [3. Personal CIA](#3-personal-cia) | `sh tools/cia/build_cia.sh "<ftl.dat>"` | `ftl3ds.cia` (~280 MB) | 1, Python, makerom, bannertool |
| [4. Public CIA](#4-public-cia) | `PUBLIC=1 sh tools/cia/build_cia.sh` | `ftl3ds_public.cia` (~0.7 MB) | 1, Python, makerom, bannertool |
| [5. FTL2CIA](#5-ftl2cia) | `sh tools/installer/build_installer.sh` | `dist/FTL2CIA.exe` (~32 MB) | 1, Python + PyInstaller, makerom, bannertool |
| [6. Release](#6-a-release) | `sh tools/release.sh <version>` | `dist/ftl3ds-<version>.zip` | 1, everything for 4 and 5 |

Steps 3–6 are Windows-only as written (they run `makerom.exe` and `bannertool.exe`).

**Never commit `ftl.dat`, anything extracted from it, or a CIA.** `.gitignore` blocks them (`*.dat`
in any letter case, `romfs/` folders, `*.cia`, `dist/`), but `git add -f` would get past it.

---

## Setting up

### devkitPro (for the game)

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with **3DS Development** ticked.
2. Open the **devkitPro MSYS2** shell (Start menu) and install the extra libraries:
   ```
   pacman -S 3ds-zlib 3ds-libpng 3ds-libvorbisidec
   ```
   (libctru, citro2d and citro3d come with 3DS Development.)

### Python (for CIAs, FTL2CIA and the PC tools)

Python 3.10 or later on Windows, with:

```
pip install pillow pefile pyinstaller
```

- `pillow` and `pefile`: FTL's own icon, taken from `FTLGame.exe` (personal CIAs, FTL2CIA).
- `pyinstaller`: packages FTL2CIA as one `.exe`.
- The generated banner (`tools/cia/make_art.py`) needs nothing beyond Python itself.

Run steps 3–6 from **Git Bash** (or any shell with `sh` and `python` on the path), from the
repository's root folder.

### makerom and bannertool (for every CIA)

These aren't in the repository (they're third-party Windows programs). Download them once:

- **makerom** from [3DSGuy/Project_CTR releases](https://github.com/3DSGuy/Project_CTR/releases):
  put `makerom.exe` at `tools/cia/makerom.exe`.
- **bannertool** from [diasurgical/bannertool releases](https://github.com/diasurgical/bannertool/releases):
  unzip it so the program is at `tools/cia/bt/windows-x86_64/bannertool.exe`.

Both are MIT-licensed; their notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
which ships with every release because FTL2CIA bundles them.

### Azahar (for testing)

- Install [Azahar](https://azahar-emu.org/) at its default path, `C:\Program Files\Azahar\azahar.exe`
  (the test scripts expect it there).
- Copy your `ftl.dat` to the emulated SD card: `%APPDATA%\Azahar\sdmc\3ds\ftl3ds\ftl.dat`.
- For sound, put any file named `dspfirm.cdc` in `%APPDATA%\Azahar\sdmc\3ds\`. Azahar emulates the
  DSP itself, so it doesn't need to be a real firmware dump.

---

## 1. The game: `make`

In the devkitPro MSYS2 shell, in the repository folder:

```
make          # builds into build/
make clean    # removes build/ and the outputs
```

Outputs:

- `ftl3ds.elf`: the program. Every CIA is made from it.
- `ftl3ds.3dsx` (+ `ftl3ds.smdh`): for the Homebrew Launcher. Copy it to `sdmc:/3ds/` and put
  `ftl.dat` at `sdmc:/3ds/ftl3ds/ftl.dat`.

To run it in Azahar: `"C:\Program Files\Azahar\azahar.exe" ftl3ds.3dsx`.

The build uses `-Wall` and is warning-free; keep it that way. For a stricter look,
`arm-none-eabi-gcc -fsyntax-only -Wall -Wextra ...` over `source/*.c` shows only a few expected
"unused parameter" warnings on library callbacks.

## 2. The test build: `make TEST=1`

```
make TEST=1   # builds into build_test/, outputs ftl3ds_test.3dsx
```

The test build adds `test/` (the scenario runner), the debug controls (SELECT debug menu, L+R+Y fire
and breach, L+R+B destroy the enemy) and its own profile (`profile_test.txt`, every ship unlocked).
The normal build contains none of this and never reads `test.txt`.

Run scenarios from PowerShell:

```
powershell tools/test/run_scenario.ps1 tools/test/scenarios/boss1.txt [-Force] [-Timeout 120]
powershell tools/test/run_all.ps1 [-Filter hazard_*] [-Force]
```

Each run copies the scenario to Azahar's SD card as `test.txt`, starts Azahar with the test build,
follows `test.log`, screenshots the window at each `shot` line into
`build_test/scenarios/<name>/`, and prints the log with a PASS / FAIL / ERROR count (exit code 1 on
any failure). Screenshots bring Azahar to the front, so the runner won't start if the PC was used
in the last two minutes; `-Force` overrides that.

Scenario files (`tools/test/scenarios/*.txt`) are documented at the top of
[source/testhooks.h](source/testhooks.h) (setup keys) and in [test/test_exec.inc](test/test_exec.inc)
(`do` actions and the values `expect` can check). A minimal one:

```
ship 1
enemy REBEL_FAT
tab fight
seed 3
at 1 expect enemy == 1
at 2 shot start
at 3 log state
at 4 done
```

`log state` includes `fps`, `cpums` (CPU time per frame), memory figures and, after a jump,
`jumpms` / `shiploadms` (how long the beacon change and the arriving ship's load took). Note that
the emulator models CPU time but not GPU cost.

## 3. Personal CIA

```
sh tools/cia/build_cia.sh "C:/path/to/FTL Faster Than Light/ftl.dat"
```

Builds `ftl3ds.cia` with your `ftl.dat` inside, so nothing goes on the SD card. Step by step:

1. `tools/cia/make_art.py` draws the flat starfield banner, the fallback icon and the banner jingle
   (no FTL art).
2. If `FTLGame.exe` sits next to `ftl.dat`, FTL's own icon is taken from it
   (`make_banner3d.py` in icon-only mode).
3. `ftl.dat` is copied to `tools/cia/romfs/` (only when it's newer).
4. bannertool makes `banner.bnr` and `icon.smdh` (title "FTL3DS").
5. makerom packs `ftl3ds.elf`, `tools/cia/ftl3ds.rsf`, the icon, the banner and the romfs into
   `ftl3ds.cia`.

The game reads `romfs:/ftl.dat` first, so this CIA ignores any `ftl.dat` on the SD card. It contains
Subset Games' data: **for your own console only.** Install with FBI.

The shelved 3D home-menu banners still build with `BANNER3D=full`, `spin` or `tested` in front of
the command (they need `gltflib` and `pycgfx`, and crashed a real HOME Menu), for testing only.

## 4. Public CIA

```
PUBLIC=1 sh tools/cia/build_cia.sh
```

Same script, but the romfs (`tools/cia/romfs_public/`) holds only a README, and the icon and banner
are the generated ones. The result, `ftl3ds_public.cia`, is safe to share: players install it and
put their own `ftl.dat` at `sdmc:/3ds/ftl3ds/ftl.dat`.

## 5. FTL2CIA

```
sh tools/installer/build_installer.sh
```

1. Gathers a payload in `tools/installer/payload/`: `ftl3ds.elf`, `ftl3ds.rsf`, the generated
   `banner.bnr` and `icon.png`, `makerom.exe`, `bannertool.exe`.
2. PyInstaller packs [tools/installer/ftl2cia.py](tools/installer/ftl2cia.py) and the payload into
   one program, `dist/FTL2CIA.exe`.

On a player's PC, FTL2CIA finds FTL (any Steam library, or GOG), copies their `ftl.dat` into a
temporary romfs, takes FTL's icon from `FTLGame.exe` if it can, and runs bannertool and makerom: the
same as step 3, with their files, on their machine.

To try it without packaging: `python tools/installer/ftl2cia.py` (after running the script above
once, so `payload/` exists). Run from a plain source download, it has no payload and reports
"The builder is missing ftl3ds.elf", which is expected: players must use the release's `.exe`.

The `.exe` isn't code-signed, so Windows SmartScreen may warn players ("More info → Run anyway").

## 6. A release

1. In the devkitPro shell: `make clean && make` (a fresh normal build).
2. In Git Bash: `sh tools/release.sh 0.1.1` (any version; today's date if left out).

`release.sh` runs steps 4 and 5, then fills `dist/ftl3ds-<version>/` and zips it:

| File | For |
|---|---|
| `FTL2CIA.exe` | Players: the easy route to a CIA |
| `ftl3ds.cia` | The public CIA (+ `ftl.dat` on the SD card) |
| `ftl3ds.3dsx` | The Homebrew Launcher |
| `INSTALL.txt` | Setup steps (3ds.hacks.guide first), the Discord link |
| `LICENSE`, `THIRD_PARTY_NOTICES.md` | MIT licence; bundled tools' licences |

It refuses to package any `.dat` file, or a CIA over 20 MB (one that might hold `ftl.dat`).

3. Test it: run `FTL2CIA.exe` from that folder, install the CIA it makes on a 3DS, start a run.
4. On GitHub: **Releases → Draft a new release**, tag `v0.1.1`, attach the zip. Say in the
   description to download the zip, not "Source code" (GitHub adds those automatically, and they
   can't build a CIA on their own).

## PC tools

From any shell with Python:

- `python tools/ftldat.py list <ftl.dat>` / `extract <ftl.dat> <name> ...`: list and extract files
  from the archive (for reading FTL's XML; keep the results out of the repository).
- `python tools/preview.py <ftl.dat>`: writes `build/preview.html`, rendering every player ship with
  the same maths as the game.

## Troubleshooting

| Problem | Cause |
|---|---|
| `make`: `arm-none-eabi-gcc` not found | Run it in the devkitPro MSYS2 shell, not Git Bash or PowerShell |
| `make`: missing `png.h`, `zlib.h` or `ivorbisfile.h` | `pacman -S 3ds-zlib 3ds-libpng 3ds-libvorbisidec` |
| `build_cia.sh`: usage message | Give the path to `ftl.dat`, or use `PUBLIC=1` |
| `build_cia.sh` / `build_installer.sh`: makerom or bannertool not found | See [makerom and bannertool](#makerom-and-bannertool-for-every-cia) |
| `release.sh`: "run make first" | Build the game (step 1) first |
| FTL2CIA: "missing ftl3ds.elf" | Running `ftl2cia.py` from source: use the packaged `.exe`, or run `build_installer.sh` first |
| The game says "Couldn't load ftl.dat" | Not at `sdmc:/3ds/ftl3ds/ftl.dat`, or from FTL before 1.6 |
| The game says "No sound" | No `sdmc:/3ds/dspfirm.cdc`: on a 3DS, see 3ds.hacks.guide; in Azahar, any file with that name |
| A test run says "PC in use" | Someone used the PC in the last two minutes; wait, or `-Force` |
| A HOME Menu banner doesn't update | The HOME Menu caches banners: reboot, or reinstall the CIA |

## Automatic builds (not set up)

A GitHub Actions workflow could run steps 1, 4, 5 and 6 whenever a version tag is pushed and attach
the zip to the release, so binaries never live in git but are always fresh. devkitPro publishes a
Docker image for step 1; makerom, bannertool and PyInstaller would need a Windows runner (or Linux
builds of the tools and a Linux FTL2CIA).
