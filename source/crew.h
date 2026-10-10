#pragma once
// Crew members: race sprites, names, and walking between rooms.

#include <stdbool.h>

#include "anim.h"
#include "ship.h"

#define MAX_CREW 20   // everyone on one ship: its crew, boarders and crew drones
#define CREW_LIMIT 8  // FTL's limit on a ship's own crew
#define MAX_RACES 16  // crew races, boarders' races and crew drone types on one ship
#define MAX_PATH (GRID_MAX * 4)

enum { DIR_DOWN, DIR_RIGHT, DIR_UP, DIR_LEFT };

// Crew skills (wiki: Crew skills). Experience comes from doing the job; level 1 and 2 each take
// the skill's threshold again.
enum { SKILL_PILOT, SKILL_ENGINES, SKILL_SHIELDS, SKILL_WEAPONS, SKILL_REPAIR, SKILL_COMBAT, SKILL_COUNT };

typedef struct {
    char name[16];  // race (animation) name, e.g. human
    char look[16];  // sprite variant, e.g. female uses human animations with female_base.png
    Texture tex;
    int fw, fh, rows;
    AnimInfo walk[4];  // indexed by DIR_*
    AnimInfo type[4];  // working at a console
    AnimInfo repair;
    AnimInfo fight[4];  // firefighting
    AnimInfo punch[2];  // hand-to-hand: [0] down, [1] up
    float speed;       // ship pixels per second
    int portrait[4];   // x0, y0, x1, y1 of the figure inside its first walk-down frame
    float maxHealth;
    bool breathes;     // false for Lanius and drones
    bool drainsAir;    // Lanius
    bool fireImmune;   // Rock crew and drones
} Race;

typedef struct {
    char name[24];
    int race;
    float x, y;  // centre, in ship pixels relative to the room-grid origin
    int path[MAX_PATH][2];
    int pathLen, pathPos;  // tiles still to walk: path[pathPos .. pathLen-1]
    int destX, destY;      // tile the crew member is heading to or standing on
    int homeX, homeY;      // saved station (FTL's "save stations"); homeX < 0 when none
    int dir;
    float animTime;
    bool manning;  // standing at a system's station
    bool female;
    float health;
    bool repairing;  // set by combat while fixing a damaged system or sealing a breach
    bool fighting;   // set by combat while putting out a fire
    bool foreign;    // a boarder: belongs to the other ship's side
    bool brawling;   // punching an enemy crew member or sabotaging a system
    float stun;      // seconds left stunned: can't move or act
    bool drone;      // a crew-like drone (repair, battle, boarding), run by its own AI
    int droneSlot;   // its owner's drone slot
    bool dormant;    // drone switched off: it stops working
    float mind;      // seconds left under mind control (fighting for the other side)
    int mindBoost;   // mind control level: +25% / +100% damage at 2 / 3
    float brawlTimer;
    short xp[SKILL_COUNT];  // skill experience
    float ability;          // Crystal lockdown: seconds until it can be used again
    signed char lastHit;    // HIT_* (feats.h): who last hurt them, for achievements
    bool lastHitBio;        // ... with an Anti-Bio Beam (shot lastHitSerial)
    int lastHitSerial;
    short ionLast;          // ion intruder: the room it last pulsed (+1; 0 none)
    float levelUp;          // seconds left showing a level-up mark
    signed char levelUpSkill;
} Crew;

typedef struct {
    Race races[MAX_RACES];
    int raceCount;
    Crew crew[MAX_CREW];
    int count;
    unsigned selected;  // bit i set when crew[i] is selected
    // Clone bay queue: crew who died waiting to be cloned, oldest first.
    struct {
        int race;
        char name[24];
        bool female;
        short xp[SKILL_COUNT];
    } cloneQueue[MAX_CREW];
    int cloneCount;
    float cloneProgress, cloneDying;
    Texture destMarker;
    FtlDat* dat;  // for loading boarders' sprites
    const AnimDb* db;
    float boost[2][2];  // walking speed multiplier by [foreign][drone] (augments; 0 means 1)
} CrewSet;

typedef struct {
    char (*names)[24];
    bool* female;
    int count;
} NameList;

// Loads names from data/names.xml (English lists only).
void names_load(NameList* list, FtlDat* dat);
void names_free(NameList* list);

// Creates the ship's starting crew and places them at the pilot, engines, weapons and
// shields stations, then any free tile.
void crew_spawn(CrewSet* cs, const Ship* ship, FtlDat* dat, const AnimDb* db, const NameList* names);
void crew_free(CrewSet* cs);
// Frees the sprite sheets of races no crew member (or queued clone) uses any more, e.g. boarders
// long gone. Wait for the GPU first.
void crew_prune_races(CrewSet* cs);

// Sends crew member `ci` to the closest free tile in `room`. Returns false if the room is full
// or unreachable.
bool crew_order(CrewSet* cs, const Ship* ship, int ci, int room);
// Moves crew along their paths and updates who is manning which system.
void crew_update(CrewSet* cs, Ship* ship, float dt);

static inline bool crew_is_selected(const CrewSet* cs, int i) { return (cs->selected >> i) & 1u; }

// Sends every selected crew member to the room, each to its own free tile.
void crew_order_selected(CrewSet* cs, const Ship* ship, int room);

// FTL's crew stations: remember where everyone of ours stands, and send them back there (the
// nearest free tile of that room if theirs is taken).
void crew_save_stations(CrewSet* cs);
void crew_return_to_stations(CrewSet* cs, const Ship* ship);

// Moves crew member `ci` of `from` to tile (tx, ty) of the ship `to` belongs to (teleporting).
// The crew member becomes a boarder there, or stops being one when going home.
bool crew_transfer(CrewSet* from, int ci, CrewSet* to, int tx, int ty);

// Index of a race in the set, loading its sprites if needed (e.g. "battle" for drones), or -1.
int crew_race(CrewSet* cs, const char* race);
// The same with a sprite variant (e.g. "female" for a female human).
int crew_race_look(CrewSet* cs, const char* race, const char* look);

// Crew in a set that are native (not boarders) / boarders. Drones don't count.
int crew_native_count(const CrewSet* cs);
int crew_foreign_count(const CrewSet* cs);

// Skill level 0-2 of a crew member (Humans learn a little faster).
int crew_skill_level(const CrewSet* cs, const Crew* m, int skill);
// Experience for the next level (the threshold), and the skill shown for each mannable system
// type, or -1.
int crew_skill_threshold(const CrewSet* cs, const Crew* m, int skill);
int crew_system_skill(int systemType);
// One point of experience. Returns true when it brings a new level.
bool crew_add_xp(CrewSet* cs, Crew* m, int skill);
// Sets a crew member's skills to the given levels (from events: "all_skills", "pilot", ...).
void crew_set_skill_level(CrewSet* cs, Crew* m, int skill, int level);
// What cloning costs: 20% of each skill's experience, but only one point of Combat.
void crew_clone_xp(short xp[SKILL_COUNT]);

// Adds a crew member of an already-loaded race on a tile, at full health. Returns false if full.
bool crew_add(CrewSet* cs, int race, const char* name, bool female, int tx, int ty);

// Removes a crew member (death), keeping the selection pointing at the same people.
void crew_remove(CrewSet* cs, int ci);

// Index of the crew member nearest (px, py) within `radius` ship pixels, or -1.
int crew_pick(const CrewSet* cs, float px, float py, float radius);

// Draws a crew member's current frame centred at screen (x, y).
void crew_draw(const CrewSet* cs, int ci, float x, float y, float scale);
