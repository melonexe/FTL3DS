#pragma once
// Ship blueprints, layouts (data/<layout>.txt + .xml) and their hull/floor images.

#include <stdbool.h>

#include "ftldat.h"
#include "system.h"
#include "texture.h"
#include "weapon.h"

#define TILE 35  // FTL room tiles are 35x35 pixels
#define MAX_ROOMS 64
#define MAX_DOORS 96
#define MAX_BLUEPRINTS 64
#define MAX_ENEMY_BLUEPRINTS 128  // auto, Lanius, Advanced Edition and pirate ships
#define MAX_CREW_KINDS 8
#define GRID_MAX 48  // room-grid tiles per side
#define MAX_WEAPON_SLOTS 4
#define MAX_ARTILLERY 4
#define MAX_MOUNTS 8
#define MAX_GIBS 8

typedef struct {
    char race[16];  // animSheet name: human, engi, mantis, energy, rock, slug, crystal, anaerobic
    int amount;
    int max;  // enemies: crew grows towards this with the sector
} CrewCount;

typedef struct {
    char name[48];    // e.g. PLAYER_SHIP_HARD
    char layout[48];  // data/<layout>.txt and .xml
    char img[48];     // img/ship/<img>_base.png and _floor.png
    CrewCount crew[MAX_CREW_KINDS];
    int crewKinds;
    int stationRooms[4];  // room ids of pilot, engines, weapons, shields; -1 if absent
    SystemDef systems[MAX_SYSTEMS];  // installed at the start (start="true") only
    int systemCount;
    SystemDef optional[MAX_SYSTEMS];  // start="false": rooms set aside for systems bought later
    int optionalCount;
    int weaponSlots, droneSlots;      // <weaponSlots>, <droneSlots>
    int reactor;  // <maxPower>
    bool enemy;   // from autoBlueprints.xml: hull art in img/ships_glow, no floor
    int health;   // <health amount>
    int missiles;
    char weapons[MAX_WEAPON_SLOTS][32];  // <weaponList><weapon name=...>
    int weaponCount;
    char weaponLoad[32];  // <weaponList load="..."> random list for enemies
    char classId[48];     // <class id="..."/>: text id of the ship's class name
    bool zoltanShield;    // <aug name="ENERGY_SHIELD"/>
    char augs[3][32];     // its starting augments (<aug name=...>)
    int augCount;
    char drones[MAX_WEAPON_SLOTS][32];  // <droneList><drone name=...>
    int droneCount;
    int droneParts;       // <droneList drones="N">
    char droneLoad[32];   // <droneList load="..."> random list for enemies
    int minSector;        // <minSector>: enemies, the first sector (1-based) they turn up in
} ShipBlueprint;

// Augments with effects in combat (counts in Ship.aug; effects from the FTL wiki, Augmentations).
enum {
    AUG_RELOADER,         // AUTO_COOLDOWN: weapons fire 10% faster each
    AUG_SHIELD_BOOST,     // SHIELD_RECHARGE: shields recharge 15% faster each
    AUG_FTL_BOOST,        // FTL_BOOSTER: the FTL drive charges 25% faster each
    AUG_ION_ARMOR,        // ION_ARMOR: 50% chance each to resist ion (two: immune)
    AUG_EXPLOSIVE,        // EXPLOSIVE_REPLICATOR: 50% chance a volley uses no missile
    AUG_PREIGNITE,        // WEAPON_PREIGNITE: powered weapons start fights charged
    AUG_O2_MASKS,         // O2_MASKS: half suffocation damage
    AUG_BATTERY,          // BATTERY_BOOSTER: battery cooldown 10 s instead of 20
    AUG_DRONE_RECOVERY,   // DRONE_RECOVERY: deployed drones come back as parts after a fight
    AUG_STEALTH_WEAPONS,  // CLOAK_FIRE: firing doesn't shorten the cloak
    AUG_SCRAMBLER,        // DEFENSE_SCRAMBLER: the enemy's defence drones don't shoot
    AUG_HACK_STUN,        // HACKING_STUN: a hacking pulse stuns everyone in the hacked room
    AUG_ZOLTAN_BYPASS,    // ZOLTAN_BYPASS: crew, bombs and mind control get through a Zoltan shield
    AUG_FIRE_SUPPRESS,    // FIRE_EXTINGUISHERS: every fire is fought at once, at a Crystal's speed
    AUG_BACKUP_DNA,       // BACKUP_DNA: the clone queue survives the clone bay being off or broken
    AUG_TELEPORT_HEAL,    // TELEPORT_HEAL: teleporting crew heals them fully
    AUG_FTL_JUMPER,       // FTL_JUMPER: jump to any beacon already visited
    AUG_DISTRACTION,      // FLEET_DISTRACTION: the fleet starts each sector a jump behind
    AUG_FTL_JAMMER,       // FTL_JAMMER: escaping enemies take twice as long to jump
    AUG_LIFE_SCANNER,     // LIFE_SCANNER: crew show in rooms the sensors can't see
    AUG_ADV_SCANNERS,     // ADV_SCANNERS: hazards and ships at neighbouring beacons on the map
    AUG_CRYSTAL_SHARDS,   // CRYSTAL_SHARDS: 10% of hull hits throw a shard back at the enemy
    AUG_NANO_MEDBAY,      // NANO_MEDBAY: crew heal 1.6 hp/s anywhere while the medbay is powered
    AUG_DRONE_SPEED,      // DRONE_SPEED: crew drones move 25% faster
    AUG_CREW_STIMS,       // CREW_STIMS: crew move 25% faster
    AUG_ROCK_ARMOR,       // ROCK_ARMOR: 15% chance to negate hull damage
    AUG_SLUG_GEL,         // SLUG_GEL: breaches seal themselves at 75% of a crew member's speed
    AUG_SYSTEM_CASING,    // SYSTEM_CASING: 15% chance to negate system damage
    AUG_ENERGY_SHIELD,    // ENERGY_SHIELD: a 5-point Zoltan shield after every jump
    AUG_COUNT
};
// The blueprint names of the augments above, in order.
extern const char* const AUG_NAMES[AUG_COUNT];

// Bits in Ship.open: which neighbouring tiles a crew member can walk to.
enum { OPEN_RIGHT = 1, OPEN_DOWN = 2, OPEN_LEFT = 4, OPEN_UP = 8 };

typedef struct {
    int id, x, y, w, h;  // tiles
} Room;

typedef struct {
    int x, y;            // tile the door sits on the edge of
    int roomA, roomB;    // -1 means outside the ship
    bool vertical;       // true: door in the tile's left wall, false: in its top wall
} Door;

typedef struct {
    float anim;    // 0 closed .. 1 fully open
    bool forced;   // opened by the player; stays open
    float hold;    // seconds a crew member keeps it open for
} DoorState;

typedef struct {
    const SystemDef* def;
    int room;              // room index
    int power;             // bars currently powered (always == level for subsystems)
    int stationX, stationY;  // station tile, or -1
    bool manned;
    signed char manCrew;   // who's manning it (index in the ship's own crew), or -1
    signed char manLevel;  // their skill level for this system
    Texture interior;
    Texture icon;          // s_<name>_overlay.png, drawn in the room
    Texture panelOn, panelOff;  // s_<name>_green1.png / _grey1.png for the power panel
    int iconBounds[4];          // x0, y0, x1, y1 of the symbol inside the panel icons
    int damage;                 // bars knocked out; usable level is def->level - damage
    float repair;               // progress (0..1) towards repairing the next bar
    float burn;                 // fire damage (0..1) towards losing the next bar
    float ionTimer;             // seconds of ion lock left
    float sabotage;             // boarder damage (0..1) towards breaking the next bar
    int zoltan;                 // free power from Zoltan crew standing in the room
    int ionLimit;               // most power allowed while ionised, -1 when not
    int eventCap;               // most power a beacon event allows (<status>), -1 when none
    int targetPower;            // power the player (or AI) asked for, restored after damage/ion
} ShipSystem;

typedef struct {
    int x, y;  // relative to the hull image
    bool rotate, mirror;
} WeaponMount;

// A piece the ship breaks into when destroyed (layout <explosion><gibN>).
typedef struct {
    Texture tex;
    int x, y;                      // offset from the hull image
    float vmin, vmax, dmin, dmax, amin, amax;  // speed, direction (degrees), spin (turns over the animation)
    float v, dir, spin;            // rolled when the ship dies
} Gib;

typedef struct {
    const DroneDef* def;
    bool powered;
    bool wanted;       // switched on; comes back on when power allows
    bool deployed;     // a drone is out (costs a drone part to deploy)
    float redeploy;    // seconds until a destroyed drone can be redeployed
    float boardTimer;  // boarding drones: seconds until it reaches the enemy hull
    Texture base, on, charged;
    Texture proj;      // its weapon's projectile animation
    int projFw, projFh, projX, projY, projFrames;
    float projTime;
} DroneSlot;

typedef struct {
    const WeaponDef* def;
    bool powered;
    bool wanted;      // switched on by the player/AI; turned back on when power allows
    bool beamAimed;   // beams: the player set a path (start and angle, enemy ship coordinates)
    int chain;        // chain weapons: volleys fired in a row (cuts the cooldown)
    float fullCharge; // seconds of charge needed to fire (cooldown x charge levels, less chain)
    float beamX, beamY, beamAngle;
    float charge;     // seconds charged, up to def->cooldown
    int target;       // room index on the other ship, or -1
    int burstLeft;    // shots still to fire from the current volley
    float burstTimer;
    float fireAnim;   // seconds since the last shot, for the firing animation
    Texture art;      // weaponAnim sheet
    int artFw, artFh, artX, artY, artFrames, chargedFrame, fireFrame, mountPx, mountPy;
    Texture proj;     // projectile animation sheet
    int projFw, projFh, projX, projY, projFrames;
    float projTime;
} WeaponSlot;

typedef struct {
    const ShipBlueprint* bp;

    // From the .txt layout
    int xOffset, yOffset;     // room-grid origin, in tiles
    int vertical, horizontal; // pixel nudges FTL applies when placing the ship
    int ellipse[4];           // shield ellipse: w, h, x, y
    Room rooms[MAX_ROOMS];
    int roomCount;
    Door doors[MAX_DOORS];
    int doorCount;

    // From the .xml layout: hull image position relative to the room-grid origin,
    // and floor image position relative to the hull.
    int hullX, hullY;
    int floorX, floorY;

    Texture hull, floor;

    // Walkable grid, in room-grid tiles (no X/Y_OFFSET applied).
    int gridW, gridH;
    signed char tileRoom[GRID_MAX][GRID_MAX];  // [y][x] room index, -1 for none
    unsigned char open[GRID_MAX][GRID_MAX];    // [y][x] OPEN_* bits

    ShipSystem systems[MAX_SYSTEMS];
    int systemCount;
    SystemDef sysDefs[MAX_SYSTEMS];  // this ship's copy of each system (systems[i].def), so it can be upgraded
    int reactor;
    int reactorCut;  // bars an ion storm takes away (half the reactor, rounded down)
    int weaponSlots, droneSlots;

    // Doors and life support.
    signed char doorRight[GRID_MAX][GRID_MAX];  // [y][x] door between (x,y) and (x+1,y), or -1
    signed char doorDown[GRID_MAX][GRID_MAX];   // [y][x] door between (x,y) and (x,y+1), or -1
    DoorState doorState[MAX_DOORS];
    float oxygen[MAX_ROOMS];  // percent
    float lockdown[MAX_ROOMS];  // seconds of crystal lockdown left: nobody walks in or out
    Texture doorSheet;

    // Fires and hull breaches, per room-grid tile ([y][x]).
    float fire[GRID_MAX][GRID_MAX];         // fire health, 0 = none, up to 100
    float fireSpread[GRID_MAX][GRID_MAX];   // countdown until an adjacent fire spreads here, 0 = idle
    float fireBurnout[GRID_MAX][GRID_MAX];  // countdown to burning out without air, 0 = idle
    float breach[GRID_MAX][GRID_MAX];       // breach health, 0 = none, up to 100

    // Combat.
    int hullHp, maxHull;
    int missiles;
    int shieldLayers;
    float shieldCharge;  // seconds towards the next layer
    WeaponMount mounts[MAX_MOUNTS];
    int mountCount;
    WeaponSlot weapons[MAX_WEAPON_SLOTS];
    int weaponCount;
    DroneSlot drones[MAX_WEAPON_SLOTS];
    int droneCount;
    int droneParts;
    Texture shieldImage;

    // Cloaking and backup battery.
    float cloakTime;     // seconds of cloak left, 0 when not cloaked
    float batteryTime;   // seconds of battery left
    int batteryBonus;    // extra reactor bars while the battery runs
    Texture cloakImage;  // <img>_cloak.png, drawn instead of the hull while cloaked (loaded on first cloak)
    Gib gibs[MAX_GIBS];  // textures loaded only when the ship may be about to break up
    int gibCount;
    FtlDat* dat;         // for art loaded later
    bool gibsLoaded, cloakTried;
    float deathTime;     // seconds since the ship broke apart, < 0 while intact
    int superShieldMax;  // full Zoltan shield: 5, or the flagship's 12
    int superShield;     // Zoltan shield points left (absorbs anything; refilled each fight)
    // Hacking. On the target: which system the other side's drone sits on, and its pulse.
    int hackedSys;       // -1 when no hacking drone is attached
    float hackPulse;     // seconds of hacking pulse left
    float hackDrain;     // shields: time towards losing the next layer
    // On the hacker: its drone in flight.
    float hackFlight;
    int hackAim;
    // Artillery (Federation cruiser): charges by itself and fires its beam when full.
    // Artillery: one weapon per artillery system (the flagship has four), charging by itself.
    WeaponSlot artillery[MAX_ARTILLERY];
    int artSys[MAX_ARTILLERY];  // the artillery system each belongs to
    int artilleryCount;
    int cloakX, cloakY;  // its offset from the hull image
    int aug[AUG_COUNT];  // fitted augments (the run keeps the list; main copies it here)
} Ship;

// Collects player ships from data/blueprints.xml and data/dlcBlueprints.xml.
int ship_list_blueprints(FtlDat* dat, ShipBlueprint* out, int max);

// FTL's enemy generation (xftl ShipGenerator, normal difficulty): crew, optional systems and
// system power grow with the sector (0-based). Writes a copy of `base` to `out`.
// A beacon event's <status> on one of a ship's systems (xftl EventStatus): LIMIT caps its power at
// `amount`, DIVIDE at its level / amount, LOSS at its level - amount; CLEAR lifts it. It lasts until
// the next jump (ship_clear_event_status).
enum { EVSTAT_LIMIT, EVSTAT_DIVIDE, EVSTAT_LOSS, EVSTAT_CLEAR };
void ship_event_status(Ship* s, int sysType, int op, int amount);
void ship_clear_event_status(Ship* s);

void ship_generate_enemy(ShipBlueprint* out, const ShipBlueprint* base, int sector, int difficulty);

// Collects regular enemy ships from data/autoBlueprints.xml (no bosses or special ships).
int ship_list_enemies(FtlDat* dat, ShipBlueprint* out, int max);

// Bars of a system locked by ion damage.
int ship_system_ion(const Ship* ship, int sys);

// A system's usable level after damage and ion.
int ship_system_capacity(const Ship* ship, int sys);

bool ship_load(Ship* ship, FtlDat* dat, const ShipBlueprint* bp, char* err, size_t errlen);
// The rebel flagship's blueprint for a stage (data/bosses.xml, e.g. BOSS_1_NORMAL_DLC).
bool ship_find_boss(FtlDat* dat, const char* name, ShipBlueprint* out);
// Art that's rarely on screen, loaded when it's about to be (linear memory is tight on a 3DS):
// the pieces a destroyed ship breaks into, and the cloaking glow.
void ship_load_gibs(Ship* ship);
void ship_load_cloak(Ship* ship);

// Installs a system (bought, or at load). Returns its index, or -1.
int ship_install_system(Ship* ship, FtlDat* dat, const SystemDef* def);
// Just the layout (rooms, doors, hull and floor offsets), no art: quick, for previews.
bool ship_load_layout(Ship* ship, FtlDat* dat, const ShipBlueprint* bp);
// Puts a different system in place of system `idx` (the medbay / clone bay swap), keeping its level.
// Frees the old one's textures: wait for the GPU first.
int ship_replace_system(Ship* ship, FtlDat* dat, int idx, const SystemDef* def);
void ship_free(Ship* ship);

// Room index at a tile, or -1.
int ship_room_at(const Ship* ship, int tx, int ty);

// System in a room, or -1.
int ship_system_in_room(const Ship* ship, int room);

// Door crossed when stepping between two adjacent tiles, or -1.
int ship_door_between(const Ship* ship, int ax, int ay, int bx, int by);

// True if the door leads to space (an airlock).
bool ship_door_is_airlock(const Ship* ship, int door);

// First system of a type, or -1.
int ship_find_system(const Ship* ship, int type);

// Seconds artillery slot k needs to charge at its system's power, 0 if unpowered: its weapon's
// cooldown x 1.25 / 1 / 0.75 / 0.5 at power 1-4 (wiki: Federation artillery 50/40/30/20, flagship
// weapons, e.g. Boss Laser 25/20/15/10).
float ship_artillery_time(const Ship* ship, int k);
// The artillery slot of system `sys`, or -1.
int ship_artillery_index(const Ship* ship, int sys);

// A system's working power: reactor power plus Zoltan crew, up to what's intact.
int ship_system_power(const Ship* ship, int sys);

// Reactor bars not allocated to any system.
int ship_reactor_free(const Ship* ship);

// Sets a reactor-powered system's power, limited by its level and the free reactor bars.
void ship_set_power(Ship* ship, int sys, int power);

// Bounding box of everything the ship draws, in ship pixels (room-grid origin at 0,0).
void ship_bounds(const Ship* ship, float* x0, float* y0, float* x1, float* y1);
