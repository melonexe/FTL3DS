#pragma once
// A run: sectors of beacons, jumping with fuel, the rebel fleet, and FTL's event scripts
// (data/events*.xml) with their choices, rewards, ships and stores.

#include "env.h"
#include <stdbool.h>

#include "crew.h"
#include "ftldat.h"
#include "ship.h"
#include "xmldom.h"

#define MAX_BEACONS 24
#define MAX_AUGS 3
#define CARGO_SLOTS 4
#define STORE_SECTIONS 4
#define STORE_ITEMS 3
#define MAX_CHOICES 8
#define RUN_SECTORS 8
#define SECTOR_ROWS 4

// One sector on the sector map (FTL's StarMap: 8 columns, the middle ones 2-4 sectors each).
enum { SECTOR_CIVILIAN, SECTOR_HOSTILE, SECTOR_NEBULA, SECTOR_FINAL };

typedef struct {
    char type[40];  // sectorDescription name, e.g. ENGI_SECTOR
    char name[48];  // its display name
    int cls;        // SECTOR_*
    int next[SECTOR_ROWS], nextCount;  // rows it links to in the next column
} SectorNode;

typedef struct {
    float x, y;  // on the 660 x 440 sector map
    int nb[8], nbCount;
    const XNode* event;  // unresolved: the event, eventList or loader assigned to this beacon
    char eventName[32];  // the name it was assigned under (e.g. STORE), for map labels
    bool visited, exit, start;
    bool quest;  // a quest event was placed here (shown as QUEST on the map)
    int env, envTarget;  // its environment, once its event has set one
    bool base;           // the Last Stand: the Federation base
    bool fleetSoon;      // the Last Stand: the rebels take it next turn (flashing on the map)
    bool fleetHeld;      // the Last Stand: rebel-held
} Beacon;

// What the player sees for the current event, and what the game should do once it's read.
typedef struct {
    bool active;
    char text[1400];
    int choiceCount;
    char choiceText[MAX_CHOICES][200];
    bool choiceBlue[MAX_CHOICES];
    bool choiceCrew[MAX_CHOICES];  // a blue option for having a crew race (Diplomatic Immunity)
    bool choiceDisabled[MAX_CHOICES];  // costs more than we have (shown greyed, can't be picked)
    // A choice's resource changes, rolled when the choices are shown so the preview is what you get.
    bool choiceHasItems[MAX_CHOICES];
    int choiceItems[MAX_CHOICES][4];  // scrap, fuel, missiles, drone parts
    const XNode* choiceEvent[MAX_CHOICES];
    // Requests for the main loop:
    const XNode* ship;    // an enemy ship node (hostile) to fight, or NULL
    const XNode* shipDef; // its <ship name=...> definition (destroyed/deadCrew/surrender events)
    bool store;           // open a store
    bool peace;           // <ship hostile="false">: the fight ends (an accepted surrender)
} EventView;

// Everything an event can touch.
typedef struct {
    Ship* ship;
    CrewSet* crew;
    const NameList* names;
    FtlDat* dat;
    const AnimDb* db;
    void (*waitGpu)(void);  // call before freeing textures (removing a weapon or drone)
} RunCtx;

typedef struct {
    int sector;  // 0-based: the column on the sector map
    int sectorRow;  // which sector of that column
    SectorNode sectors[RUN_SECTORS][SECTOR_ROWS];
    int sectorCount[RUN_SECTORS];
    char sectorType[40];
    char sectorName[64];
    char track[24];  // music now playing (sounds.xml track name)
    char tracks[8][24];  // the sector's trackList, played in order
    int trackCount, trackIndex;
    // The beacon's backdrop (ftl.dat paths, "" for none) from events_imageList.xml.
    char bg[64], planet[64];
    int planetSize;          // the size FTL draws the planet at, in 1280x720 screen pixels
    float planetX, planetY;  // where, as fractions of the screen
    Beacon beacons[MAX_BEACONS];
    int beaconCount, current;
    int scrap, fuel;
    float fleetX, fleetY;  // rebel fleet circle centre (radius 767)
    int env, envTarget;    // the current beacon's environment (ENV_*) and PDS targets
    int difficulty;        // DIFF_* (kept by run_start: set it before starting)
    bool revealed;
    bool over, won;
    // The Last Stand (final sector; wiki: The Rebel Flagship, xftl FlagshipBoss): the flagship
    // heads for the Federation base, jumping every second turn; three turns at the base and it's
    // lost. Beat its three stages to win.
    bool lastStand;
    int baseBeacon;
    int flagBeacon, flagNext;  // where it is; where it's jumping to next (-1: staying put)
    bool flagJumping;          // it jumps this turn
    bool flagRunning;          // forced off the base after losing a stage
    int flagStage;             // 1-3; 4 once destroyed
    int baseTurns;             // turns it has spent at the base
    bool bossFight;            // we're at the flagship: the game starts the stage's fight
    int flagCrew;              // its crew outside the weapon rooms (11 humans in all at first)
    bool flagArtCrew[4];       // a crewman left in each weapon room (ARTILLERY_BOSS_1-4)
    bool baseLost;
    EventView view;
    // Store stock.
    int storeFuel, storeMissiles, storeParts;
    int storeKind[STORE_SECTIONS], storeSectionCount;   // ITEM_* per section
    char storeItem[STORE_SECTIONS][STORE_ITEMS][32];   // blueprint names (systems: system name), "" once bought
    unsigned char storeSkill[STORE_SECTIONS][STORE_ITEMS][2];  // crew for hire: skill * 4 + level (0: untrained)
    // Equipment that isn't on a ship slot.
    char augs[MAX_AUGS][32];
    int augCount;
    char cargo[CARGO_SLOTS][32];  // weapon or drone blueprints in the hold, "" for empty
    // Quests that didn't fit in their sector, placed in the next one.
    const XNode* delayedQuests[4];
    int delayedCount;
    // What this run has (and hasn't) done, for achievements (main.c check_achievements).
    struct {
        bool upgradedSystem, upgradedReactor, upgradedWeapons;  // bought in the upgrade menu
        bool storeRepair, storeBuy;                             // at a store
        bool firedShot, usedMissiles, usedDrones, offensive, teleported, crewDied;
        bool dangerJump;   // arrived at a beacon with an environmental danger
        bool oxygenOver;   // the ship's oxygen went over 20% after the first jump
        bool hullAtOne;    // the hull has been down to 1
        int crewKillWins;  // fights won by killing the crew
        int nebulaJumps, blueCrewUses, rockShipsDestroyed;
        int lastSector;    // the sector the checks last saw (achievements on arrival)
    } feats;
} Run;

bool run_load(FtlDat* dat);
void run_free(void);

void run_start(Run* run, RunCtx* ctx);         // sector 1, start beacon event
// Moves on to sector `row` of the next column (one the current sector links to).
void run_next_sector(Run* run, RunCtx* ctx, int row);
// Whether the current sector links to that row of the next column.
bool run_sector_reachable(const Run* run, int row);
bool run_can_jump(const Run* run, int beacon);  // a neighbour, with fuel
void run_jump(Run* run, RunCtx* ctx, int beacon);
bool run_beacon_overtaken(const Run* run, int beacon);

// Out of fuel: wait a turn. The fleet advances one jump and an out-of-fuel event happens
// (NO_FUEL, or NO_FUEL_FLEET once the fleet has caught up).
void run_wait(Run* run, RunCtx* ctx);
// Saving: the run's pointers into the event files become their place in a walk through the files
// (the same ftl.dat loads the same way), and back. The open event dialog isn't kept.
void run_pack(Run* run);
void run_unpack(Run* run);

// A flagship stage was destroyed: it runs (or, after stage 3, the run is won).
void run_flagship_defeated(Run* run, RunCtx* ctx);

// Shows a named event now (testing).
void run_show_named(Run* run, RunCtx* ctx, const char* name);

// The current event's choice was picked.
void run_choose(Run* run, RunCtx* ctx, int choice);
// The fight ended; destroyed = the ship blew up (otherwise its crew died).
void run_fight_won(Run* run, RunCtx* ctx, bool destroyed);

// Picks a background and planet from imageLists (NULL: the defaults, "NONE": no planet).
void run_backdrop(Run* run, const char* back, const char* planet);

// Resource prices (scrap): fuel, missiles, drone parts, one hull point.
int run_price(const Run* run, int item);
enum { STORE_FUEL, STORE_MISSILES, STORE_PARTS, STORE_HULL };
bool run_buy(Run* run, RunCtx* ctx, int item);

// Store items: price, and buying (installs or stores it). Returns false if it can't.
int run_store_price(const Run* run, int section, int slot);
bool run_store_buy(Run* run, RunCtx* ctx, int section, int slot);
// Adds a weapon/drone/augment/crew by name ("RANDOM" or a blueprint list picks one). Fills `got`
// with its display name. Returns false if there was no room.
bool run_give(Run* run, RunCtx* ctx, int kind, const char* name, char* got, size_t gotSize);
// Adds a crew member ("random" or a race; name NULL for a random one) on a free tile.
void run_add_crew(RunCtx* ctx, const char* race, const char* name);
// Whether an augment is fitted (for its effect).
bool run_has_aug(const Run* run, const char* name);
// Long-Ranged Scanners: what an unvisited beacon holds (its hazard, ENV_*, and a hostile ship).
bool run_beacon_scan(const Run* run, int beacon, int* env, bool* ship);
// A beacon's environment (ENV_*): the one it had, or the one its event will bring. Nebulas show
// on FTL's map before you get there.
int run_beacon_env(const Run* run, int beacon);

// The current enemy's surrender and escape hull thresholds (0: never) and escape charge time,
// rolled as xftl RandomWithChance: `chance` is the chance of 0, otherwise min..max.
void run_ship_thresholds(const Run* run, int* surrender, int* escape, float* escapeTimer);
// Shows the current enemy's <surrender>, <escape> or <gotaway> event. False if it has none.
bool run_ship_event(Run* run, RunCtx* ctx, const char* which);

// The name of the enemy blueprint a ship node wants (picked from its list), or NULL.
const char* run_ship_blueprint(const XNode* ship, const XNode* def);
// The ship the beacon's event is about, if any: one already hostile (ship set) or one that may turn
// hostile through a choice (only def set). For loading it ahead of the fight.
bool run_arrival_ship(const Run* run, const XNode** ship, const XNode** def);
