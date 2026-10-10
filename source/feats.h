#pragma once
// What happens in play that achievements care about. Combat and life code record it here; main.c
// turns it into achievements (see check_achievements).

#include <stdbool.h>

// What took a point off the enemy's hull.
enum { SRC_NONE, SRC_WEAPON, SRC_MISSILE, SRC_DRONE, SRC_ARTILLERY, SRC_SHARD, SRC_HAZARD, SRC_COUNT };
// Who last hurt a crew member (Crew.lastHit): our crew in a fist fight, our weapons, or our
// boarding drone in slot n (HIT_BOARDER + n).
enum { HIT_NONE, HIT_CREW, HIT_WEAPON, HIT_BOARDER };

typedef struct {
    // This fight (cleared when one starts).
    struct {
        float clock;                // seconds since it started
        int enemyHull[SRC_COUNT];   // enemy hull lost to each kind of our attack
        int lastEnemyHullSrc;       // what took the last point
        int enemyCrewKilled, ourCrewLost;
        int enemyShots;             // volleys the enemy has fired
        int weaponFires[4];         // volleys each of our weapons has fired
        float cloakAvoided;         // damage our cloak has dodged since it went up
        bool cloakFullHull;         // the enemy was untouched when our cloak went up
        int hitStreak;              // enemy shots in a row we didn't dodge with engines at 8
        float roomBeamTime[64];     // when our beams last swept each enemy room (+1, 0 never)
        bool allRoomsBeamed;        // every room inside 5 s
        int boarderKills[4];        // enemy crew killed by our boarding drone in each slot
        int bioSerial, bioKills;    // Anti-Bio Beam kills by one shot
        bool burningKill;           // our crew killed an enemy standing in a fire on their ship
        bool enemyDefenseDrone;     // the enemy had a defence drone out
    } fight;
    int shotSerial;  // numbers each shot, so kills can be traced to one
    // Since main last looked (it adds them to the run and clears them).
    int shotsFired, missileShots, dronesUsed, offensiveDrones, teleports, ourDeaths;
} Feats;

extern Feats gFeats;
