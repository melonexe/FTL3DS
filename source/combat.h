#pragma once
// Ship-to-ship combat: weapons, shields, evasion, projectiles, damage, repairs and the enemy AI.

#include <stdbool.h>

#include "anim.h"
#include "crew.h"
#include "ship.h"

#define MAX_SHOTS 32
#define MAX_EFFECTS 24

enum { SIDE_PLAYER, SIDE_ENEMY };
enum { COMBAT_FIGHTING, COMBAT_WON, COMBAT_LOST };

typedef struct {
    bool active;
    int from;          // SIDE_* of the ship that fired
    int weapon;        // slot on the firing ship
    int room;          // target room on the other ship
    float sx, sy;      // start, in the firing ship's coordinates (X/Y_OFFSET included)
    float tx, ty;      // end, in the target ship's coordinates
    float t, duration;
    float speed;       // game units, for drawing at the real speed
    bool miss;
    bool outside;      // flak that lands outside every room
    int drone;         // firing drone's slot, or -1 for a ship weapon (then `weapon` is used)
    bool inTarget;     // fired from inside the target's space (drones): sx/sy are target coordinates
    bool shotDown;     // hit by a defence drone
} Projectile;

// A beam sweeping across the other ship.
typedef struct {
    bool active;
    int from, weapon, drone;  // drone slot, or -1 when fired by `weapon`
    float sx, sy, ex, ey;  // start and end, in the target ship's coordinates (X/Y_OFFSET included)
    float delay, t, duration;
    unsigned long long hitRooms;    // rooms already damaged (bit per room)
    int superTicks;                 // Zoltan shield hits done
    unsigned char hitTiles[GRID_MAX * GRID_MAX / 8];
    int serial;                     // numbered shot (feats.h)
} Beam;

#define MAX_BEAMS 8

// A deployed drone flying around a ship.
typedef struct {
    bool active;
    int side;          // owner (SIDE_*)
    int slot;          // owner's drone slot
    int space;         // ship whose coordinates x/y are in
    float x, y, dx, dy;  // position and destination
    float angle;       // around the shield ellipse
    float pause;       // stopped at a destination
    bool fired;        // has fired at this stop (beam drones then wait out their beam)
    float cooldown;    // defence shots / shield recharge
    float stun, stunTime;
    int repairsLeft;   // hull repair drones
    float shotFlash, shotX, shotY;  // defence drone shot, for drawing
    int tempShots;     // a flagship power-surge drone: shots left before it goes (0: a real drone)
} DroneEntity;

#define MAX_DRONE_ENTITIES 16

enum { FX_EXPLOSION, FX_SHIELD, FX_MISS };

typedef struct {
    bool active;
    int side;  // ship the effect is on
    int kind;  // FX_*
    float x, y, t;
    const char* anim;  // FX_EXPLOSION: animations.xml name (the weapon's <explosion>, or a random big one)
} Effect;

typedef struct {
    Ship* ship[2];  // SIDE_PLAYER, SIDE_ENEMY
    CrewSet* crew[2];
    Projectile shots[MAX_SHOTS];
    Effect effects[MAX_EFFECTS];
    Beam beams[MAX_BEAMS];
    DroneEntity droneEnts[MAX_DRONE_ENTITIES];
    int result;  // COMBAT_*
    float resultTime;
    float aiTimer;
    float shieldFlash[2];
    bool autofire;  // player weapons keep their target after firing
    bool enemyHoldFire;  // testing: the enemy keeps its weapons off
    int enemyBoardings;  // times the enemy has sent boarders this fight
    float boardTimer;
    bool crewKilled;     // won by killing the enemy crew rather than destroying the ship
    bool enemyHadCrew;   // the enemy started the fight with crew aboard
    // The beacon's environment (run.h ENV_*), its PDS targets, and its timers.
    int env, envTargets;
    int envPhase;          // asteroids: 0 idle, 1 slow wave, 2 fast wave
    float envPhaseTime;    // asteroids: seconds left in the phase
    float envNext;         // seconds to the next asteroid / flare / pulse / battery shot
    bool envAtEnemy;       // asteroids alternate between the ships
    int envShields;        // asteroids: our shield bubbles when we arrived
    bool envWarned, envSounded;
    float envFlash;        // flare / pulse flash, 1 -> 0
    int envFlashKind;      // ENV_SUN or ENV_PULSAR
    int difficulty;        // DIFF_* (kept by combat_start)
    // The rebel flagship (0: not fighting it): its stage, and its power surges in stages 2-3.
    int bossStage;
    float surgeNext;       // seconds to the next surge
    int surgeCount;        // surges so far (stage 3: every fourth restores the Zoltan shield)
    int surgeBeams;        // stage 2: how many of the surge drones are beam drones
    bool surgeWarned;
} Combat;

// Hazard shots have no firing ship: these `weapon` values stand for them.
#define SHOT_ASTEROID (-2)
#define SHOT_PDS (-3)
#define SHOT_SURGE (-4)  // the flagship's power-surge lasers
#define SHOT_SHARD (-5)  // Crystal Vengeance's shard (from the ship that took the hit)
// Artillery shots and beams: slot k of the firing ship's artillery.
#define SHOT_ART(k) (-10 - (k))
#define SHOT_IS_ART(w) ((w) <= -10)
#define SHOT_ART_SLOT(w) (-10 - (w))

// Loads the ship's weapons: its blueprint's list, or for enemies a random pick from its weapon
// list that the weapons system can power. Enemy weapons start powered.
void combat_arm(Ship* ship, FtlDat* dat, const AnimDb* db);

// Equipment changes (event rewards, stores, the ship screen). Removing frees textures, so the
// caller must make sure the GPU is done with them first.
bool combat_add_weapon(Ship* ship, FtlDat* dat, const AnimDb* db, const char* name);
// Crystal Vengeance's shard: load its art (outside a frame), then draw shards with it.
void combat_load_shard(FtlDat* dat, const AnimDb* db);
// Queues decoding of a ship's weapon and drone sounds (after a jump has released them).
void combat_prefetch_ship_sounds(const Ship* s);
// What a jump resets: the Zoltan shield refills, lockdowns end and recharge, weapons lose their
// charge (Weapon Pre-Igniter: powered ones start full). Also done when a fight starts.
void combat_jump_reset(Ship* player, CrewSet* playerCrew);
// Crystal lockdown: coats a room of ship `side` for 12 s (lockdown bombs, the Crystal ability).
void combat_lockdown(Combat* c, int side, int room);
// Crystal crew member `ci` of `side`'s crew set uses the ability where they stand.
bool combat_crystal_lockdown(Combat* c, int side, int ci);
const WeaponSlot* combat_shard_slot(void);
bool combat_add_drone(Ship* ship, FtlDat* dat, const AnimDb* db, const char* name);
void combat_remove_weapon(Ship* ship, int i);
void combat_remove_drone(Ship* ship, int i);

void combat_start(Combat* c, Ship* player, CrewSet* playerCrew, Ship* enemy, CrewSet* enemyCrew);
void combat_update(Combat* c, float dt);
// The beacon's environment (ENV_* from run.h, PDS target bits). Timers restart when it changes.
void combat_set_environment(Combat* c, int env, int targets);
// The next hazard event, for the HUD: seconds until it while its warning is up, else -1.
float combat_env_warning(const Combat* c);
// Fighting the rebel flagship at stage 1-3 (after combat_start); 0 for an ordinary fight.
void combat_set_boss(Combat* c, int stage);
// Seconds to the flagship's next power surge while its warning is up, else -1.
float combat_surge_warning(const Combat* c);

// Weapon power. Toggling fails (returns false) when there's no power or the weapon is unsupported.
bool combat_toggle_weapon(Ship* ship, int weapon);
// Powers weapons in slot order to use at most `power` bars (the power tab's weapons card).
void combat_set_weapons_power(Ship* ship, int power);

void combat_set_target(Combat* c, int weapon, int room);

// Beams: aim along a path starting at (x, y) in the enemy's coordinates (X/Y_OFFSET included).
void combat_set_beam(Combat* c, int weapon, float x, float y, float angle);

// Drones: toggling deploys (one drone part) when needed. The power tab's drones card uses
// combat_set_drones_power.
bool combat_toggle_drone(Combat* c, int side, int drone);
void combat_set_drones_power(Combat* c, int side, int power);

// Cloaking (5 s per power, +60% evasion, then 20 s recharge) and the backup battery (2 extra
// reactor bars per level for 30 s, then 20 s recharge). Return false if they can't start now.
bool combat_cloak(Combat* c, int side);
bool combat_battery(Combat* c, int side);

// Teleporter: 0 ready, 1 recharging, 2 unpowered, -1 none. Send moves crew standing in the
// teleporter room to a room of the other ship; return brings boarders home. Both return the
// number of crew moved.
int combat_teleporter_state(const Combat* c, int side);
int combat_teleport_send(Combat* c, int side, int room);
int combat_teleport_return(Combat* c, int side);

// Hacking and mind control.
enum { HACK_NONE, HACK_NO_DRONE, HACK_FLYING, HACK_READY, HACK_PULSING, HACK_RECHARGING, HACK_NO_POWER };
enum { MIND_NONE, MIND_READY, MIND_RECHARGING, MIND_NO_POWER };
int combat_hack_state(const Combat* c, int side);
bool combat_hack_launch(Combat* c, int side, int room);  // sends the drone at the system in `room`
bool combat_hack_pulse(Combat* c, int side);
int combat_mind_state(const Combat* c, int side);
bool combat_mind_control(Combat* c, int side, int room, bool onOwnShip);

// Debug: moves up to n of a side's crew aboard the other ship's system rooms, no teleporter needed.
int combat_debug_board(Combat* c, int side, int n);

// Shield ellipse centre and radii in the ship's coordinates (X/Y_OFFSET included).
void combat_shield_ellipse(const Ship* ship, float* cx, float* cy, float* rw, float* rh);

// The weapon a projectile or beam came from (a ship weapon or a drone's).
const WeaponDef* combat_shot_def(const Combat* c, int from, int weapon, int drone);

// Chance (percent) that a shot at this ship misses.
int combat_evasion(const Ship* ship);

// Whether a side's FTL drive can charge (xftl Ship.canChargeFTL): engines powered, piloting not
// broken, someone able in the piloting room (crewless auto-ships fly themselves), and neither
// system under a hacking pulse.
bool combat_can_charge_ftl(const Combat* c, int side);
int combat_max_shields(const Ship* ship);
// How far the next shield layer has charged, 0..1, or -1 when the shields are full.
float combat_shield_progress(const Ship* ship);

// Ship-space point (X/Y_OFFSET included) of a weapon's mount.
void combat_mount_point(const Ship* ship, int weapon, float* x, float* y);
