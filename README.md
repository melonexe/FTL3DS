# FTL3DS

A from-scratch reimplementation of the FTL: Faster Than Light engine for Nintendo 3DS homebrew:
full runs from the hangar to the rebel flagship, on the 3DS's two screens.

An unofficial fan project, not affiliated with or endorsed by Subset Games. FTL: Faster Than Light
is theirs; buy it if you haven't. This repository contains **no FTL code or assets**: the game loads
`ftl.dat` from your own copy of FTL (1.6 or later) at runtime. The engine is written from
community-documented data formats and reverse-engineering notes (xftl, the FTL wiki). Some gameplay
numbers were then checked against the game's executable, so they match FTL exactly. No FTL code or
assets are included.

## Work in progress

This is an unofficial fan project and still a work in progress: expect bugs, rough edges and the
odd crash, and don't expect it to match FTL perfectly yet (see [What's missing](#whats-missing)).
Back up your SD card's `3ds/ftl3ds/` folder if you care about your saves.

**Chat, help and news:** join the [FTL3DS Discord](https://discord.gg/vP9u78qRs). Bugs are best
reported as GitHub issues, so they don't get lost.

**Contributions are welcome:** bug reports (what happened, which ship and sector, a photo helps),
fixes, and work on anything in the missing list. If you open a pull request, never include
`ftl.dat` or anything extracted from it.

## Before you start: set up your 3DS

This is homebrew, so your 3DS needs custom firmware. Follow **[3ds.hacks.guide](https://3ds.hacks.guide/)**
from start to finish: it installs everything this game relies on (FBI to install CIAs, the Homebrew
Launcher, and the sound firmware homebrew needs). If your console is already set up that way,
you're ready.

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

If the game says "No sound", your console's setup is missing a step: see
[3ds.hacks.guide](https://3ds.hacks.guide/).

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

### Experimental: stereoscopic 3D

The [`experimental/stereo-3d`](https://github.com/melonexe/FTL3DS/tree/experimental/stereo-3d)
branch makes more of the 3D slider: shields become glassy domes bulging out of the screen, shots
arc out towards you, and drones and explosions float in front of the ships. It isn't in the
releases yet; build it from that branch (see [BUILDING.md](BUILDING.md)) and tell us on Discord how
it looks on your console.

## What's missing

- **The tutorial.**
- **The secret Crystal sector** (so the Ancestry achievement can't be earned).
- **Some event scripting:**
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
(drones and special systems) and **POWER** (the reactor and every system's bars). In a run, the
top-left buttons are SHIP (upgrades and equipment) and JUMP. As in FTL, JUMP needs the FTL drive
charged, the engines powered and someone at a working helm; tapping it otherwise says what's wrong.

| Input | Action |
|---|---|
| Tap a crew member (map or crew bar), or drag a box | Select them |
| Tap a room, or another crew member's tile | Send the selected crew there (station first, then the nearest free tile) |
| Tap a door | Hold it open / close it (airlocks vent the room to space), even while paused |
| Open all doors button (CREW tab) | Opens the inside doors; tap again to open the airlocks too |
| Close all doors button | Closes every door |
| Tap a weapon card, then an enemy room | Power the weapon and target that room |
| Beams: tap the card, then drag across the enemy | Set the beam's path; rooms it will hit turn red |
| Tap a drone card or a system card (SYS tab) | Deploy / activate it |
| Tap a power bar or system icon | Set the system's power |
| A, or the PAUSE button | Pause / resume (A after losing: back to the menu) |
| X | Next tab |
| Y / B | Select the next crew member / deselect |
| START | Pause menu |

## Building

**[BUILDING.md](BUILDING.md)** has the full guide: setting up devkitPro, Python, makerom,
bannertool and Azahar, then every build (the game, the test build and scenario tests, personal and
public CIAs, FTL2CIA, releases), the PC tools and troubleshooting.

The short version, in the devkitPro MSYS2 shell:

```
pacman -S 3ds-zlib 3ds-libpng 3ds-libvorbisidec   # once
make                                               # ftl3ds.3dsx and ftl3ds.elf
make TEST=1                                        # the test build (ftl3ds_test.3dsx)
```

then, for CIAs and releases (Git Bash, with makerom and bannertool in `tools/cia/`):

```
sh tools/cia/build_cia.sh "<path to ftl.dat>"     # personal CIA, your ftl.dat inside
sh tools/release.sh <version>                     # release zip: FTL2CIA, public CIA, .3dsx
```

`.gitignore` keeps `ftl.dat` (any letter case), romfs folders, CIAs, builds and anything else made
from FTL's files out of the repository.

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
| `test/` | The test build's scenario runner (`make TEST=1` only); `test/headless/` builds it for a PC, with nothing drawn |
| `tools/` | CIA and release scripts, FTL2CIA, scenario runners (Azahar and headless), the test server, PC tools |

## Licences

FTL3DS's code is under the [MIT licence](LICENSE). FTL: Faster Than Light and its data belong to
Subset Games and aren't covered by it.

FTL2CIA bundles makerom, bannertool, pefile, Pillow and the Python runtime; their notices are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) (and in every release zip).
