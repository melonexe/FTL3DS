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

**Menus and saving.** FTL's main menu: CONTINUE, NEW GAME, SANDBOX, STATS, OPTIONS, QUIT. The
hangar picks the ship, its type (A/B/C) and the difficulty (easy / normal / hard: starting scrap,
rewards, enemy strength and targeting as in FTL). The game saves whenever the ship is at rest and
CONTINUE resumes there; the run's end deletes the save. START pauses (resume, options with music
and effects volume, main menu, quit).

**Runs.** Eight sectors on FTL's sector map, each a map of beacons generated the way FTL does it
(xftl's notes), with FTL's own event scripts from `ftl.dat`: text, choices (blue options for a
system, crew race, weapon or augment you have), rewards from FTL's loot tables, crew joining or
leaving (a clone bay can save some), damage, hostile and friendly ships, quests and stores.
Jumping costs 1 fuel and the rebel fleet advances each turn (slower in nebulas); with no fuel, WAIT.
- **The map** marks what FTL marks (wiki: Beacon): the exit always, quests from any distance,
  stores and distress beacons once you've been next to them. Long-Ranged Scanners add ships and
  hazards at neighbouring beacons; nebulas show as clouds.
- **Stores** have FTL's sections (systems, weapons, drones, augments, crew; layout rules from
  xftl), stock from the sector's loot table, fuel, missiles, drone parts, hull repair and a SELL
  tab at half price. Each store remembers its stock, so you can go back. Crew for hire may come
  trained, more often later in the run (our numbers: the wiki gives none).
- **Ships at beacons:** an event's ship is in view while its text is up; one that surrenders or
  makes peace, or whose crew you killed, stays until you jump. Enemies surrender or try to escape
  at their scripted hull thresholds, and an escaping ship charges its FTL drive.
- **Enemies** are built with FTL's generator as reverse-engineered by xftl: power pools by sector
  and difficulty, optional systems, and weapons drawn from each race's list to fill the weapons
  system's power.
- **The SHIP menu** (locked in a fight, with intruders aboard or in a dangerous hazard) has
  upgrades at FTL's costs and equipment: weapon and drone slots, augments and a 4-slot cargo hold.
- **Hazards** (wiki: Environmental Hazards; xftl): asteroid fields, suns (solar flares), pulsars
  (ion pulses), anti-ship batteries, nebulas (blinded sensors, drifting clouds on screen) and ion
  storms (half the reactor). A badge names the hazard and counts down; with a hazard the FTL drive
  has to charge even without an enemy.
- **The Last Stand** (wiki: The Rebel Flagship; xftl FlagshipBoss): the flagship heads for the
  Federation base, jumping every second turn; three turns there and the run is lost. Its three
  stages: four weapon rooms with hacking and cloaking; drones and power surges; a 12-point Zoltan
  shield, teleporter and mind control with laser surges. Its crew carries over; with them all dead
  its AI flies it. Beating stage 3 wins.

**Combat.** Your ship on the top screen, the enemy in a panel beside it.
- **Weapons:** lasers, missiles, ion, beams, flak, bombs, chain and charge weapons, stun, lockdown
  and healing bombs, with the game's stats, art and sounds and Mike Hopley's measured speeds.
  Shields stop shots layer by layer; missiles and bombs ignore them; beams lose a point per layer.
  Hit or miss is decided as a shot arrives (so cloaking dodges shots already on their way).
- **Systems:** shields, engines, piloting, oxygen, medbay or clone bay, sensors, doors, weapons,
  drones, cloaking, backup battery, teleporter, hacking, mind control and artillery, each with
  FTL's numbers.
- **Drones:** combat, defence, anti-drone, hull repair, shield, crew repair, anti-personnel,
  boarding and ion intruder drones.
- **Boarding:** teleport crew over and back; crew fight hand to hand and sabotage systems; enemy
  ships board you and recall hurt boarders; killing the whole crew wins without the explosion.
- **Augments:** every obtainable augment works, including each ship's own.
- **Crystal crew** can lock a room down; **Zoltan crew** power the system they stand in.

**Crew.** Named from the game's lists; they walk, man stations, repair, fight fires, seal
breaches, suffocate and heal. Skills train as in FTL (wiki: Crew skills) and raise the manning
bonuses; cloning keeps 80% of the experience. Each room has its own oxygen (open doors share air;
airlocks, breaches and Lanius drain it; fires burn it); fires spread, damage and burn out.

**Unlocks and achievements.** Ships unlock as in FTL (wiki: Ships): the Kestrel at first; the
Engi on reaching sector 5 with it; other ships from their quests or by beating the flagship with
the ship before them; Type B from two of a ship's three achievements, Type C by reaching sector 8
with Type B; the Lanius after four ships, the Crystal after A and B wins with every other ship. All
of FTL's achievements are tracked, with popups and a list under STATS.

**Sandbox.** SANDBOX on the main menu opens the hangar with every ship unlocked, then a loadout
screen where each weapon, drone and augment slot can hold anything in the game (the weapons system
grows to power it). Sandbox runs earn no achievements or unlocks.

**On the 3DS.** FTL's own bitmap fonts at 1:1 pixels, the game's music (explore and battle
versions crossfading) and effects, stereoscopic 3D (stars behind, readouts in front), and assets
loaded and freed as needed to fit the 3DS's memory.

**Not there yet:** the tutorial, the secret Crystal sector (so Ancestry can't be earned), and a 3D
home-menu banner (tried; it crashed the real HOME Menu, so the CIA has a flat banner).

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

## Where the numbers come from

The FTL wiki (ftl.fandom.com), Mike Hopley's measurements (mikehopley.github.io) and xftl's reverse
engineering (gitlab.com/znixian/xftl): repair takes 12.5 s per bar for a Human, crew walk 2.2
tiles/s, shield layers recharge in 2 s, oxygen refills at 1.2 / 4.8 / 8.4 % per second, and so on.
The few values no source gives (door animation speed, the jump animation, some rates) are marked as
our own choice where they're defined.

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
