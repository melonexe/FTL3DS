# FTL 3DS

A from-scratch reimplementation of the FTL: Faster Than Light engine for Nintendo 3DS homebrew.

This repository contains **no FTL code or assets**. The game loads `ftl.dat` from your own copy of FTL
(1.6 or later) at runtime. The engine is written from the community-documented data formats, without
decompiling the original game.

## Status

**Stage 5: combat.** Pick any player ship (including the Advanced Edition and Type C layouts) and
fight random enemy ships from the game's own list. Destroy one and the next arrives a few seconds
later; lose your ship and press A to start again.

- **Ships:** your ship on the left of the top screen, the enemy in a panel on the right, each with
  hull, shields and evasion. Room interiors, system icons (green powered, grey off, orange/red
  damaged), doors and weapons on the hull all come from `ftl.dat`.
- **Crew:** named from the game's lists and placed at their stations. They walk through doors,
  man systems (pilot, engines, shields, weapons, sensors, doors), repair damaged systems in their
  room, suffocate without air and heal in the medbay.
- **Weapons:** lasers, missiles, ion weapons, beams, flak and bombs with the game's stats, art and
  sounds. Shields stop shots layer by layer and recharge; missiles and bombs ignore them; beams lose
  one damage per layer and sweep across rooms; flak spreads over an area; ion locks systems for 5 s
  per point. Engines and crew in the cockpit give evasion. Hits damage the hull, systems and crew and
  can start fires and hull breaches.
- **Drones:** combat drones (laser, beam, ion, missile) fly around the enemy and fire at random rooms;
  defence drones shoot down incoming missiles and flak (Mark II also lasers); anti-drones stun enemy
  drones; hull repair drones fix 3-5 hull; shield drones block a shot every 15 s. Deploying costs a
  drone part, and a destroyed drone takes 10 s before it can be redeployed. Inside drones: repair
  drones (25 HP) fight fires, seal breaches and repair at Engi speed; anti-personnel drones (150 HP)
  hunt boarders; boarding drones (150 HP) breach the enemy hull and fight or sabotage.
- **Hacking:** launch the hacking drone (1 part) at an enemy system; it can't be manned and repairs
  at half speed while attached. A pulse (4/7/10 s by power) shuts the system down (shields lose a
  layer every 2 s, weapons lose charge), then 20 s recharge.
- **Mind control:** turns one enemy crew member in a room for 14/20/28 s (+15/+30 HP and more
  damage at levels 2/3), then 20 s recharge. Slugs are immune.
- **Zoltan:** Zoltan crew add a free power bar to the system they stand in; the Zoltan shield
  (Zoltan ships) absorbs 5 points of damage (ion double, beams twice) and blocks boarding, hacking
  and mind control; it refills each fight.
- **Special weapons:** chain weapons charge faster with each volley, charge weapons store 2-3
  volleys and fire them together, stun effects (3 s from missiles, longer from stun weapons), and
  the Federation cruiser's artillery (charges in 50/40/30/20 s by power, fires an unaimed beam that
  pierces all shields).
- **Boarding:** the teleporter sends the crew standing in its room to an enemy room (20/15/10 s
  recharge by level) and RETURN brings them home. Crew in the same room fight (3-7 damage per hit,
  Mantis x1.5, Engi x0.5); boarders with no one to fight sabotage the room's system and move between
  system rooms on their own. Enemies with a teleporter board you twice per fight and recall hurt
  boarders. Killing the whole enemy crew wins the fight without destroying the ship. Hostile crew
  have red-framed health bars (red rims as markers).
- **Sensors:** level 1 shows your whole ship (otherwise only rooms with your crew), level 2 the
  enemy's interior and crew, level 3 the enemy's weapon charge; manning adds a level. Hidden rooms
  are dark but keep their system icons for targeting.
- **Life support:** each room has its own oxygen (the game's model: open doors share air, open
  airlocks, breaches and Lanius drain it, fires burn it). Fires spread between tiles and rooms,
  damage crew and systems, and burn out without air; crew in a room fight its fires first, then seal
  breaches, then repair its system, and the enemy sends crew to do the same.
- **Sound:** the game's music and effects.

The touchscreen has four tabs (tap them, or press X):

- **CREW:** a map of your ship with a crew bar underneath, plus OPEN ALL / CLOSE ALL for the doors.
- **FIGHT:** a map of the enemy ship for targeting, and a card per weapon with its charge, status and
  power button. AUTO keeps weapons firing at their target; with it off, each targeting fires one
  volley, as in the original.
- **SYS:** drone cards (tap to deploy / switch off) and special systems: cloaking (5 s per power,
  +60% evasion, enemy weapons stop charging, 20 s recharge), backup battery (+2 reactor bars per
  level for 30 s), the clone bay (revives dead crew in 12/9/7 s by power) and the teleporter (tap
  the card, then a room on the fight tab; its RETURN button brings your boarders back).
- **POWER:** the reactor and every system's power bars.

**Runs.** The game starts a run: eight sectors of beacons generated the way FTL does it (xftl's
notes on the sector map), with FTL's own event scripts from `ftl.dat`: the text, choices (blue
options when you have the system, crew race or weapon), rewards from FTL's loot tables, crew
joining or leaving, damage, hostile ships and stores. Between fights the PAUSE button becomes JUMP
and opens the sector map. Jumping costs 1 fuel and the rebel fleet (red) advances each jump. Tap a
blue-ringed beacon to jump; at the exit, NEXT SECTOR moves on. Stores sell fuel, missiles, drone
parts and hull repair. Winning a fight plays that ship's destroyed or crew-killed event.
Stores have FTL's sections (systems, weapons, drones, augments, crew; layout rules from xftl) and
a SELL tab at half price. The SHIP button opens upgrades (system levels and reactor bars at FTL's
costs) and equipment (weapon and drone slots, augments, a 4-slot cargo hold). Event rewards give real
weapons, drones, augments and crew. Enemies scale with the sector using FTL's generator (via xftl).
In a fight the FTL drive charges (wiki charge times by engine power) and JUMP escapes when it's full.
Text uses FTL's own bitmap fonts at 1:1 pixels.
Every obtainable augment works (wiki: Augmentations), including the ships' own (they take augment
slots, as in FTL): Defense Scrambler, Hacking Stun, Zoltan Shield Bypass, Fire Suppression, Backup
DNA Bank, Reconstructive Teleport, Adv. FTL Navigation (jump to any visited beacon), Distraction
Buoys, FTL Jammer, Lifeform Scanner (life signs in rooms the sensors can't see; a Slug aboard does
the same), Long-Ranged Scanners (ships and hazards at neighbouring beacons on the map), Crystal
Vengeance, Engi Med-bot Dispersal, Drone Reactor Booster, Mantis Pheromones, Rock Plating, Slug Repair
Gel, Titanium System Casing and the Zoltan shield, besides the re-loader, boosters, arms, armour and
the rest. Enemies surrender or try to escape at their scripted hull thresholds.
Lockdown: a Crystal crew member selected alone gets a LOCKDOWN button (crew tab) that coats their
room in crystal for 12 s (nobody walks in or out; 50 s recharge, ready again after a jump); the
Crystal Lockdown Bomb does the same to an enemy room. The Ion Intruder drone boards like a boarding
drone and every 8.2-10 s ionises the system it stands by (3 ion) and stuns the crew there for 6 s.
Stores stock each sector's loot table (its rarity list in `sector_data.xml`), and crew for hire may
come trained in one or two skills, more often later in the run (our numbers: the wiki gives none).
Crew skills (wiki: Crew skills): piloting and engines train on dodges, shields on blocked hits,
weapons on each volley, repair on each repaired bar, combat on kills and sabotage. Levels 1 and 2
raise the manning bonuses (evasion +5/7/10%, shield recharge +10/20/30%, weapon charge -10/15/20%,
FTL charge by the wiki's table), repair speed and hand-to-hand damage; cloning keeps 80% of the
experience. Select one crew member to see their skills at the bottom left of the top screen.
Beacon hazards (wiki: Environmental Hazards; xftl's environment code): asteroid fields fire waves
of asteroids at both ships, timed by our shield level; a sun sends solar flares that start fires;
a pulsar ionises systems; anti-ship batteries fire 3-damage, shield-piercing, breaching shots;
nebulas blind the sensors and slow the rebel fleet; ion storms halve the reactor. A badge on the
top screen names the hazard and counts down to the next flare, pulse or shot. With a hazard the
FTL drive has to charge even without an enemy, and the ship menu is locked.
The Last Stand (sector 8; wiki: The Rebel Flagship, xftl FlagshipBoss): the Federation base sits
in the map's middle columns and the rebel flagship heads for it, jumping every second turn (its
next jump is drawn on the map); three turns at the base and the game is lost. Meeting it starts
its stage: stage 1 with four weapon rooms, hacking and cloaking; stage 2 with drones and power
surges of six extra drones; stage 3 with a 12-point Zoltan shield, teleporter and mind control,
and surges of seven lasers (every fourth restores the shield). Each stage's weapons are artillery
systems at FTL's charge times, their guns drawn on the hull; its crew carries over between stages;
with them all dead the AI flies it (systems count as manned, damage repairs a bar per 20 s each,
our rate); beating stage 3 wins the run.
The game opens on FTL's main menu (CONTINUE, NEW GAME, OPTIONS, QUIT) with a hangar to choose the
ship, type and difficulty (easy / normal / hard: starting scrap, rewards and enemy targeting as in
FTL). It saves whenever the ship is at rest and CONTINUE resumes there; the run's end deletes the
save. START pauses (resume, options with music and effects volume, main menu, quit).
Ships unlock as in FTL (wiki: Ships): only the Kestrel at first; the Engi on reaching sector 5
with it; each other ship's Type A from its quest event or by beating the flagship with the ship
before it; Type B from two of the ship's three achievements, Type C by reaching sector 8 with
Type B; the Lanius after four ships, the Crystal after A and B victories with every other ship.
All of FTL's achievements (data/achievements.xml) are tracked, with a popup when earned and the
list under STATS; progress is kept in sdmc:/3ds/ftl3ds/profile.txt. Ancestry (the Crystal sector)
can't be earned yet: there's no secret sector.
With the 3D slider up the top screen is stereoscopic: the stars sit behind the ships and the
readouts in front. The flagship uses FTL's layout for the difficulty (hard links its weapon rooms
to the rest of the ship).

| Input | Action |
|---|---|
| Tap a crew member (on the map or in the crew bar) | Select them |
| Tap a room | Send the selected crew member there (its station first, then the nearest free tile) |
| Tap a door | Hold it open / close it (airlocks vent the room to space) |
| Tap a weapon card, then an enemy room | Power the weapon and target that room (tap the room again to cancel) |
| Beams: tap the card, then drag across the enemy | Set the beam's path; rooms it will hit turn red |
| Tap a weapon's ON/OFF button | Power it on or off |
| Tap a drone card (sys tab) | Deploy / power the drone, or switch it off |
| Tap Cloaking / Backup Battery (sys tab) | Activate it |
| Tap a power bar | Power the system up to that bar (tap the top lit bar to remove it) |
| Tap a system icon | Switch the system fully on or off |
| A, or the PAUSE button | Pause / resume (A after losing: restart the run) |
| JUMP (between fights) | Open the sector map |
| L + R + Y | Debug: start a fire and a hull breach on your ship |
| L + R + B | Debug: destroy the enemy ship |
| SELECT | Debug menu: pick any player ship and enemy and start that one-off fight, or a new run with that ship, plus quick actions (destroy enemy, kill crew, fire + breach, repair, boarding both ways, +missiles/parts/power/scrap/fuel, enemy hold fire, clear cooldowns, break enemy systems, full sensors) |
| X | Next tab |
| Y / B | Select next crew member / deselect |
| START | Pause menu (options, main menu, quit) |

Game numbers come from the FTL wiki (ftl.fandom.com), Mike Hopley's measurements
(mikehopley.github.io) and xftl's reverse engineering (gitlab.com/znixian/xftl): repair takes 12.5 s per bar for a Human, crew walk 2.2 tiles/s, shield layers
recharge in 2 s, oxygen refills at 1.2 / 4.8 / 8.4 % per second, weapons travel at their game speeds,
fires spread on a 10-49 countdown, and so on. The few values no source gives (door animation speed,
beam sweep time) are marked as our own choice at the top of `combat.c` and `life.c`.

## Building

1. Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with **3DS Development** selected.
2. In the devkitPro MSYS2 terminal, install the extra libraries:
   ```
   pacman -S 3ds-zlib 3ds-libpng 3ds-libvorbisidec
   ```
3. From the same terminal, run `make` in this folder. This produces `ftl3ds.3dsx`.

## Running

1. Copy `ftl.dat` from your FTL install folder to `sdmc:/3ds/ftl3ds/ftl.dat` on your SD card.
   Use an unmodded copy: Slipstream or Hyperspace mods change the file.
2. Copy `ftl3ds.3dsx` to `sdmc:/3ds/` and launch it from the Homebrew Launcher.
3. For sound, your SD card needs `sdmc:/3ds/dspfirm.cdc`. If you don't have it, run the
   [DSP1](https://github.com/zoogie/DSP1) homebrew once to dump it from your console. Without it the
   game runs silently and says so on the touchscreen.

You can also run it in an emulator such as Azahar or Citra by putting `ftl.dat` at the same path on the
emulator's virtual SD card. Emulators emulate the DSP themselves, so any file named
`3ds/dspfirm.cdc` on the virtual SD card enables sound; it doesn't need to be a real dump.

## Test CIA (personal use)

`tools/cia/build_cia.sh "<path to your ftl.dat>"` (after `make`) builds `ftl3ds.cia` with your own
ftl.dat inside its romfs, so nothing needs copying to the SD card. It downloads nothing: put
makerom.exe (3DSGuy/Project_CTR) and bannertool (diasurgical/bannertool, in `tools/cia/bt/`) in
`tools/cia/` first. The CIA contains Subset Games' data, so it's for testing on your own console
only; never share it. The game reads `romfs:/ftl.dat` first, then the SD card paths.

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
tab fight                  #   paused, board, skills, memory, env, boss, laststand)
at 1 expect artillery == 4 # PASS or FAIL in the log
at 2 do target 1 shields   # act: target, destroy, jump, wait, choose, surge, ... (test/test_exec.inc)
at 3 shot fighting         # the runner screenshots the Azahar window
at 4 log state             # a line of game state
at 5 done
```

The runner copies the scenario to the emulated SD card, starts Azahar with the test build, follows
`test.log`, saves screenshots to `build_test/scenarios/<name>/`, and ends with a pass / fail count.
It won't take over the screen while someone is using the PC (`-Force` to override).

## PC tools

- `tools/ftldat.py list|extract <ftl.dat> ...` lists and extracts archive files.
- `tools/preview.py <ftl.dat>` writes `build/preview.html`, which renders every player ship with the
  same maths as the 3DS code. It's useful for checking layout changes without a console.

## Code layout

| File | Purpose |
|---|---|
| `source/ftldat.c` | `ftl.dat` (PKG) archive reader with zlib inflation |
| `source/texture.c` | PNG to 3DS GPU texture conversion (tiling, power-of-two padding) |
| `source/xml.c` | Minimal tag/attribute scanner for FTL's XML |
| `source/ship.c` | Blueprints (crew, systems, reactor), layout parsing, walkable tile grid, power |
| `source/anim.c` | Sprite sheets and animations from `animations.xml` / `dlcAnimations.xml` |
| `source/system.c` | System types, subsystems and default stations |
| `source/text.c` | Display names from `text_blueprints.xml` / `text_misc.xml` |
| `source/weapon.c` | Weapon blueprints and blueprint lists |
| `source/combat.c` | Weapons, shields, evasion, projectiles, damage, repairs, enemy AI |
| `source/life.c` | Doors, oxygen, venting, suffocation and medbay healing |
| `source/crew.c` | Crew races, names, spawning, pathfinding, movement and manning |
| `source/sound.c` | Sound effects (WAV/OGG) and streamed music through the DSP |
| `source/main.c` | Main loop, cameras, drawing and touch input |
