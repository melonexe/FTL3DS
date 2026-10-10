#pragma once
// Saving and continuing a run: sdmc:/3ds/ftl3ds/save.dat holds the run (sector map, beacons,
// resources, store, equipment) and the player's ship (systems, weapons, drones, crew). The game
// saves at peaceful moments (no fight, no event open) and deletes the save when the run ends, as
// FTL does.

#include <stdbool.h>

#include "crew.h"
#include "run.h"
#include "ship.h"

#define SAVE_CREW 12

typedef struct {
    char bp[48];  // the ship's blueprint
    int hullHp, maxHull, missiles, droneParts, reactor;
    int systemCount;
    struct {
        int type, level, power, damage;
    } sys[MAX_SYSTEMS];
    int weaponCount;
    char weapons[MAX_WEAPON_SLOTS][32];
    bool weaponOn[MAX_WEAPON_SLOTS];
    int droneCount;
    char drones[MAX_WEAPON_SLOTS][32];
    int crewCount;
    struct {
        char race[16], look[16], name[24];
        bool female;
        int x, y, homeX, homeY;
        float health;
        short xp[SKILL_COUNT];
    } crew[SAVE_CREW];
} SaveShip;

// Fills a SaveShip from the ship and its own crew (not boarders or drones).
void save_ship_capture(SaveShip* out, const Ship* s, const CrewSet* cs);
// Writes the run and ship. False if the file couldn't be written.
bool save_store(const Run* run, const SaveShip* ship);
// Reads them back (the run's event pointers restored). False if there's no usable save.
bool save_load(Run* run, SaveShip* ship);
bool save_exists(void);
void save_delete(void);
