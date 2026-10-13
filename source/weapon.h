#pragma once
// Weapon blueprints (<weaponBlueprint>) and blueprint lists (<blueprintList>) from ftl.dat.

#include <stdbool.h>

#include "ftldat.h"

enum { WEAPON_LASER, WEAPON_MISSILES, WEAPON_BEAM, WEAPON_ION, WEAPON_BOMB, WEAPON_BURST, WEAPON_OTHER };

#define MAX_WEAPON_SOUNDS 3

typedef struct {
    char name[32];       // blueprint name, e.g. LASER_BURST_3
    char shortName[24];  // display name, e.g. "Burst Laser II"
    int type;            // WEAPON_*
    int damage, shots, sp, power, missiles, ion;
    float cooldown;      // seconds
    int speed;           // projectile speed in the game's units (<speed>, or the type's default)
    int length;          // beams: sweep length in ship pixels
    int radius;          // flak: landing spread around the target, in ship pixels
    int flakShots;       // flak: projectiles that can hit (not the "fake" ones)
    int sysDamage, persDamage;  // extra system damage and crew damage (x15 HP) on top of `damage`
    int hullBust;               // extra hull damage multiplier against rooms without a system
    int chargeLevels;           // charge weapons store this many volleys and fire them together
    float boostAmount;          // chain weapons: cooldown cut per volley ...
    int boostCount;             // ... up to this many times (reset when unpowered)
    float stun;                 // seconds crew in the hit room are stunned
    int stunChance;             // out of 10: chance of a 3 s stun
    int fireChance, breachChance;  // out of 10
    int lockdown;                  // <lockdown>: crystal-coats the room it hits
    char image[32];      // projectile animation
    char art[32];        // weaponAnim drawn on the hull
    char explosion[32];  // <explosion>: the hit's animation ("" or explosion_random: one of the big ones)
    char launch[MAX_WEAPON_SOUNDS][24], hitShip[MAX_WEAPON_SOUNDS][24], hitShield[MAX_WEAPON_SOUNDS][24];
    int launchCount, hitShipCount, hitShieldCount;
} WeaponDef;

enum {
    DRONE_COMBAT, DRONE_DEFENSE, DRONE_SHIP_REPAIR, DRONE_SHIELD,  // fly outside
    DRONE_CREW_REPAIR, DRONE_BATTLE, DRONE_BOARDER,               // walk around inside, like crew
    DRONE_ION_INTRUDER,                                           // a boarder that ionises systems
    DRONE_INTERNAL                                                // not supported yet (hacking)
};
enum { DRONE_TARGET_MISSILES, DRONE_TARGET_LASERS, DRONE_TARGET_DRONES };  // what defence drones shoot

typedef struct {
    char name[32];
    char shortName[24];
    int type;          // DRONE_*
    int power;
    float cooldown;    // seconds (defence drones' shot cooldown, shield drones' recharge)
    int speed;         // flight speed in the game's units
    int target;        // DRONE_TARGET_*
    char image[32];    // img/ship/drones/<image>_base.png, _on.png, _charged.png
    char icon[16];     // img/systemUI/weapbox_icon_D_<icon>.png, FTL's 26x26 drone icon
    const WeaponDef* weapon;  // what it fires, if anything
} DroneDef;

bool weapons_load(FtlDat* dat);
void weapons_free(void);
const WeaponDef* weapon_find(const char* name);
const DroneDef* drone_find(const char* name);

// External drones (combat, defence, hull repair, shield) are supported; ones that work inside a
// ship (repair, battle, boarding, hacking) aren't yet.
bool drone_supported(const DroneDef* d);

// Names in a <blueprintList>, e.g. WEAPONS_REBEL. Returns how many were written to out.
int weapon_list(const char* listName, const char** out, int max);

// Whether combat supports this weapon (everything but unknown types).
bool weapon_supported(const WeaponDef* w);
