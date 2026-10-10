#pragma once
// What things cost and what they're called: system, weapon, drone, augment and crew blueprints
// from data/blueprints.xml (+ the DLC files), for upgrades, stores and event rewards.

#include <stdbool.h>

#include "ftldat.h"

enum { ITEM_WEAPON, ITEM_DRONE, ITEM_AUGMENT, ITEM_CREW, ITEM_SYSTEM, ITEM_KINDS };

typedef struct {
    int kind;            // ITEM_*
    const char* name;    // blueprint name (crew: race name)
    const char* title;   // display name
    const char* desc;    // description, or ""
    int cost, rarity;    // rarity 0: never offered at random
    float value;         // augments: the effect's strength
    bool stackable;      // augments
} ItemDef;

typedef struct {
    int startPower, maxPower;
    int cost;              // to buy in a store
    int upgrade[8];        // cost to go from level i+1 to i+2
    int upgradeCount;
    const char* title;
} SysBlueprint;

bool items_load(FtlDat* dat);
void items_free(void);

const ItemDef* item_find(const char* name);                  // weapon, drone, augment or crew
const SysBlueprint* item_system(int type);                   // SYS_*; NULL if unknown
int items_of_kind(int kind, const ItemDef** out, int max);
// Every item of a kind, store rarity or not (sandbox loadouts).
int items_all_of_kind(int kind, const ItemDef** out, int max);   // offered at random (rarity > 0)

// Scrap to buy the next reactor bar when the ship has `bars` (xftl ShipWindow: 30 below five,
// then 20/25/30/35 per block of five), up to 25.
int item_reactor_cost(int bars);
#define REACTOR_MAX 25
