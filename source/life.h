#pragma once
// Doors, oxygen, fires, hull breaches, suffocation and medbay healing.

#include "crew.h"
#include "ship.h"

// Breathing crew take damage in rooms with less oxygen than this (percent).
#define LIFE_SUFFOCATE_BELOW 5.0f

void life_update(Ship* ship, CrewSet* crew, float dt);

// Crew who died aboard a ship that isn't theirs (boarders), since the last call: the owner's clone
// bay brings them back, as in FTL. With out == NULL the list is just cleared.
typedef struct {
    char race[16];
    char name[24];
    bool female;
    short xp[SKILL_COUNT];
} DeadBoarder;
int life_take_dead_boarders(DeadBoarder* out, int max);

// Player door controls. Return true if anything changed.
bool life_toggle_door(Ship* ship, int door);
bool life_set_all_doors(Ship* ship, bool open);

// Average oxygen over all rooms, weighted by size (percent).
float life_ship_oxygen(const Ship* ship);

// Starts a fire / opens a hull breach on a random tile of the room that doesn't have one.
bool life_ignite(Ship* ship, int room);
bool life_breach(Ship* ship, int room);

int life_room_fires(const Ship* ship, int room);
int life_room_breaches(const Ship* ship, int room);
int life_ship_fires(const Ship* ship);
int life_ship_breaches(const Ship* ship);
