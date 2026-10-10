# FTL 3DS

A from-scratch reimplementation of the FTL: Faster Than Light engine for Nintendo 3DS homebrew:
full runs from the hangar to the rebel flagship, on the 3DS's two screens.

This repository contains **no FTL code or assets**. The game loads `ftl.dat` from your own copy of FTL
(1.6 or later) at runtime. The engine is written from the community-documented data formats and
reverse-engineering notes, without decompiling the original game.

## Installing

You need your own copy of FTL: Faster Than Light, version 1.6 or later (Steam, GOG, Humble...).
Use an unmodded `ftl.dat`: Slipstream or Hyperspace mods change the file.

**Easiest (Windows):** run `FTL2CIA.exe` from a release. It finds FTL in any Steam library or GOG
folder (or you pick `ftl.dat`), asks where to save, and builds `ftl3ds.cia` with your game data
inside. Install that CIA on the 3DS with FBI. It contains your copy of FTL's data: don't share it.

**By hand:**

1. Copy `ftl.dat` from your FTL folder (Steam: right-click FTL > Manage > Browse local files) to
   `sdmc:/3ds/ftl3ds/ftl.dat` on the SD card.
2. Install the release's `ftl3ds.cia` with FBI, or copy `ftl3ds.3dsx` to `sdmc:/3ds/` and start it
   from the Homebrew Launcher.

For sound the SD card needs `sdmc:/3ds/dspfirm.cdc`; run [DSP1](https://github.com/zoogie/DSP1)
once to dump it if you don't have it (without it the game runs silently and says so).

The game reads `romfs:/ftl.dat` first, then `sdmc:/3ds/ftl3ds/ftl.dat`, then `sdmc:/ftl.dat`.
Saves, settings and profile progress live in `sdmc:/3ds/ftl3ds/`.

It also runs in Azahar or Citra with `ftl.dat` at the same path on the emulator's SD card; there any
file named `3ds/dspfirm.cdc` enables sound.

## What's in the game

A full FTL run: the hangar (every ship and A/B/C layout, easy / normal / hard), eight sectors with
FTL's own events, stores, upgrades and equipment, every system, weapon, drone and augment, crew
with skills, boarding, environmental hazards, and the rebel flagship in the Last Stand. Ship
unlocks and achievements work as in FTL. There's also a SANDBOX mode (any ship with any weapons,
drones and augments; no unlocks). Numbers come from the FTL wiki, Mike Hopley's measurements and
xftl's reverse engineering; the few no source gives are marked as our own choice in the code.

## What's missing

- **The tutorial.**
- **The secret Crystal sector** (so the Ancestry achievement can't be earned).
- **Some event scripting:**
  - `<boarders>`: events where enemy boarders beam aboard (58 uses in FTL's events) do nothing.
  - `<status>`: events that limit or damage your systems for the fight (79 uses) are ignored.
  - `<crew>` and `<weaponOverride>` on event ships: their scripted crew and weapons are
    generated randomly instead.
  - `<fleet>` backdrops (the rebel fleet behind you), `<repair>` and `<secretSector>`.
- **Languages:** English text only; FTL's translations aren't loaded.
- **Mods:** Slipstream / Hyperspace mods aren't supported (use an unmodded `ftl.dat`).
- **A 3D home-menu banner:** tried, but it crashed the real HOME Menu; the CIA has a flat one.

Some details are our own choices where no source gives FTL's (door and jump animation speeds,
trained crew in stores, a few rates), and play is mostly by touch.

## Controls

The touchscreen has four tabs (tap them, or press X): **CREW** (your ship map, crew bar, door and
station buttons), **FIGHT** (the enemy map for targeting, a card per weapon, AUTO fire), **SYS**
(drones and special systems) and **POWER** (the reactor and every system's bars). Between fights
the top-left buttons are SHIP and JUMP.

| Input | Action |
|---|---|
| Tap a crew member (map or crew bar), or drag a box | Select them |
| Tap a room, or another crew member's tile | Send the selected crew there (station first, then the nearest free tile) |
| Tap a door | Hold it open / close it (airlocks vent the room to space) |
| Tap a weapon card, then an enemy room | Power the weapon and target that room |
| Beams: tap the card, then drag across the enemy | Set the beam's path; rooms it will hit turn red |
| Tap a drone card or a system card (SYS tab) | Deploy / activate it |
| Tap a power bar or system icon | Set the system's power |
| A, or the PAUSE button | Pause / resume (A after losing: back to the menu) |
| X | Next tab |
| Y / B | Select the next crew member / deselect |
| START | Pause menu |
| SELECT | Debug menu (any ship and enemy, quick actions) |
| L + R + Y / L + R + B | Debug: fire and breach on your ship / destroy the enemy |

## Building

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with **3DS Development** selected.
2. In the devkitPro MSYS2 terminal, install the extra libraries:
   ```
   pacman -S 3ds-zlib 3ds-libpng 3ds-libvorbisidec
   ```
3. From the same terminal, run `make` in this folder. This produces `ftl3ds.3dsx` and `ftl3ds.elf`.

### CIAs and releases

These need makerom.exe (3DSGuy/Project_CTR) in `tools/cia/` and bannertool
(diasurgical/bannertool) in `tools/cia/bt/`, and `make` run first.

- **Release:** `tools/release.sh [version]` packs `dist/ftl3ds-<version>.zip`: FTL2CIA, the
  `.3dsx`, a public CIA (`PUBLIC=1 tools/cia/build_cia.sh`: empty romfs, generated starfield icon
  and banner) and install notes. It refuses to package anything that looks like it holds an
  `ftl.dat`.
- **FTL2CIA:** `tools/installer/build_installer.sh` packages `tools/installer/ftl2cia.py` as
  `dist/FTL2CIA.exe` (needs Python with PyInstaller, pefile and Pillow). It carries only the
  engine, makerom, bannertool and the generated banner.
- **Personal CIA:** `tools/cia/build_cia.sh "<path to your ftl.dat>"` builds `ftl3ds.cia` with your
  `ftl.dat` inside and FTL's own icon. For your own console only.

`.gitignore` keeps `ftl.dat` (any letter case), romfs folders, CIAs, builds and anything else made
from FTL's files out of the repository.

## Scenario tests

Testing has its own build, so the game you play never contains test code (and never reads a
`test.txt`):

```
make TEST=1                                          # builds ftl3ds_test.3dsx in build_test/
powershell tools/test/run_scenario.ps1 tools/test/scenarios/boss1.txt
powershell tools/test/run_all.ps1 [-Filter hazard_*]
```

A scenario (`tools/test/scenarios/*.txt`) sets a situation up and then runs timed commands inside
the game, acting on its state directly rather than by tapping the screen:

```
boss 1                     # setup: a fight with flagship stage 1 (see source/testhooks.h for all keys:
ship 1                     #   ship, enemy, event, crew, tab, seed, weapons, enemyfire, enemyhull,
tab fight                  #   paused, board, skills, memory, env, boss, laststand, title)
at 1 expect artillery == 4 # PASS or FAIL in the log
at 2 do target 1 shields   # act: target, destroy, jump, wait, choose, key, tap, ... (test/test_exec.inc)
at 3 shot fighting         # the runner screenshots the Azahar window
at 4 log state             # a line of game state (fps, memory and more)
at 5 done
```

The runner copies the scenario to the emulated SD card, starts Azahar with the test build, follows
`test.log`, saves screenshots to `build_test/scenarios/<name>/`, and ends with a pass / fail count.
It won't take over the screen while someone is using the PC (`-Force` to override). Test runs keep
their own profile (`profile_test.txt`, everything unlocked).

## PC tools

- `tools/ftldat.py list|extract <ftl.dat> ...` lists and extracts archive files.
- `tools/preview.py <ftl.dat>` writes `build/preview.html`, which renders every player ship with the
  same maths as the 3DS code.

## Code layout

| File | Purpose |
|---|---|
| `source/main.c` | Main loop, menus, hangar, cameras, drawing, touch input, achievements hooks |
| `source/combat.c` | Weapons, shields, evasion, projectiles, beams, drones, boarding, hacking, mind control, hazards, the flagship, enemy AI |
| `source/run.c` | Runs: sector and beacon maps, events, rewards, stores, the rebel fleet, the Last Stand |
| `source/ship.c` | Blueprints, layouts, the walkable grid, power, enemy ship generation |
| `source/crew.c` | Crew races, names, spawning, pathfinding, movement, manning, skills |
| `source/life.c` | Doors, oxygen, fires, breaches, suffocation, medbay and clone bay |
| `source/weapon.c` | Weapon and drone blueprints, blueprint lists |
| `source/items.c` | Store items, systems for sale and their prices |
| `source/profile.c` | Ship unlocks, achievements and victories (`profile.txt`) |
| `source/save.c` | Saving and continuing a run |
| `source/sound.c` | Sound effects (WAV/OGG, resampled to the DSP's rate) and streamed music |
| `source/font.c` | FTL's bitmap fonts |
| `source/anim.c` | Sprite sheets and animations from `animations.xml` |
| `source/texture.c` | PNG to 3DS GPU textures |
| `source/text.c` | Display text from the `text_*.xml` files |
| `source/xmldom.c`, `source/xml.c` | XML trees (events) and a light tag scanner |
| `source/ftldat.c` | `ftl.dat` (PKG) archive reader |
| `source/system.c` | System types, subsystems and default stations |
| `source/feats.h`, `source/env.h` | Achievement bookkeeping; hazards and difficulty |
| `test/` | The test build's scenario runner (`make TEST=1` only) |
| `tools/` | CIA and release scripts, FTL2CIA, scenario runner, PC tools |
