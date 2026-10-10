#include "combat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "feats.h"
#include "life.h"
#include "sound.h"

// Numbers from the FTL wiki (ftl.fandom.com: Shields, Engines, Piloting, Crew skills, Weapons), Mike
// Hopley's measurements (mikehopley.github.io) and xftl, a reimplementation built from the
// reverse-engineered game (gitlab.com/znixian/xftl), unless marked as our own choice.
static const float SHIELD_LAYER_TIME[5] = {2.0f, 2.0f, 1.72f, 1.5f, 1.33f};  // seconds for the n-th layer
// Manning bonuses by skill level 0/1/2 (wiki: Crew skills).
static const int MAN_EVASION[3] = {5, 7, 10};              // piloting and engines: + evasion
static const float SHIELDS_MANNED_RATE[3] = {1.1f, 1.2f, 1.3f};   // faster recharge
static const float WEAPONS_MANNED_TIME[3] = {0.9f, 0.85f, 0.8f};  // charge time
static const float REPAIR_SKILL_RATE[3] = {1.0f, 1.1f, 1.2f};     // repairs and fire fighting
static const float COMBAT_SKILL_DAMAGE[3] = {1.0f, 1.1f, 1.2f};  // hand-to-hand damage
#define CREW_DAMAGE 15.0f           // crew health lost per point of weapon damage
#define REPAIR_SECONDS 12.5f        // untrained Human, one system bar or one hull breach
#define FIRE_FIGHT_RATE 8.0f        // fire health per second x race fire multiplier x race repair speed
// Delay before a shot leaves the muzzle, by weapon family (Hopley).
static float fire_delay(const WeaponDef* d) {
    switch (d->type) {
        case WEAPON_ION: return 0.17f;
        case WEAPON_MISSILES: return 0.16f;
        case WEAPON_BURST: return 0.11f;
        default: return 0.14f;
    }
}

// Beam firing delay and sweep time (Hopley), by blueprint; others use the Halberd's.
static void beam_timing(const WeaponDef* d, float* delay, float* duration) {
    static const struct { const char* name; float delay, duration; } T[] = {
        {"BEAM_1", 0.30f, 0.95f},  {"BEAM_LONG", 0.24f, 0.84f}, {"BEAM_HULL", 0.34f, 1.25f},
        {"BEAM_2", 0.24f, 1.00f},  {"BEAM_3", 0.24f, 1.00f},    {"BEAM_BIO", 0.29f, 0.68f},
        {"BEAM_FIRE", 0.24f, 1.75f},
    };
    *delay = 0.24f;
    *duration = 1.0f;
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (!strcmp(d->name, T[i].name)) {
            *delay = T[i].delay;
            *duration = T[i].duration;
        }
}
#define DIST_TO_ENEMY 44.0f         // speed units from player mounts to enemy shields (mean of measured)
#define DIST_TO_PLAYER 70.0f        // from enemy mounts to player shields
#define SHOT_SPACING 0.25f          // seconds between shots of a volley (dual/burst shot duration)
#define UNPOWERED_DRAIN 10.0f       // charge (seconds) lost per second by an unpowered weapon (xftl)
#define ION_SECONDS 5.0f            // per point of ion damage (wiki: Ion weapons)
#define CLOAK_PER_POWER 5.0f        // xftl Cloaking.kt / wiki Cloaking
#define CLOAK_EVASION 60
#define CLOAK_FIRE_PENALTY 0.2f     // share of the full cloak lost per non-beam weapon fired
#define SYSTEM_RECHARGE 20.0f       // cloak and battery cooldown
#define BATTERY_SECONDS 30.0f       // xftl BackupBattery.kt
#define ION_MAX 25.0f
#define BOMB_TELEPORT 0.95f         // bombs dodge-check here, then materialise ...
#define BOMB_EXPLODE 0.75f          // ... and explode this much later
#define EFFECT_TIME 0.75f

// Evasion by engine power, before manning bonuses.
static const int ENGINE_EVASION[9] = {0, 5, 10, 15, 20, 25, 28, 31, 35};

static void play_random(const char (*sounds)[24], int count) {
    if (count > 0) sound_play(sounds[rand() % count]);
}

// Whether a shot is dodged, rolled when it's fired so its path can be drawn going past; resolve()
// corrects it for any change in the target's evasion while it flew (cloaking against shots already
// on their way works, as in FTL, where evasion counts when the shot arrives).
static void roll_miss(Projectile* p, int evade) {
    if (evade < 0) evade = 0;
    if (evade > 100) evade = 100;
    p->evade = (signed char)evade;
    p->miss = rand() % 100 < evade;
}

static bool shield_drone_absorbs(Combat* c, int side);
static int free_tiles(const Ship* s, const CrewSet* cs, int room, int out[][2], int max, bool foreign);
static void drones_sync(Ship* s);

// Hazard shots (wiki: Environmental Hazards; xftl AsteroidProjectile): an asteroid is a 1-damage
// shot with 20% fire / 10% breach chances at missile speed that shields stop and defence drones can
// shoot down; an anti-ship battery shot does 3 damage through any shield and always breaches.
Feats gFeats;
static int sDmgSrc;  // SRC_* of the hit damage_room is applying, for achievements

static WeaponDef sAsteroidDef, sPdsDef, sFlareDef, sSurgeDef, sShardDef;
static WeaponSlot sShardSlot;  // Crystal Vengeance's shard: its art, for drawing

static void hazard_defs_init(void) {
    static bool ready;
    if (ready) return;
    ready = true;
    WeaponDef* a = &sAsteroidDef;
    snprintf(a->name, sizeof(a->name), "ASTEROID");
    snprintf(a->shortName, sizeof(a->shortName), "Asteroid");
    a->type = WEAPON_MISSILES;  // a target for defence drones
    a->damage = 1;
    a->shots = a->chargeLevels = 1;
    a->speed = 35;
    a->fireChance = 2;
    a->breachChance = 1;
    for (int k = 0; k < 3; k++) {
        snprintf(a->hitShip[k], sizeof(a->hitShip[k]), "hitHull%d", k + 1);
        snprintf(a->hitShield[k], sizeof(a->hitShield[k]), "hitShield%d", k + 1);
    }
    a->hitShipCount = a->hitShieldCount = 3;
    WeaponDef* p = &sPdsDef;
    *p = *a;
    snprintf(p->name, sizeof(p->name), "PDS");
    snprintf(p->shortName, sizeof(p->shortName), "Anti-Ship Battery");
    p->type = WEAPON_LASER;
    p->damage = 3;
    p->sp = 10;
    p->speed = 60;
    p->fireChance = 0;
    p->breachChance = 10;
    sFlareDef.damage = 1;  // a solar flare's hit: 1 hull and system damage, nothing else
    sFlareDef.shots = sFlareDef.chargeLevels = 1;
    // The flagship's surge lasers: Heavy Laser I, but 1 damage (wiki: The Rebel Flagship).
    const WeaponDef* heavy = weapon_find("LASER_HEAVY_1");
    if (heavy) {
        sSurgeDef = *heavy;
    } else {
        sSurgeDef = sPdsDef;
        sSurgeDef.sp = 0;
    }
    sSurgeDef.damage = 1;
    sSurgeDef.shots = sSurgeDef.chargeLevels = 1;
    // Crystal Vengeance (wiki): 1 damage, 10% breach, 20% stun, through any shield; evasion and
    // defence drones still count. FTL's CRYSTAL_1 has the art and sounds.
    const WeaponDef* shard = weapon_find("CRYSTAL_1");
    if (shard) {
        sShardDef = *shard;
    } else {
        sShardDef = sAsteroidDef;
        sShardDef.fireChance = 0;
    }
    sShardDef.type = WEAPON_MISSILES;
    sShardDef.damage = 1;
    sShardDef.sp = 10;
    sShardDef.breachChance = 1;
    sShardDef.stunChance = 2;
    sShardDef.shots = sShardDef.chargeLevels = 1;
    if (sShardDef.speed <= 0) sShardDef.speed = 35;
}

const WeaponDef* combat_shot_def(const Combat* c, int from, int weapon, int drone) {
    if (weapon == SHOT_ASTEROID || weapon == SHOT_PDS || weapon == SHOT_SURGE || weapon == SHOT_SHARD) {
        hazard_defs_init();
        return weapon == SHOT_ASTEROID ? &sAsteroidDef : weapon == SHOT_PDS ? &sPdsDef : weapon == SHOT_SURGE ? &sSurgeDef : &sShardDef;
    }
    const Ship* s = c->ship[from];
    if (drone >= 0) return s->drones[drone].def->weapon;
    if (SHOT_IS_ART(weapon)) return s->artillery[SHOT_ART_SLOT(weapon)].def;
    if (weapon < 0) return NULL;
    return s->weapons[weapon].def;
}

// ---- Loading -------------------------------------------------------------------

static bool load_png(FtlDat* dat, const char* path, Texture* t) {
    uint32_t size;
    uint8_t* png = ftldat_read(dat, path, &size);
    bool ok = png && texture_load_png(t, png, size);
    free(png);
    return ok;
}

// A weapon's launch and hit sounds, decoded ahead (the first hit of a new weapon would otherwise
// wait for its sound to decode).
static void prefetch_sounds(const WeaponDef* d) {
    if (!d) return;
    for (int i = 0; i < d->launchCount; i++) sound_prefetch(d->launch[i]);
    for (int i = 0; i < d->hitShipCount; i++) sound_prefetch(d->hitShip[i]);
    for (int i = 0; i < d->hitShieldCount; i++) sound_prefetch(d->hitShield[i]);
}

void combat_prefetch_ship_sounds(const Ship* s) {
    for (int i = 0; i < s->weaponCount; i++) prefetch_sounds(s->weapons[i].def);
    for (int i = 0; i < s->droneCount; i++) prefetch_sounds(s->drones[i].def->weapon);
    for (int k = 0; k < s->artilleryCount; k++) prefetch_sounds(s->artillery[k].def);
}

static void load_weapon_art(WeaponSlot* w, FtlDat* dat, const AnimDb* db) {
    prefetch_sounds(w->def);
    WeaponAnimInfo wa;
    SheetInfo sheet;
    if (animdb_weapon(db, w->def->art, &wa) && animdb_sheet(db, wa.sheet, &sheet) && load_png(dat, sheet.file, &w->art)) {
        w->artFw = sheet.fw;
        w->artFh = sheet.fh;
        w->artX = wa.x;
        w->artY = sheet.h / sheet.fh - 1 - wa.y;  // rows from the top
        w->artFrames = wa.length;
        w->chargedFrame = wa.chargedFrame;
        w->fireFrame = wa.fireFrame;
        w->mountPx = wa.mountX;
        w->mountPy = wa.mountY;
    }
    AnimInfo pa;
    if (animdb_anim(db, w->def->image, &pa) && animdb_sheet(db, pa.sheet, &sheet) && load_png(dat, sheet.file, &w->proj)) {
        w->projFw = sheet.fw;
        w->projFh = sheet.fh;
        w->projX = pa.x;
        w->projY = sheet.h / sheet.fh - 1 - pa.y;
        w->projFrames = pa.length;
        w->projTime = pa.time > 0 ? pa.time : 1.0f;
    }
}

void combat_load_shard(FtlDat* dat, const AnimDb* db) {
    if (sShardSlot.def) return;
    hazard_defs_init();
    sShardSlot.def = &sShardDef;
    load_weapon_art(&sShardSlot, dat, db);
}

const WeaponSlot* combat_shard_slot(void) { return sShardSlot.def ? &sShardSlot : NULL; }

static int weapons_system(const Ship* s) { return ship_find_system(s, SYS_WEAPONS); }

static int weapons_used(const Ship* s) {
    int used = 0;
    for (int i = 0; i < s->weaponCount; i++)
        if (s->weapons[i].powered) used += s->weapons[i].def->power;
    return used;
}

// The weapons system's power is whatever its powered weapons draw.
static void weapons_sync(Ship* s) {
    int ws = weapons_system(s);
    if (ws < 0) return;
    int cap = ship_system_capacity(s, ws);
    for (int i = s->weaponCount - 1; i >= 0 && weapons_used(s) > cap; i--) s->weapons[i].powered = false;
    s->systems[ws].power = weapons_used(s);
}

static bool sEnemyHasWeapons;  // combat_arm: the enemy's weapons cover hull damage and lasers

void combat_arm(Ship* s, FtlDat* dat, const AnimDb* db) {
    const ShipBlueprint* bp = s->bp;
    s->weaponCount = 0;
    const char* names[MAX_WEAPON_SLOTS];
    int count = 0;
    for (int i = 0; i < bp->weaponCount; i++) names[count++] = bp->weapons[i];
    sEnemyHasWeapons = count > 0;  // a fixed weapon list (bosses, events) is assumed to do the job

    if (count == 0 && bp->weaponLoad[0]) {
        // Enemies (xftl ShipGenerator): fill the weapons system's power with weapons that each use
        // over a quarter of what's left, until the ship can both hurt the hull and has a
        // laser-style weapon (lasers, or missiles that don't pierce shields).
        const char* list[48];
        int n = weapon_list(bp->weaponLoad, list, 48);
        int ws = weapons_system(s);
        int level = ws >= 0 ? s->systems[ws].def->level : 0, used = 0;
        int slots = bp->weaponSlots < s->mountCount ? bp->weaponSlots : s->mountCount;
        bool hull = false, laser = false;
        while (count < slots && level - used > 0) {
            int left = level - used;
            const WeaponDef* ok[48];
            int m = 0;
            for (int i = 0; i < n; i++) {
                const WeaponDef* w = weapon_find(list[i]);
                if (!w || !weapon_supported(w) || w->power > left) continue;
                if (w->power == left && w->power != 1) continue;
                if (used == 0 && left >= 3 && w->power < 2) continue;
                if (w->power < left / 4.0f) continue;
                bool h = w->damage != 0, l = w->type == WEAPON_LASER || (w->type == WEAPON_MISSILES && w->sp <= 3);
                if (!((!hull && h) || (!laser && l) || (hull && laser))) continue;
                ok[m++] = w;
            }
            if (!m) break;
            const WeaponDef* w = ok[rand() % m];
            hull |= w->damage != 0;
            laser |= w->type == WEAPON_LASER || (w->type == WEAPON_MISSILES && w->sp <= 3);
            names[count++] = w->name;
            used += w->power;
        }
        sEnemyHasWeapons = hull && laser;
        if (s->missiles == 0) s->missiles = 8;
    }

    for (int i = 0; i < count; i++) combat_add_weapon(s, dat, db, names[i]);
    weapons_sync(s);
    if (bp->enemy)
        for (int i = 0; i < s->weaponCount; i++) combat_toggle_weapon(s, i);

    // Each artillery system's weapon, with its art.
    s->artilleryCount = 0;
    for (int i = 0; i < s->systemCount && s->artilleryCount < MAX_ARTILLERY; i++) {
        const SystemDef* sd = s->systems[i].def;
        if (sd->type != SYS_ARTILLERY || !sd->weapon[0]) continue;
        const WeaponDef* wd = weapon_find(sd->weapon);
        if (!wd) continue;
        WeaponSlot* w = &s->artillery[s->artilleryCount];
        memset(w, 0, sizeof(*w));
        w->def = wd;
        w->target = -1;
        load_weapon_art(w, dat, db);
        s->artSys[s->artilleryCount++] = i;
    }

    // Drones: the blueprint's, or for enemies a random pick the drone system can power.
    s->droneCount = 0;
    s->droneParts = bp->droneParts;
    const char* dnames[MAX_WEAPON_SLOTS];
    int dcount = 0;
    for (int i = 0; i < bp->droneCount; i++) dnames[dcount++] = bp->drones[i];
    int dsys = ship_find_system(s, SYS_DRONES);
    if (dcount == 0 && bp->droneLoad[0] && dsys >= 0) {
        // xftl: drones that fit the power left (no single drone using all of a 4+ system),
        // no duplicates, and only combat drones if the weapons can't do the job.
        const char* list[48];
        int n = weapon_list(bp->droneLoad, list, 48);
        int level = s->systems[dsys].def->level, used = 0;
        while (dcount < bp->droneSlots && level - used > 0) {
            int left = level - used;
            const DroneDef* ok[48];
            int m = 0;
            for (int pass = 0; pass < 2 && m == 0; pass++)
                for (int i = 0; i < n; i++) {
                    const DroneDef* d = drone_find(list[i]);
                    if (!d || !drone_supported(d) || d->power > left || (level >= 4 && d->power >= level)) continue;
                    if (pass == 0) {
                        bool dup = false;
                        for (int k = 0; k < dcount; k++) dup |= !strcmp(dnames[k], d->name);
                        if (dup || (!sEnemyHasWeapons && d->type != DRONE_COMBAT)) continue;
                    }
                    ok[m++] = d;
                }
            if (!m) break;
            const DroneDef* d = ok[rand() % m];
            dnames[dcount++] = d->name;
            used += d->power;
        }
        if (s->droneParts < dcount * 2) s->droneParts = dcount * 2;
    }
    for (int i = 0; i < dcount; i++) combat_add_drone(s, dat, db, dnames[i]);
}

bool combat_add_weapon(Ship* s, FtlDat* dat, const AnimDb* db, const char* name) {
    const WeaponDef* def = weapon_find(name);
    if (!def || s->weaponCount >= MAX_WEAPON_SLOTS) return false;
    WeaponSlot* w = &s->weapons[s->weaponCount++];
    memset(w, 0, sizeof(*w));
    w->def = def;
    w->target = -1;
    load_weapon_art(w, dat, db);
    return true;
}

void combat_remove_weapon(Ship* s, int i) {
    if (i < 0 || i >= s->weaponCount) return;
    texture_free(&s->weapons[i].art);
    texture_free(&s->weapons[i].proj);
    memmove(&s->weapons[i], &s->weapons[i + 1], sizeof(WeaponSlot) * (s->weaponCount - i - 1));
    s->weaponCount--;
    weapons_sync(s);
}

void combat_remove_drone(Ship* s, int i) {
    if (i < 0 || i >= s->droneCount) return;
    DroneSlot* d = &s->drones[i];
    texture_free(&d->base);
    texture_free(&d->on);
    texture_free(&d->charged);
    texture_free(&d->proj);
    memmove(&s->drones[i], &s->drones[i + 1], sizeof(DroneSlot) * (s->droneCount - i - 1));
    s->droneCount--;
    drones_sync(s);
}

bool combat_add_drone(Ship* s, FtlDat* dat, const AnimDb* db, const char* name) {
    {
        const DroneDef* def = drone_find(name);
        if (!def || s->droneCount >= MAX_WEAPON_SLOTS) return false;
        DroneSlot* ds = &s->drones[s->droneCount++];
        memset(ds, 0, sizeof(*ds));
        ds->def = def;
        prefetch_sounds(def->weapon);
        char path[80];
        snprintf(path, sizeof(path), "img/ship/drones/%s_base.png", def->image);
        load_png(dat, path, &ds->base);
        snprintf(path, sizeof(path), "img/ship/drones/%s_on.png", def->image);
        load_png(dat, path, &ds->on);
        snprintf(path, sizeof(path), "img/ship/drones/%s_charged.png", def->image);
        load_png(dat, path, &ds->charged);
        AnimInfo pa;
        SheetInfo sheet;
        if (def->weapon && animdb_anim(db, def->weapon->image, &pa) && animdb_sheet(db, pa.sheet, &sheet) &&
            load_png(dat, sheet.file, &ds->proj)) {
            ds->projFw = sheet.fw;
            ds->projFh = sheet.fh;
            ds->projX = pa.x;
            ds->projY = sheet.h / sheet.fh - 1 - pa.y;
            ds->projFrames = pa.length;
            ds->projTime = pa.time > 0 ? pa.time : 1.0f;
        }
    }
    return true;
}

// ---- Power ---------------------------------------------------------------------

// Powers a weapon if the weapons system and reactor have room; doesn't change `wanted`.
static bool power_weapon(Ship* s, int i) {
    WeaponSlot* w = &s->weapons[i];
    int ws = weapons_system(s);
    if (ws < 0 || !weapon_supported(w->def) || s->systems[ws].ionTimer > 0.0f) return false;
    if (weapons_used(s) + w->def->power > ship_system_capacity(s, ws)) return false;
    if (w->def->power > ship_reactor_free(s)) return false;
    w->powered = true;
    weapons_sync(s);
    return true;
}

bool combat_toggle_weapon(Ship* s, int i) {
    WeaponSlot* w = &s->weapons[i];
    int ws = weapons_system(s);
    if (ws < 0) return false;
    if (w->powered || w->wanted) {
        w->powered = false;
        w->wanted = false;
        w->target = -1;  // switching a weapon off cancels its targeting, as in FTL
        w->beamAimed = false;
        weapons_sync(s);
        return true;
    }
    if (power_weapon(s, i)) {
        w->wanted = true;
        return true;
    }
    return false;
}

void combat_set_weapons_power(Ship* s, int power) {
    for (int i = 0; i < s->weaponCount; i++) s->weapons[i].powered = s->weapons[i].wanted = false;
    weapons_sync(s);
    for (int i = 0; i < s->weaponCount; i++)
        if (weapons_used(s) + s->weapons[i].def->power <= power) combat_toggle_weapon(s, i);
}

float combat_shield_progress(const Ship* s) {
    if (s->shieldLayers >= combat_max_shields(s)) return -1.0f;
    float k = s->shieldCharge / SHIELD_LAYER_TIME[s->shieldLayers < 5 ? s->shieldLayers : 4];
    return k > 1.0f ? 1.0f : k;
}

int combat_max_shields(const Ship* s) {
    return ship_system_power(s, ship_find_system(s, SYS_SHIELDS)) / 2;
}

// Auto-ships (no crew at all) get the manning bonuses everywhere, as in the game.
// The flagship with its crew all dead: "the AI took control" (wiki: The Rebel Flagship).
static bool ai_controlled(const Combat* c, const Ship* s) {
    return c && c->bossStage && s == c->ship[SIDE_ENEMY] && c->crew[SIDE_ENEMY] && crew_native_count(c->crew[SIDE_ENEMY]) == 0;
}

static bool is_manned(const Combat* c, const Ship* s, int sys) {
    if (sys < 0 || s->systems[sys].ionTimer > 0.0f) return false;  // ionised systems can't be manned
    // The AI mans every undamaged system, hacked or not.
    if (ai_controlled(c, s)) return s->systems[sys].damage == 0;
    bool autoShip = s->bp->enemy && s->bp->crewKinds == 0;
    if (s->hackedSys == sys && !autoShip) return false;
    if (s->systems[sys].manned) return true;
    for (int side = 0; side < 2; side++)
        if (c && c->ship[side] == s) return s->bp->enemy && crew_native_count(c->crew[side]) == 0 && s->bp->crewKinds == 0;
    return false;
}

// The skill level of whoever mans a system, or -1 if no one does (auto-ships count as level 0).
static int man_level(const Combat* c, const Ship* s, int sys) {
    if (!is_manned(c, s, sys)) return -1;
    return s->systems[sys].manned ? s->systems[sys].manLevel : 0;
}

// One point of experience for our crew member manning a system of the given type.
static void manning_xp(Combat* c, int side, int sysType, int skill) {
    if (side != SIDE_PLAYER || c->result != COMBAT_FIGHTING) return;  // enemy crew never learn
    Ship* s = c->ship[side];
    int si = ship_find_system(s, sysType);
    if (si < 0 || !s->systems[si].manned || s->systems[si].manCrew < 0) return;
    CrewSet* cs = c->crew[side];
    if (s->systems[si].manCrew < cs->count) crew_add_xp(cs, &cs->crew[(int)s->systems[si].manCrew], skill);
}

static const Combat* sCombat;  // for evasion queries from the UI

static int evasion_base(const Combat* c, const Ship* s);

static int evasion(const Combat* c, const Ship* s) {
    return evasion_base(c, s) + (s->cloakTime > 0.0f ? CLOAK_EVASION : 0);
}

static int evasion_base(const Combat* c, const Ship* s) {
    int pilot = ship_find_system(s, SYS_PILOT), engines = ship_find_system(s, SYS_ENGINES);
    if (pilot < 0 || engines < 0 || ship_system_capacity(s, pilot) == 0) return 0;
    // A hacking pulse on piloting (as on engines, via its power) leaves no evasion (wiki: Engines).
    if (s->hackPulse > 0.0f && s->hackedSys == pilot) return 0;
    int p = ship_system_power(s, engines);
    if (p <= 0) return 0;
    int el = man_level(c, s, engines);
    int e = ENGINE_EVASION[p < 8 ? p : 8] + (el >= 0 ? MAN_EVASION[el] : 0);

    // Someone must be in the piloting room; otherwise piloting level 2/3 flies on autopilot at
    // 50%/80% of the engines' evasion.
    bool crewInPilot = is_manned(c, s, pilot) || ai_controlled(c, s);
    for (int side = 0; side < 2 && !crewInPilot && c; side++) {
        if (c->ship[side] != s) continue;
        const CrewSet* cs = c->crew[side];
        // Stunned or mind-controlled crew can't fly (wiki: Piloting); crew drones don't count.
        for (int i = 0; i < cs->count && !crewInPilot; i++) {
            const Crew* m = &cs->crew[i];
            crewInPilot = !m->foreign && !m->drone && m->stun <= 0.0f && m->mind <= 0.0f &&
                          ship_room_at(s, m->destX, m->destY) == s->systems[pilot].room;
        }
    }
    if (!crewInPilot) {
        int lvl = ship_system_capacity(s, pilot);
        return lvl >= 3 ? e * 8 / 10 : lvl == 2 ? e / 2 : 0;
    }
    int pl = man_level(c, s, pilot);
    return e + (pl >= 0 ? MAN_EVASION[pl] : 0);
}

int combat_evasion(const Ship* s) { return evasion(sCombat, s); }

bool combat_can_charge_ftl(const Combat* c, int side) {
    const Ship* s = c->ship[side];
    if (!s) return false;
    int eng = ship_find_system(s, SYS_ENGINES), pil = ship_find_system(s, SYS_PILOT);
    if (eng < 0 || pil < 0 || ship_system_power(s, eng) <= 0 || ship_system_capacity(s, pil) <= 0) return false;
    if (s->hackPulse > 0.0f && (s->hackedSys == eng || s->hackedSys == pil)) return false;
    if (s->bp->enemy && s->bp->crewKinds == 0) return true;  // auto-ship
    const CrewSet* cs = c->crew[side];
    for (int i = 0; i < cs->count; i++) {
        const Crew* m = &cs->crew[i];
        if (!m->foreign && !m->drone && m->stun <= 0.0f && m->mind <= 0.0f &&
            ship_room_at(s, (int)floorf(m->x / TILE), (int)floorf(m->y / TILE)) == s->systems[pil].room)
            return true;
    }
    return false;
}

bool combat_cloak(Combat* c, int side) {
    Ship* s = c->ship[side];
    int ci = ship_find_system(s, SYS_CLOAKING);
    if (ci < 0 || ship_system_power(s, ci) <= 0 || s->systems[ci].ionTimer > 0.0f || s->cloakTime > 0.0f) return false;
    s->cloakTime = ship_system_power(s, ci) * CLOAK_PER_POWER;
    sound_play("cloak");
    return true;
}

bool combat_battery(Combat* c, int side) {
    Ship* s = c->ship[side];
    int bi = ship_find_system(s, SYS_BATTERY);
    if (bi < 0 || ship_system_capacity(s, bi) <= 0 || s->systems[bi].ionTimer > 0.0f || s->batteryTime > 0.0f) return false;
    s->batteryTime = BATTERY_SECONDS;
    s->batteryBonus = ship_system_capacity(s, bi) * 2;
    sound_play("batteryStart");
    return true;
}

// ---- Geometry ------------------------------------------------------------------

void combat_mount_point(const Ship* s, int weapon, float* x, float* y) {
    float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    if (weapon < s->mountCount) {
        *x = ox + s->hullX + s->mounts[weapon].x;
        *y = oy + s->hullY + s->mounts[weapon].y;
    } else {
        *x = ox + s->hullX + s->hull.width * 0.5f;
        *y = oy + s->hullY + s->hull.height * 0.5f;
    }
}

// A random tile centre inside a room, X/Y_OFFSET included.
static void room_point(const Ship* s, int room, float* x, float* y) {
    const Room* r = &s->rooms[room];
    *x = (s->xOffset + r->x + rand() % r->w) * TILE + TILE * 0.5f;
    *y = (s->yOffset + r->y + rand() % r->h) * TILE + TILE * 0.5f;
}

// ---- Combat --------------------------------------------------------------------

static int crew_owned_by(const Combat* c, int owner);

void combat_jump_reset(Ship* player, CrewSet* playerCrew) {
    player->superShield = player->superShieldMax = player->aug[AUG_ENERGY_SHIELD] ? 5 : 0;
    memset(player->lockdown, 0, sizeof(player->lockdown));
    for (int i = 0; i < playerCrew->count; i++) playerCrew->crew[i].ability = 0.0f;  // the lockdown is ready again
    // Jumping loses the weapons' charge (FTL); the Pre-Igniter gives it back to powered weapons.
    for (int i = 0; i < player->weaponCount; i++) {
        WeaponSlot* w = &player->weapons[i];
        w->chain = 0;
        w->charge = 0.0f;
        if (player->aug[AUG_PREIGNITE] && w->powered) w->charge = w->fullCharge > 0 ? w->fullCharge : w->def->cooldown * w->def->chargeLevels;
    }
}

void combat_start(Combat* c, Ship* player, CrewSet* playerCrew, Ship* enemy, CrewSet* enemyCrew) {
    bool autofire = c->autofire, hold = c->enemyHoldFire;
    int difficulty = c->difficulty;
    memset(c, 0, sizeof(*c));
    c->difficulty = difficulty;
    c->autofire = autofire;
    c->enemyHoldFire = hold;
    sCombat = c;
    for (int i = 0; i < player->weaponCount; i++) {
        player->weapons[i].target = -1;
        player->weapons[i].beamAimed = false;
        player->weapons[i].burstLeft = 0;
    }
    player->cloakTime = player->batteryTime = 0.0f;
    player->hackedSys = -1;
    for (int k = 0; k < player->artilleryCount; k++) {
        player->artillery[k].charge = 0.0f;
        player->artillery[k].burstLeft = 0;
    }
    player->hackPulse = player->hackFlight = 0.0f;
    player->batteryBonus = 0;
    if (enemy) {
        combat_jump_reset(player, playerCrew);  // a new fight starts fresh, as after a jump
        memset(&gFeats.fight, 0, sizeof(gFeats.fight));
    }
    // External drones are lost between fights and must be redeployed (Drone Recovery Arm: as parts).
    for (int i = 0; i < player->droneCount; i++) {
        DroneSlot* d = &player->drones[i];
        if (d->def->type == DRONE_CREW_REPAIR || d->def->type == DRONE_BATTLE) continue;
        if (d->deployed && player->aug[AUG_DRONE_RECOVERY] && d->def->type != DRONE_BOARDER && d->def->type != DRONE_ION_INTRUDER)
            player->droneParts++;
        d->deployed = d->powered = d->wanted = false;
        d->boardTimer = 0.0f;
    }
    drones_sync(player);
    c->ship[SIDE_PLAYER] = player;
    c->ship[SIDE_ENEMY] = enemy;
    c->crew[SIDE_PLAYER] = playerCrew;
    c->crew[SIDE_ENEMY] = enemyCrew;
    // Only a crew that was there can be killed: an enemy that came without any (or whose crew
    // couldn't be set up) isn't an instant "crew killed" win.
    c->enemyHadCrew = enemy && enemyCrew && crew_owned_by(c, SIDE_ENEMY) > 0;
}

void combat_set_target(Combat* c, int weapon, int room) {
    Ship* s = c->ship[SIDE_PLAYER];
    if (weapon >= 0 && weapon < s->weaponCount) {
        s->weapons[weapon].target = room;
        s->weapons[weapon].beamAimed = false;
    }
}

void combat_set_beam(Combat* c, int weapon, float x, float y, float angle) {
    Ship* s = c->ship[SIDE_PLAYER];
    Ship* o = c->ship[SIDE_ENEMY];
    if (weapon < 0 || weapon >= s->weaponCount || !o) return;
    WeaponSlot* w = &s->weapons[weapon];
    w->target = ship_room_at(o, (int)floorf(x / TILE) - o->xOffset, (int)floorf(y / TILE) - o->yOffset);
    w->beamAimed = w->target >= 0;
    w->beamX = x;
    w->beamY = y;
    w->beamAngle = angle;
}

// The explosion a hit shows: the weapon's own <explosion> (ion, crystal, lockdown, healing...), or
// for weapons without one (and "explosion_random") one of FTL's big explosions at random.
static const char* explosion_for(const WeaponDef* d) {
    static const char* const RANDOM[] = {"explosion1", "explosion_big1", "explosion_big2", "explosion_big3", "explosion_big4"};
    if (d && d->explosion[0] && strcmp(d->explosion, "explosion_random")) return d->explosion;
    return RANDOM[rand() % 5];
}

static void add_effect(Combat* c, int side, int kind, float x, float y) {
    for (int i = 0; i < MAX_EFFECTS; i++)
        if (!c->effects[i].active) {
            c->effects[i] = (Effect){true, side, kind, x, y, 0.0f, kind == FX_EXPLOSION ? explosion_for(NULL) : NULL};
            return;
        }
}

// An explosion for a hit by weapon d.
static void add_hit(Combat* c, int side, float x, float y, const WeaponDef* d) {
    add_effect(c, side, FX_EXPLOSION, x, y);
    for (int i = MAX_EFFECTS - 1; i >= 0; i--)
        if (c->effects[i].active && c->effects[i].t == 0.0f && c->effects[i].kind == FX_EXPLOSION && c->effects[i].x == x &&
            c->effects[i].y == y) {
            c->effects[i].anim = explosion_for(d);
            return;
        }
}

// Rooms a beam from (x,y) in direction (dx,dy) over `len` pixels would cross.
static int beam_rooms(const Ship* o, float x, float y, float dx, float dy, float len) {
    unsigned long long seen = 0;
    int n = 0;
    for (float d = 0; d <= len; d += 6.0f) {
        int r = ship_room_at(o, (int)floorf((x + dx * d) / TILE) - o->xOffset, (int)floorf((y + dy * d) / TILE) - o->yOffset);
        if (r >= 0 && !(seen & (1ull << r))) {
            seen |= 1ull << r;
            n++;
        }
    }
    return n;
}

static void fire_beam(Combat* c, int side, int wi) {
    Ship* o = c->ship[!side];
    WeaponSlot* w = &c->ship[side]->weapons[wi];
    for (int i = 0; i < MAX_BEAMS; i++) {
        Beam* b = &c->beams[i];
        if (b->active) continue;
        memset(b, 0, sizeof(*b));
        b->active = true;
        b->from = side;
        b->weapon = wi;
        b->drone = -1;
        const Room* r = &o->rooms[w->target];
        b->sx = (o->xOffset + r->x + r->w * 0.5f) * TILE;
        b->sy = (o->yOffset + r->y + r->h * 0.5f) * TILE;
        // The player's beams follow the path they drew; enemies sweep in a random direction from the
        // room's centre, as in the game.
        float a = (rand() % 8) * (float)M_PI / 4;
        if (side == SIDE_PLAYER && w->beamAimed) {
            b->sx = w->beamX;
            b->sy = w->beamY;
            a = w->beamAngle;
        } else if (side == SIDE_PLAYER) {
            int best = 0, bestRooms = -1;
            for (int k = 0; k < 8; k++) {
                float ka = k * (float)M_PI / 4;
                int n = beam_rooms(o, b->sx, b->sy, cosf(ka), sinf(ka), (float)w->def->length);
                if (n > bestRooms) {
                    bestRooms = n;
                    best = k;
                }
            }
            a = best * (float)M_PI / 4;
        }
        b->ex = b->sx + cosf(a) * w->def->length;
        b->ey = b->sy + sinf(a) * w->def->length;
        b->serial = ++gFeats.shotSerial;
        beam_timing(w->def, &b->delay, &b->duration);
        break;
    }
}

static void fire_shot(Combat* c, int side, int wi) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    WeaponSlot* w = &s->weapons[wi];
    if (w->target < 0 || w->target >= o->roomCount) return;
    w->fireAnim = 0.0f;
    play_random(w->def->launch, w->def->launchCount);
    if (w->def->type == WEAPON_BEAM) {
        fire_beam(c, side, wi);
        return;
    }
    int count = w->def->type == WEAPON_BURST && w->def->flakShots > 0 ? w->def->flakShots : 1;
    for (int n = 0; n < count; n++) {
        Projectile* p = NULL;
        for (int i = 0; i < MAX_SHOTS && !p; i++)
            if (!c->shots[i].active) p = &c->shots[i];
        if (!p) break;
        memset(p, 0, sizeof(*p));
        p->evade = -1;
        p->active = true;
        p->from = side;
        p->weapon = wi;
        p->drone = -1;
        p->room = w->target;
        combat_mount_point(s, wi, &p->sx, &p->sy);
        if (w->def->type == WEAPON_BURST) {
            // Flak lands anywhere in a circle around the target room's centre.
            const Room* r = &o->rooms[w->target];
            float ang = (rand() % 628) / 100.0f, rad = w->def->radius * sqrtf((rand() % 1000) / 1000.0f);
            p->tx = (o->xOffset + r->x + r->w * 0.5f) * TILE + cosf(ang) * rad;
            p->ty = (o->yOffset + r->y + r->h * 0.5f) * TILE + sinf(ang) * rad;
            p->room = ship_room_at(o, (int)floorf(p->tx / TILE) - o->xOffset, (int)floorf(p->ty / TILE) - o->yOffset);
            p->outside = p->room < 0;
        } else {
            room_point(o, w->target, &p->tx, &p->ty);
        }
        if (w->def->type == WEAPON_BOMB) {
            p->duration = BOMB_TELEPORT + BOMB_EXPLODE;
        } else {
            float dist = side == SIDE_PLAYER ? DIST_TO_ENEMY : DIST_TO_PLAYER;
            p->duration = fire_delay(w->def) + dist / (w->def->speed > 0 ? w->def->speed : 60) + n * 0.05f;
        }
        p->speed = (float)w->def->speed;
        roll_miss(p, evasion(c, o));
    }
}

// One shot of an artillery volley (slot k), from the artillery's room at the room it picked.
static void fire_artillery_shot(Combat* c, int side, int k) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    WeaponSlot* w = &s->artillery[k];
    if (!o || w->target < 0 || w->target >= o->roomCount) return;
    Projectile* p = NULL;
    for (int i = 0; i < MAX_SHOTS && !p; i++)
        if (!c->shots[i].active) p = &c->shots[i];
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->evade = -1;
    p->active = true;
    p->from = side;
    p->weapon = SHOT_ART(k);
    p->drone = -1;
    p->room = w->target;
    int mount = s->mountCount - s->artilleryCount + k;  // where its gun is drawn
    if (mount >= s->weaponCount && mount >= 0 && mount < s->mountCount) {
        combat_mount_point(s, mount, &p->sx, &p->sy);
    } else {
        const Room* ar = &s->rooms[s->systems[s->artSys[k]].room];
        p->sx = (s->xOffset + ar->x + ar->w * 0.5f) * TILE;
        p->sy = (s->yOffset + ar->y + ar->h * 0.5f) * TILE;
    }
    room_point(o, w->target, &p->tx, &p->ty);
    float dist = side == SIDE_PLAYER ? DIST_TO_ENEMY : DIST_TO_PLAYER;
    p->duration = fire_delay(w->def) + dist / (w->def->speed > 0 ? w->def->speed : 60);
    p->speed = (float)w->def->speed;
    roll_miss(p, evasion(c, o));
}

// Crystal Vengeance: a shard from the damaged ship (`side`) at a random room of the other.
static void shard_shot(Combat* c, int side) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    if (!o || o->roomCount == 0 || o->hullHp <= 0 || c->result != COMBAT_FIGHTING) return;
    Projectile* p = NULL;
    for (int i = 0; i < MAX_SHOTS && !p; i++)
        if (!c->shots[i].active) p = &c->shots[i];
    if (!p) return;
    const WeaponDef* d = combat_shot_def(c, side, SHOT_SHARD, -1);
    memset(p, 0, sizeof(*p));
    p->evade = -1;
    p->active = true;
    p->from = side;
    p->weapon = SHOT_SHARD;
    p->drone = -1;
    p->room = rand() % o->roomCount;
    const Room* sr = &s->rooms[rand() % s->roomCount];
    p->sx = (s->xOffset + sr->x + sr->w * 0.5f) * TILE;
    p->sy = (s->yOffset + sr->y + sr->h * 0.5f) * TILE;
    room_point(o, p->room, &p->tx, &p->ty);
    float dist = side == SIDE_PLAYER ? DIST_TO_ENEMY : DIST_TO_PLAYER;
    p->duration = dist / d->speed;
    p->speed = (float)d->speed;
    roll_miss(p, evasion(c, o));
    play_random(d->launch, d->launchCount);
}

// Reverse Ion Field: one resists ion half the time, two always (wiki: Augmentations).
static bool ion_resisted(const Ship* s) {
    return s->aug[AUG_ION_ARMOR] >= 2 || (s->aug[AUG_ION_ARMOR] && rand() % 2);
}

static void ion_system(Ship* s, int si, int amount);

static void apply_ion(Ship* s, int si, int amount) {
    if (si < 0 || amount <= 0 || ion_resisted(s)) return;
    ion_system(s, si, amount);
}

static void ion_system(Ship* s, int si, int amount) {
    if (si < 0 || amount <= 0) return;
    ShipSystem* y = &s->systems[si];
    int undamaged = y->def->level - y->damage;
    int base = y->ionLimit >= 0 ? y->ionLimit
               : system_is_subsystem(y->def->type) ? undamaged
               : (y->power < undamaged ? y->power : undamaged);
    y->ionLimit = base - amount > 0 ? base - amount : 0;
    y->ionTimer += ION_SECONDS * amount;
    if (y->ionTimer > ION_MAX) y->ionTimer = ION_MAX;
    if (y->def->type == SYS_WEAPONS) weapons_sync(s);
}

static void shard_shot(Combat* c, int side);

#define LOCKDOWN_SECONDS 12.0f  // wiki: Crystal Lockdown
#define LOCKDOWN_RECHARGE 50.0f

void combat_lockdown(Combat* c, int side, int room) {
    Ship* s = c->ship[side];
    if (!s || room < 0 || room >= s->roomCount) return;
    s->lockdown[room] = LOCKDOWN_SECONDS;
    sound_play(rand() % 2 ? "lockdown1" : "lockdown2");
}

bool combat_crystal_lockdown(Combat* c, int side, int ci) {
    CrewSet* cs = c->crew[side];
    if (ci < 0 || ci >= cs->count) return false;
    Crew* m = &cs->crew[ci];
    if (strcmp(cs->races[m->race].name, "crystal") || m->ability > 0.0f || m->stun > 0.0f) return false;
    // Crew of ours over on their ship lock down the room they're in there.
    Ship* s = c->ship[m->foreign ? !side : side];
    if (!s) return false;
    int room = ship_room_at(s, (int)floorf(m->x / TILE), (int)floorf(m->y / TILE));
    if (room < 0) return false;
    combat_lockdown(c, m->foreign ? !side : side, room);
    m->ability = LOCKDOWN_RECHARGE;
    return true;
}

static void damage_room(Combat* c, int side, int room, int hullDamage, const WeaponDef* d, bool crewToo) {
    Ship* s = c->ship[side];
    CrewSet* cs = c->crew[side];
    int si = ship_system_in_room(s, room);
    int hd = hullDamage > 0 ? hullDamage : 0;
    int hullLoss = hd * (si < 0 ? 1 + d->hullBust : 1);
    // Rock Plating: 15% to shrug off the hull damage, not from flares or sure-breach shots (wiki).
    bool flare = d == &sFlareDef;
    if (hullLoss > 0 && s->aug[AUG_ROCK_ARMOR] && !flare && d->breachChance < 10 && rand() % 100 < 15) hullLoss = 0;
    s->hullHp -= hullLoss;
    if (side == SIDE_ENEMY && hullLoss > 0 && sDmgSrc > SRC_NONE && sDmgSrc != SRC_HAZARD) {
        gFeats.fight.enemyHull[sDmgSrc] += hullLoss;
        gFeats.fight.lastEnemyHullSrc = sDmgSrc;
    }
    // Crystal Vengeance: one hull hit in ten throws a shard back.
    if (hullLoss > 0 && s->aug[AUG_CRYSTAL_SHARDS] && rand() % 10 == 0) shard_shot(c, side);
    int sysDamage = d->sysDamage + hd;
    // Titanium System Casing: 15% to keep the system undamaged (not from flares).
    if (si >= 0 && sysDamage > 0 && s->aug[AUG_SYSTEM_CASING] && !flare && rand() % 100 < 15) sysDamage = 0;
    if (si >= 0 && sysDamage > 0) {
        ShipSystem* y = &s->systems[si];
        y->damage += sysDamage;
        if (y->damage > y->def->level) y->damage = y->def->level;
        y->repair = 0.0f;
        if (y->def->type == SYS_WEAPONS) weapons_sync(s);
    }
    apply_ion(s, si, d->ion);
    if (d->lockdown) combat_lockdown(c, side, room);
    float crewDamage = CREW_DAMAGE * (d->persDamage + hd);
    float stun = d->stun > 0 ? d->stun : (rand() % 10 < d->stunChance ? 3.0f : 0.0f);
    for (int i = 0; stun > 0.0f && i < cs->count; i++)
        if (ship_room_at(s, (int)floorf(cs->crew[i].x / TILE), (int)floorf(cs->crew[i].y / TILE)) == room &&
            cs->crew[i].stun < stun)
            cs->crew[i].stun = stun;
    for (int i = 0; crewToo && i < cs->count; i++) {
        Crew* m = &cs->crew[i];
        // Negative crew damage heals (Healing Burst), and only the attacker's own crew aboard.
        if (crewDamage < 0.0f && !m->foreign) continue;
        if (ship_room_at(s, (int)floorf(m->x / TILE), (int)floorf(m->y / TILE)) == room) {
            m->health -= m->drone ? crewDamage * 0.5f : crewDamage;
            m->lastHit = side == SIDE_ENEMY && sDmgSrc != SRC_HAZARD && sDmgSrc != SRC_NONE ? HIT_WEAPON : HIT_NONE;
            m->lastHitBio = false;
        }
    }
    if (rand() % 100 < d->fireChance * 10) {
        life_ignite(s, room);  // the game rolls two fires, which can land on the same tile
        if (rand() % 2) life_ignite(s, room);
    } else if (rand() % 100 < d->breachChance * 10) {
        life_breach(s, room);
    }
    // Deaths are handled by life_update.
}

static void resolve(Combat* c, Projectile* p) {
    int side = !p->from;  // the ship being hit
    Ship* s = c->ship[side];
    const WeaponDef* d = combat_shot_def(c, p->from, p->weapon, p->drone);
    sDmgSrc = p->from != SIDE_PLAYER          ? SRC_NONE
              : p->weapon == SHOT_SHARD        ? SRC_SHARD
              : SHOT_IS_ART(p->weapon)         ? SRC_ARTILLERY
              : p->drone >= 0                  ? SRC_DRONE
              : p->weapon < 0                  ? SRC_HAZARD
              : d->type == WEAPON_MISSILES     ? SRC_MISSILE
                                               : SRC_WEAPON;
    if (side == SIDE_PLAYER && p->weapon != SHOT_ASTEROID && p->weapon != SHOT_PDS && !p->shotDown) {
        // Dodged while cloaked (Phase Shift); hits in a row with the engines maxed (Astronomically Low Odds).
        if (p->miss && s->cloakTime > 0.0f) gFeats.fight.cloakAvoided += d->damage;
        int eng = ship_find_system(s, SYS_ENGINES);
        bool maxed = eng >= 0 && s->systems[eng].def->level >= 8 && ship_system_power(s, eng) >= 8;
        gFeats.fight.hitStreak = p->miss || !maxed ? 0 : gFeats.fight.hitStreak + 1;
    }
    if (!p->shotDown && p->evade >= 0) {
        // Evasion now against when it was rolled: flip with the probability that makes the overall
        // chance of a miss the evasion at arrival.
        int now = evasion(c, s);
        if (now > 100) now = 100;
        float r = rand() / (float)RAND_MAX;
        if (!p->miss && now > p->evade && r < (now - p->evade) / (float)(100 - p->evade)) p->miss = true;
        else if (p->miss && now < p->evade && r < (p->evade - now) / (float)p->evade) p->miss = false;
    }
    if (p->shotDown) {  // a defence drone got it
        add_effect(c, side, FX_EXPLOSION, p->tx, p->ty);
        return;
    }
    if (p->miss) {
        add_effect(c, side, FX_MISS, p->tx, p->ty);
        sound_play("miss");
        if (s->cloakTime <= 0.0f) {  // dodged: piloting and engines learn (not while cloaked)
            manning_xp(c, side, SYS_PILOT, SKILL_PILOT);
            manning_xp(c, side, SYS_ENGINES, SKILL_ENGINES);
        }
        return;
    }
    if (p->weapon == SHOT_PDS) {
        // Through any shield, Zoltan's included; always a breach (wiki: Anti-Ship Battery).
        damage_room(c, side, p->room, d->damage, d, true);
        life_breach(s, p->room);
        add_hit(c, side, p->tx, p->ty, d);
        play_random(d->hitShip, d->hitShipCount);
        return;
    }
    if (d->type != WEAPON_BOMB && shield_drone_absorbs(c, side)) {
        c->shieldFlash[side] = 0.4f;
        add_effect(c, side, FX_SHIELD, p->tx, p->ty);
        play_random(d->hitShield, d->hitShieldCount);
        return;
    }
    bool bypass = d->type == WEAPON_BOMB && p->weapon >= 0 && c->ship[p->from]->aug[AUG_ZOLTAN_BYPASS];
    if (s->superShield > 0 && !bypass) {
        // Zoltan shield: absorbs the shot; ion does double (wiki, xftl Ship.attackShields).
        int amount = (d->type == WEAPON_BOMB ? d->sysDamage : (d->damage > 0 ? d->damage : 0)) + d->ion * 2;
        if (amount > 0 || d->type != WEAPON_BOMB) {
            s->superShield -= amount;
            if (s->superShield < 0) s->superShield = 0;
            c->shieldFlash[side] = 0.4f;
            add_effect(c, side, FX_SHIELD, p->tx, p->ty);
            play_random(d->hitShield, d->hitShieldCount);
        }
        return;  // fire, lockdown and healing bombs fizzle on it
    }
    if (d->type == WEAPON_BOMB) {
        // Bombs teleport past shields; their damage is all system/crew/ion (damage is 0).
        damage_room(c, side, p->room, d->damage, d, true);
        add_hit(c, side, p->tx, p->ty, d);
        play_random(d->hitShip, d->hitShipCount);
        return;
    }
    int shieldSys = ship_find_system(s, SYS_SHIELDS);
    if (s->shieldLayers > 0) {
        if (d->sp >= s->shieldLayers) {
            // Pierced: ion weapons still ionise the shields on the way through.
            apply_ion(s, shieldSys, d->ion);
        } else {
            // Blocked. Only shots with hull damage pop a layer; ion locks the shields instead.
            if (d->damage > 0) {
                s->shieldLayers--;
                s->shieldCharge = 0.0f;
                c->shieldFlash[side] = 0.4f;
                if (d->ion <= 0) manning_xp(c, side, SYS_SHIELDS, SKILL_SHIELDS);
            }
            apply_ion(s, shieldSys, d->ion);
            add_effect(c, side, FX_SHIELD, p->tx, p->ty);
            play_random(d->hitShield, d->hitShieldCount);
            return;
        }
    }
    if (p->outside) {  // flak that landed between rooms
        add_effect(c, side, FX_MISS, p->tx, p->ty);
        return;
    }
    damage_room(c, side, p->room, d->damage, d, true);
    add_hit(c, side, p->tx, p->ty, d);
    play_random(d->hitShip, d->hitShipCount);
}

// Beams damage each room they enter (less one per shield layer), and crew on each tile they cross.
static void update_beams(Combat* c, float dt) {
    for (int i = 0; i < MAX_BEAMS; i++) {
        Beam* b = &c->beams[i];
        if (!b->active) continue;
        if (b->delay > 0.0f) {
            b->delay -= dt;
            continue;
        }
        b->t += dt / b->duration;
        int side = !b->from;
        Ship* s = c->ship[side];
        const WeaponDef* d = combat_shot_def(c, b->from, b->weapon, b->drone);
        if (s->superShield > 0) {
            int ticks = (b->t >= 0.33f) + (b->t >= 0.8f);  // xftl doc/weapons: at 33% and 80% of the sweep
            while (b->superTicks < ticks && s->superShield > 0) {
                b->superTicks++;
                s->superShield -= (d->damage > 1 ? d->damage : 1) + d->ion * 2;
                if (s->superShield < 0) s->superShield = 0;
                c->shieldFlash[side] = 0.4f;
            }
            if (b->t >= 1.2f) b->active = false;
            continue;
        }
        int over = s->shieldLayers - d->sp;
        bool pierces = s->shieldLayers == 0 || d->damage + d->sp > s->shieldLayers;
        int dmg = !pierces ? 0 : d->damage - (over > 0 ? over : 0);
        float k = b->t > 1.0f ? 1.0f : b->t;
        float x = b->sx + (b->ex - b->sx) * k, y = b->sy + (b->ey - b->sy) * k;
        int tx = (int)floorf(x / TILE) - s->xOffset, ty = (int)floorf(y / TILE) - s->yOffset;
        int room = ship_room_at(s, tx, ty);
        // A beam that gets through acts even at 0 damage: the Fire Beam's fires and the Anti-Bio
        // Beam's crew damage (0-damage beams only get through with the shields down).
        if (room >= 0 && pierces) {
            if (!(b->hitRooms & (1ull << room))) {
                b->hitRooms |= 1ull << room;
                sDmgSrc = b->from != SIDE_PLAYER ? SRC_NONE : b->drone >= 0 ? SRC_DRONE : SHOT_IS_ART(b->weapon) ? SRC_ARTILLERY : SRC_WEAPON;
                damage_room(c, side, room, dmg, d, false);  // crew are hit per tile, below
                if (b->from == SIDE_PLAYER && room < 64) {  // Slice and Dice: every room inside 5 s
                    gFeats.fight.roomBeamTime[room] = gFeats.fight.clock + 1.0f;
                    bool all = true;
                    for (int r = 0; r < s->roomCount && r < 64 && all; r++)
                        all = gFeats.fight.roomBeamTime[r] > 0.0f && gFeats.fight.clock + 1.0f - gFeats.fight.roomBeamTime[r] <= 5.0f;
                    if (all) gFeats.fight.allRoomsBeamed = true;
                }
                add_hit(c, side, x, y, d);
                play_random(d->hitShip, d->hitShipCount);
            }
            int bit = ty * GRID_MAX + tx;
            if (!(b->hitTiles[bit >> 3] & (1 << (bit & 7)))) {
                b->hitTiles[bit >> 3] |= 1 << (bit & 7);
                CrewSet* cs = c->crew[side];
                for (int m = 0; m < cs->count; m++)
                    if ((int)floorf(cs->crew[m].x / TILE) == tx && (int)floorf(cs->crew[m].y / TILE) == ty) {
                        cs->crew[m].health -= CREW_DAMAGE * (dmg + d->persDamage);
                        cs->crew[m].lastHit = b->from == SIDE_PLAYER ? HIT_WEAPON : HIT_NONE;
                        cs->crew[m].lastHitBio = b->from == SIDE_PLAYER && !strcmp(d->name, "BEAM_BIO");
                        cs->crew[m].lastHitSerial = b->serial;
                    }
            }
        }
        if (b->t >= 1.2f) b->active = false;  // linger a moment at the end of the sweep
    }
}

// Cloak and battery timers, ion wearing off, and power returning to systems. Needs no enemy.
static void update_systems(Combat* c, int side, float dt) {
    Ship* s = c->ship[side];
    for (int r = 0; r < s->roomCount; r++)
        if (s->lockdown[r] > 0.0f) s->lockdown[r] -= dt;
    int ci = ship_find_system(s, SYS_CLOAKING);
    if (s->cloakTime > 0.0f) {
        float full = (ci >= 0 ? s->systems[ci].power : 0) * CLOAK_PER_POWER;
        s->cloakTime -= dt;
        if (s->cloakTime > full) s->cloakTime = full;  // damage cuts it short
        if (s->cloakTime <= 0.0f) {
            s->cloakTime = 0.0f;
            if (ci >= 0) s->systems[ci].ionTimer += SYSTEM_RECHARGE;  // locked while it recharges
            sound_play("decloak");
        }
    }
    int bi = ship_find_system(s, SYS_BATTERY);
    if (s->batteryTime > 0.0f) {
        s->batteryTime -= dt;
        s->batteryBonus = bi >= 0 ? ship_system_capacity(s, bi) * 2 : 0;
        if (s->batteryTime <= 0.0f) {
            s->batteryTime = 0.0f;
            s->batteryBonus = 0;
            if (bi >= 0) s->systems[bi].ionTimer += s->aug[AUG_BATTERY] ? SYSTEM_RECHARGE * 0.5f : SYSTEM_RECHARGE;
            sound_play("batteryStop");
        }
    }
    // Without the battery's bars, switch things off from the end until the reactor copes.
    for (int i = s->weaponCount - 1; i >= 0 && ship_reactor_free(s) < 0; i--) s->weapons[i].powered = false;
    for (int i = s->systemCount - 1; i >= 0 && ship_reactor_free(s) < 0; i--) {
        ShipSystem* y = &s->systems[i];
        if (system_is_subsystem(y->def->type) || y->def->type == SYS_WEAPONS) continue;
        while (y->power > 0 && ship_reactor_free(s) < 0) y->power--;
    }

    for (int i = 0; i < s->systemCount; i++) {
        ShipSystem* y = &s->systems[i];
        if (y->ionTimer > 0.0f) {
            y->ionTimer -= dt;
            if (y->ionTimer <= 0.0f) {
                y->ionTimer = 0.0f;
                y->ionLimit = -1;
            }
        }
    }
    weapons_sync(s);
    for (int i = 0; i < s->systemCount; i++) {
        ShipSystem* y = &s->systems[i];
        int cap = ship_system_capacity(s, i);
        if (system_is_subsystem(y->def->type)) {
            y->power = cap;
        } else if (y->def->type != SYS_WEAPONS) {
            if (y->power > cap) y->power = cap;
            // After repairs or when ion wears off, the game puts back the power you'd set.
            int want = y->targetPower < cap ? y->targetPower : cap;
            if (y->power < want) {
                int add = want - y->power, free = ship_reactor_free(s);
                y->power += add < free ? add : free;
            }
        }
    }
    // Weapons the player left switched on come back on the same way.
    for (int i = 0; i < s->weaponCount; i++)
        if (s->weapons[i].wanted && !s->weapons[i].powered) power_weapon(s, i);
}

// Shield layers recharge one at a time, in or out of a fight.
static void update_shields(Combat* c, int side, float dt) {
    Ship* s = c->ship[side];
    int sh = ship_find_system(s, SYS_SHIELDS), maxLayers = combat_max_shields(s);
    if (s->shieldLayers > maxLayers) s->shieldLayers = maxLayers;
    if (s->shieldLayers < maxLayers && !(s->hackPulse > 0.0f && s->hackedSys == sh)) {
        int sl = man_level(c, s, sh);
        s->shieldCharge += dt * (sl >= 0 ? SHIELDS_MANNED_RATE[sl] : 1.0f) * (1.0f + 0.15f * s->aug[AUG_SHIELD_BOOST]);
        if (s->shieldCharge >= SHIELD_LAYER_TIME[s->shieldLayers < 5 ? s->shieldLayers : 4]) {
            s->shieldLayers++;
            s->shieldCharge = 0.0f;
            if (side == SIDE_PLAYER) sound_play("shieldsUp");
        }
    } else {
        s->shieldCharge = 0.0f;
    }
}

static void update_ship(Combat* c, int side, float dt) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    int ci = ship_find_system(s, SYS_CLOAKING);
    update_systems(c, side, dt);
    update_shields(c, side, dt);

    // Artillery charges by itself (no console to man) and fires unaimed: a beam sweeps a random
    // room, anything else sends its volley at one.
    for (int k = 0; k < s->artilleryCount; k++) {
        WeaponSlot* aw = &s->artillery[k];
        const WeaponDef* ad = aw->def;
        aw->fireAnim += dt;  // the gun's firing frames on its mount
        if (aw->burstLeft > 0) {
            aw->burstTimer -= dt;
            if (aw->burstTimer <= 0.0f) {
                fire_artillery_shot(c, side, k);
                aw->burstLeft--;
                aw->burstTimer = SHOT_SPACING;
            }
            continue;
        }
        float full = ship_artillery_time(s, k);
        if (full <= 0.0f) {
            aw->charge -= UNPOWERED_DRAIN * dt;
            if (aw->charge < 0.0f) aw->charge = 0.0f;
            continue;
        }
        if (o->cloakTime > 0.0f || c->result != COMBAT_FIGHTING || o->hullHp <= 0 || o->roomCount == 0) continue;
        aw->charge += dt;
        if (aw->charge < full) continue;
        aw->charge = 0.0f;
        aw->fireAnim = 0.0f;
        if (side == SIDE_PLAYER) gFeats.shotsFired++;
        else gFeats.fight.enemyShots++;
        play_random(ad->launch, ad->launchCount);
        manning_xp(c, side, SYS_WEAPONS, SKILL_WEAPONS);  // wiki: artillery trains Weapons too
        if (ad->type != WEAPON_BEAM) {
            aw->target = rand() % o->roomCount;
            aw->burstLeft = ad->shots > 0 ? ad->shots : 1;
            aw->burstTimer = 0.0f;
            continue;
        }
        for (int b = 0; b < MAX_BEAMS; b++) {
            Beam* be = &c->beams[b];
            if (be->active) continue;
            memset(be, 0, sizeof(*be));
            be->active = true;
            be->from = side;
            be->weapon = SHOT_ART(k);
            be->drone = -1;
            const Room* r = &o->rooms[rand() % o->roomCount];
            be->sx = (o->xOffset + r->x + r->w * 0.5f) * TILE;
            be->sy = (o->yOffset + r->y + r->h * 0.5f) * TILE;
            float a = (rand() % 8) * (float)M_PI / 4;
            be->ex = be->sx + cosf(a) * ad->length;
            be->ey = be->sy + sinf(a) * ad->length;
            be->delay = 0.24f;
            be->duration = 1.0f;
            break;
        }
    }

    // Weapons charge while powered and fire at their target once charged.
    int ws = weapons_system(s);
    int wl = man_level(c, s, ws);
    for (int i = 0; i < s->weaponCount; i++) {
        WeaponSlot* w = &s->weapons[i];
        w->fireAnim += dt;
        if (w->burstLeft > 0) {
            w->burstTimer -= dt;
            if (w->burstTimer <= 0.0f) {
                fire_shot(c, side, i);
                w->burstLeft--;
                w->burstTimer = SHOT_SPACING;
                if (w->burstLeft == 0 && side == SIDE_PLAYER && !c->autofire) {
                    w->target = -1;
                    w->beamAimed = false;
                }
            }
            continue;
        }
        float cd = w->def->cooldown - w->chain * w->def->boostAmount;
        if (cd < 1.0f) cd = 1.0f;
        w->fullCharge = cd * w->def->chargeLevels;
        if (!w->powered) {
            w->charge -= UNPOWERED_DRAIN * dt;
            if (w->charge < 0.0f) w->charge = 0.0f;
            w->chain = 0;
            continue;
        }
        bool hacked = s->hackPulse > 0.0f && s->hackedSys == ws;
        if (hacked) {
            w->charge -= dt;  // hacking reverse-charges weapons
            if (w->charge < 0.0f) w->charge = 0.0f;
            continue;
        }
        if (o->cloakTime <= 0.0f) w->charge += dt / (wl >= 0 ? WEAPONS_MANNED_TIME[wl] : 1.0f) * (1.0f + 0.1f * s->aug[AUG_RELOADER]);
        if (w->charge > w->fullCharge) w->charge = w->fullCharge;
        bool ready = w->charge >= w->fullCharge && w->target >= 0 && o->hullHp > 0 && c->result == COMBAT_FIGHTING &&
                     o->cloakTime <= 0.0f;
        if (ready && w->def->missiles > 0 && s->missiles < w->def->missiles) ready = false;
        if (ready) {
            if (s->cloakTime > 0.0f && w->def->type != WEAPON_BEAM && ci >= 0 && !s->aug[AUG_STEALTH_WEAPONS])
                s->cloakTime -= CLOAK_FIRE_PENALTY * s->systems[ci].power * CLOAK_PER_POWER;
            if (!(s->aug[AUG_EXPLOSIVE] && rand() % 2)) s->missiles -= w->def->missiles;
            if (side == SIDE_PLAYER) {
                gFeats.shotsFired++;
                if (w->def->missiles > 0 || w->def->type == WEAPON_BOMB) gFeats.missileShots++;
                if (i < 4) gFeats.fight.weaponFires[i]++;
            } else {
                gFeats.fight.enemyShots++;
            }
            w->charge = 0.0f;
            manning_xp(c, side, SYS_WEAPONS, SKILL_WEAPONS);  // a volley counts once
            w->burstLeft = w->def->shots * w->def->chargeLevels;
            w->burstTimer = 0.0f;
            if (w->chain < w->def->boostCount) w->chain++;
        }
    }
}

// Crew standing still in a room with a damaged system fix it.
// The tile in `room` closest to (tx, ty) whose value in grid is above zero, or false.
static bool nearest_in_room(const Ship* s, int room, const float grid[GRID_MAX][GRID_MAX], int tx, int ty, int* ox, int* oy) {
    const Room* r = &s->rooms[room];
    int best = 1 << 30;
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++) {
            int d = abs(x - tx) + abs(y - ty);
            if (grid[y][x] > 0.0f && d < best) {
                best = d;
                *ox = x;
                *oy = y;
            }
        }
    return best < (1 << 30);
}

// Crew standing in a room fight its fires first, then seal its breaches, then repair its system.
static void update_repairs(Combat* c, int side, float dt) {
    Ship* s = c->ship[side];
    CrewSet* cs = c->crew[side];
    for (int i = 0; i < cs->count; i++) {
        Crew* m = &cs->crew[i];
        m->repairing = false;
        m->fighting = false;
        if (m->pathPos < m->pathLen || m->foreign || m->brawling || m->stun > 0.0f || m->dormant) continue;
        if (m->drone && strcmp(cs->races[m->race].name, "repair")) continue;  // only repair drones repair
        int room = ship_room_at(s, m->destX, m->destY);
        if (room < 0) continue;
        const char* race = cs->races[m->race].name;
        float rate = !strcmp(race, "engi") || !strcmp(race, "repair") ? 2.0f : !strcmp(race, "mantis") ? 0.5f : 1.0f;
        rate *= REPAIR_SKILL_RATE[crew_skill_level(cs, m, SKILL_REPAIR)];
        int fx, fy;
        if (nearest_in_room(s, room, s->fire, m->destX, m->destY, &fx, &fy)) {
            // Rock crew have a 2.0 fire multiplier, Crystals 1.0, everyone else 1.2 (xftl doc/fires).
            float fireMult = !strcmp(race, "rock") ? 2.0f : !strcmp(race, "crystal") ? 1.0f : 1.2f;
            m->fighting = true;
            m->dir = fx > m->destX ? DIR_RIGHT : fx < m->destX ? DIR_LEFT : fy < m->destY ? DIR_UP : DIR_DOWN;
            s->fire[fy][fx] -= FIRE_FIGHT_RATE * fireMult * rate * dt;
            if (s->fire[fy][fx] <= 0.0f) {
                s->fire[fy][fx] = 0.0f;
                s->fireBurnout[fy][fx] = 0.0f;
            }
            continue;
        }
        if (nearest_in_room(s, room, s->breach, m->destX, m->destY, &fx, &fy)) {
            m->repairing = true;
            s->breach[fy][fx] -= 100.0f / REPAIR_SECONDS * rate * dt;
            if (s->breach[fy][fx] < 0.0f) s->breach[fy][fx] = 0.0f;
            continue;
        }
        int si = ship_system_in_room(s, room);
        if (si < 0 || s->systems[si].damage <= 0) continue;
        ShipSystem* y = &s->systems[si];
        m->repairing = true;
        y->repair += dt * rate * (s->hackedSys == si ? 0.5f : 1.0f) / REPAIR_SECONDS;
        if (y->repair >= 1.0f) {
            y->repair = 0.0f;
            y->damage--;
            if (side == SIDE_PLAYER) crew_add_xp(cs, m, SKILL_REPAIR);  // whoever finishes the bar
            if (system_is_subsystem(y->def->type)) y->power = ship_system_capacity(s, si);
        }
    }
    // Fire Suppression: every fire at once, as fast as a Crystal; Slug Repair Gel: every breach at
    // once, at 75% of a crew member's speed (wiki: Augmentations).
    if (s->aug[AUG_FIRE_SUPPRESS] || s->aug[AUG_SLUG_GEL])
        for (int y = 0; y < GRID_MAX; y++)
            for (int x = 0; x < GRID_MAX; x++) {
                if (s->aug[AUG_FIRE_SUPPRESS] && s->fire[y][x] > 0.0f) {
                    s->fire[y][x] -= FIRE_FIGHT_RATE * dt;
                    if (s->fire[y][x] <= 0.0f) s->fire[y][x] = s->fireBurnout[y][x] = 0.0f;
                }
                if (s->aug[AUG_SLUG_GEL] && s->breach[y][x] > 0.0f) {
                    s->breach[y][x] -= 100.0f / REPAIR_SECONDS * 0.75f * dt;
                    if (s->breach[y][x] < 0.0f) s->breach[y][x] = 0.0f;
                }
            }
}

// The flagship's AI repairs every damaged system at once, a bar per AI_REPAIR_SECONDS (the wiki
// says "a set rate"; this rate is our choice), except where there's a fire or a breach.
#define AI_REPAIR_SECONDS 20.0f

static void update_ai_repairs(Ship* s, float dt) {
    for (int i = 0; i < s->systemCount; i++) {
        ShipSystem* y = &s->systems[i];
        if (y->damage <= 0 || life_room_fires(s, y->room) > 0 || life_room_breaches(s, y->room) > 0) continue;
        y->repair += dt / AI_REPAIR_SECONDS;
        if (y->repair >= 1.0f) {
            y->repair = 0.0f;
            y->damage--;
            if (system_is_subsystem(y->def->type)) y->power = ship_system_capacity(s, i);
        }
    }
}

// Which of our rooms the enemy shoots at (xftl doc/combat-ai, CombatAI::PrioritizeSystem): on easy
// and normal a system 20% / 33% of the time, else any room. On hard, half the shots go to any room,
// a quarter to any system and a quarter to a high-priority one: powered shields, engines and
// piloting while we dodge over 25%, oxygen below 50%, weapons and drones if any are on, cloaking
// when it's ready, a charging artillery, the clone bay while cloning, the doors with intruders or
// fires aboard, the teleporter with our crew on their ship, an active battery.
static int ai_target_room(const Combat* c) {
    const Ship* p = c->ship[SIDE_PLAYER];
    if (p->systemCount == 0 || p->roomCount == 0) return p->roomCount ? rand() % p->roomCount : 0;
    if (c->difficulty != DIFF_HARD) {
        if (rand() % 100 < (c->difficulty == DIFF_EASY ? 20 : 33)) return p->systems[rand() % p->systemCount].room;
        return rand() % p->roomCount;
    }
    int roll = rand() % 4;
    if (roll < 2) return rand() % p->roomCount;
    int high[MAX_SYSTEMS], n = 0;
    if (roll == 3) {
        bool intruders = crew_foreign_count(c->crew[SIDE_PLAYER]) > 0, fires = life_ship_fires(p) > 0;
        bool ourCrewThere = c->crew[SIDE_ENEMY] && crew_foreign_count(c->crew[SIDE_ENEMY]) > 0;
        for (int i = 0; i < p->systemCount; i++) {
            const ShipSystem* y = &p->systems[i];
            bool pick = false;
            switch (y->def->type) {
                case SYS_SHIELDS: pick = y->power > 0; break;
                case SYS_ENGINES:
                case SYS_PILOT: pick = combat_evasion(p) > 25; break;
                case SYS_OXYGEN: pick = life_ship_oxygen(p) < 50.0f; break;
                case SYS_WEAPONS:
                    for (int k = 0; k < p->weaponCount; k++) pick |= p->weapons[k].powered;
                    break;
                case SYS_DRONES:
                    for (int k = 0; k < p->droneCount; k++) pick |= p->drones[k].powered;
                    break;
                case SYS_CLOAKING: pick = p->cloakTime <= 0.0f && y->ionTimer <= 0.0f; break;
                case SYS_ARTILLERY:
                    for (int k = 0; k < p->artilleryCount; k++) pick |= p->artSys[k] == i && p->artillery[k].charge < 4.0f;
                    break;
                case SYS_CLONEBAY: pick = c->crew[SIDE_PLAYER]->cloneCount > 0; break;
                case SYS_DOORS: pick = intruders || fires; break;
                case SYS_TELEPORTER: pick = ourCrewThere; break;
                case SYS_BATTERY: pick = p->batteryTime > 0.0f; break;
                default: break;
            }
            if (pick) high[n++] = i;
        }
    }
    int si = n ? high[rand() % n] : rand() % p->systemCount;
    return p->systems[si].room;
}

// The enemy keeps its weapons powered, picks a new player room for each volley, and sends a
// crew member to fix damaged systems.
static void enemy_ai(Combat* c, float dt) {
    Ship* e = c->ship[SIDE_ENEMY];
    CrewSet* cs = c->crew[SIDE_ENEMY];
    for (int i = 0; i < e->weaponCount; i++) {
        WeaponSlot* w = &e->weapons[i];
        if (w->target < 0 || (w->burstLeft == 0 && w->charge < 0.5f)) {
            int room = ai_target_room(c);
            w->target = room;
        }
    }

    c->aiTimer -= dt;
    if (c->aiTimer > 0.0f) return;
    c->aiTimer = 1.0f;
    combat_cloak(c, SIDE_ENEMY);
    combat_battery(c, SIDE_ENEMY);
    for (int i = 0; i < e->weaponCount; i++)
        if (c->enemyHoldFire) {
            e->weapons[i].powered = e->weapons[i].wanted = false;
            weapons_sync(e);
        } else if (!e->weapons[i].powered && !e->weapons[i].wanted) {
            combat_toggle_weapon(e, i);
        }
    for (int i = 0; i < e->droneCount; i++)
        if (!e->drones[i].powered && !e->drones[i].wanted) combat_toggle_drone(c, SIDE_ENEMY, i);
    for (int si = 0; si < e->systemCount; si++) {
        ShipSystem* y = &e->systems[si];
        if (!system_is_subsystem(y->def->type) && y->def->type != SYS_WEAPONS && y->power < ship_system_capacity(e, si))
            ship_set_power(e, si, ship_system_capacity(e, si));
    }
    for (int room = 0; room < e->roomCount; room++) {
        int si = ship_system_in_room(e, room);
        bool work = life_room_fires(e, room) > 0 || life_room_breaches(e, room) > 0 || (si >= 0 && e->systems[si].damage > 0);
        if (!work) continue;
        bool covered = false;
        for (int k = 0; k < cs->count && !covered; k++)
            covered = !cs->crew[k].foreign && ship_room_at(e, cs->crew[k].destX, cs->crew[k].destY) == room;
        if (!covered && crew_native_count(cs) > 0) {
            int k;
            do k = rand() % cs->count;
            while (cs->crew[k].foreign || cs->crew[k].drone);
            crew_order(cs, e, k, room);
        }
    }
}


// ---- Drones ------------------------------------------------------------------------
// Behaviour from xftl (drones/*.kt, doc/combat-drone) and the FTL wiki (Drone Control):
// combat drones fly between points just outside the enemy's shield ellipse, stop, wait 0.5 s and
// fire at a random room; defence drones orbit their own ship and shoot down incoming shots on
// their cooldown; hull repair drones fix 1 hull per stop, 3-5 times; shield drones absorb one shot
// per 15 s. Deploying costs a drone part; a destroyed drone can't be redeployed for 10 s.

#define DRONE_PAUSE 0.5f
#define DRONE_REDEPLOY 10.0f
#define DRONE_BEAM_TIME 0.5f
#define DRONE_SHOT_PX 14.0f  // ship pixels per second per unit of weapon speed, as on screen
#define BOARDER_FLIGHT 2.5f  // our choice: seconds for a boarding drone to cross to the enemy

static float frand(float lo, float hi);
static bool is_boarder(const DroneDef* d) { return d->type == DRONE_BOARDER || d->type == DRONE_ION_INTRUDER; }

static bool is_crew_drone(const DroneDef* d) {
    return d->type == DRONE_CREW_REPAIR || d->type == DRONE_BATTLE || is_boarder(d);
}

#define ION_PULSE_MIN 8.2f  // wiki (Drone Control): an ion intruder pulses every 8.2-10 s ...
#define ION_PULSE_SPREAD 1.8f
#define ION_INTRUDER_ION 3   // ... for 3 ion damage to the room's system ...
#define ION_INTRUDER_STUN 6.0f  // ... and a 6 s stun for the crew there

static bool is_intruder(const CrewSet* cs, const Crew* m) { return m->drone && !strcmp(cs->races[m->race].name, "boarder_ion"); }

// Puts a crew-like drone on a ship: repair and battle drones in our drone room, boarders in an
// enemy system room (breaching the hull there).
static bool spawn_crew_drone(Combat* c, int side, int slot) {
    const DroneDef* d = c->ship[side]->drones[slot].def;
    bool boarder = is_boarder(d);
    Ship* s = c->ship[boarder ? !side : side];
    CrewSet* cs = c->crew[boarder ? !side : side];
    if (!s || cs->count >= MAX_CREW) return false;
    int room = -1;
    if (boarder) {
        room = s->systemCount ? s->systems[rand() % s->systemCount].room : rand() % s->roomCount;
    } else {
        int ds = ship_find_system(s, SYS_DRONES);
        room = ds >= 0 ? s->systems[ds].room : 0;
    }
    int tiles[8][2], n = free_tiles(s, cs, room, tiles, 1, boarder);
    for (int r = 0; n == 0 && r < s->roomCount; r++) n = free_tiles(s, cs, r, tiles, 1, boarder);
    int race = crew_race(cs, d->type == DRONE_CREW_REPAIR ? "repair" : d->type == DRONE_ION_INTRUDER ? "boarder_ion" : "battle");
    if (!n || race < 0 || !crew_add(cs, race, d->shortName, false, tiles[0][0], tiles[0][1])) return false;
    Crew* m = &cs->crew[cs->count - 1];
    m->drone = true;
    m->droneSlot = slot;
    m->foreign = boarder;
    if (d->type == DRONE_ION_INTRUDER) m->brawlTimer = ION_PULSE_MIN + frand(0.0f, ION_PULSE_SPREAD);
    if (boarder) life_breach(s, room);
    return true;
}

// The crew-like drone for a slot, wherever it is, or NULL.
static Crew* crew_drone(Combat* c, int side, int slot) {
    for (int k = 0; k < 2; k++) {
        CrewSet* cs = c->crew[k];
        for (int i = 0; i < cs->count; i++)
            if (cs->crew[i].drone && cs->crew[i].droneSlot == slot && cs->crew[i].foreign == (k != side)) return &cs->crew[i];
    }
    return NULL;
}

static int drones_system(const Ship* s) { return ship_find_system(s, SYS_DRONES); }

static int drones_used(const Ship* s) {
    int used = 0;
    for (int i = 0; i < s->droneCount; i++)
        if (s->drones[i].powered) used += s->drones[i].def->power;
    return used;
}

static void drones_sync(Ship* s) {
    int ds = drones_system(s);
    if (ds < 0) return;
    int cap = ship_system_capacity(s, ds);
    for (int i = s->droneCount - 1; i >= 0 && drones_used(s) > cap; i--) s->drones[i].powered = false;
    s->systems[ds].power = drones_used(s);
}

// The shield ellipse of a ship, in its own coordinates (X/Y_OFFSET included).
void combat_shield_ellipse(const Ship* s, float* cx, float* cy, float* rw, float* rh) {
    float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    *cx = ox + s->hullX + s->hull.width * 0.5f;
    *cy = oy + s->hullY + s->hull.height * 0.5f;
    if (!s->bp->enemy && s->roomCount > 0) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < s->roomCount; i++) {
            const Room* r = &s->rooms[i];
            if (r->x * TILE < x0) x0 = r->x * TILE;
            if (r->y * TILE < y0) y0 = r->y * TILE;
            if ((r->x + r->w) * TILE > x1) x1 = (r->x + r->w) * TILE;
            if ((r->y + r->h) * TILE > y1) y1 = (r->y + r->h) * TILE;
        }
        *cx = ox + (x0 + x1) * 0.5f + s->ellipse[2];
        *cy = oy + (y0 + y1) * 0.5f + s->ellipse[3];
    }
    *rw = s->ellipse[0] > 0 ? s->ellipse[0] : s->hull.width * 0.55f;
    *rh = s->ellipse[1] > 0 ? s->ellipse[1] : s->hull.height * 0.55f;
}

static float drone_speed(const Ship* space, const DroneDef* d) {
    float cx, cy, rw, rh;
    combat_shield_ellipse(space, &cx, &cy, &rw, &rh);
    return (rw > rh ? rw : rh) * d->speed / 21.875f;
}

// A new stopping point at least 90 degrees round the ellipse from the last, 15% outside it.
static void pick_destination(DroneEntity* e, const Ship* space) {
    float cx, cy, rw, rh, a;
    combat_shield_ellipse(space, &cx, &cy, &rw, &rh);
    do {
        a = (rand() % 6283) / 1000.0f;
    } while (fabsf(a - e->angle) < (float)M_PI_2 || fabsf(a - e->angle) > 3 * (float)M_PI_2);
    e->angle = a;
    e->dx = cx + cosf(a) * rw * 1.15f;
    e->dy = cy + sinf(a) * rh * 1.15f;
}

static DroneEntity* spawn_drone(Combat* c, int side, int slot) {
    Ship* s = c->ship[side];
    const DroneDef* d = s->drones[slot].def;
    for (int i = 0; i < MAX_DRONE_ENTITIES; i++) {
        DroneEntity* e = &c->droneEnts[i];
        if (e->active) continue;
        memset(e, 0, sizeof(*e));
        e->active = true;
        e->side = side;
        e->slot = slot;
        // Combat drones work around the enemy; everything else stays home.
        e->space = d->type == DRONE_COMBAT ? !side : side;
        const Ship* sp = c->ship[e->space];
        e->angle = (rand() % 6283) / 1000.0f;
        float cx, cy, rw, rh;
        combat_shield_ellipse(sp, &cx, &cy, &rw, &rh);
        e->x = cx + cosf(e->angle) * rw * 1.15f;
        e->y = cy + sinf(e->angle) * rh * 1.15f;
        pick_destination(e, sp);
        e->repairsLeft = 3 + rand() % 3;
        e->cooldown = d->type == DRONE_SHIELD ? d->cooldown : 0.0f;
        return e;
    }
    return NULL;
}

static bool power_drone(Combat* c, int side, int i) {
    Ship* s = c->ship[side];
    DroneSlot* d = &s->drones[i];
    int ds = drones_system(s);
    if (ds < 0 || !drone_supported(d->def) || s->systems[ds].ionTimer > 0.0f) return false;
    if (s->hackPulse > 0.0f && s->hackedSys == ds) return false;  // hacked: drones stay down
    if (drones_used(s) + d->def->power > ship_system_capacity(s, ds)) return false;
    if (d->def->power > ship_reactor_free(s)) return false;
    if (!d->deployed) {
        if (s->droneParts <= 0 || d->redeploy > 0.0f || !c->ship[!side] || c->result != COMBAT_FIGHTING) return false;
        if (is_crew_drone(d->def)) {
            if (is_boarder(d->def)) {
                d->boardTimer = BOARDER_FLIGHT;  // spawned when it arrives
            } else if (!spawn_crew_drone(c, side, i)) {
                return false;
            }
        } else {
            spawn_drone(c, side, i);
        }
        s->droneParts--;
        d->deployed = true;
        if (side == SIDE_PLAYER) {
            gFeats.dronesUsed++;
            if (d->def->type == DRONE_COMBAT || is_boarder(d->def)) gFeats.offensiveDrones++;
        }
        sound_play("droneLaunch");
    }
    d->powered = true;
    drones_sync(s);
    return true;
}

bool combat_toggle_drone(Combat* c, int side, int i) {
    DroneSlot* d = &c->ship[side]->drones[i];
    if (d->powered || d->wanted) {
        d->powered = d->wanted = false;
        drones_sync(c->ship[side]);
        return true;
    }
    if (!power_drone(c, side, i)) return false;
    d->wanted = true;
    return true;
}

void combat_set_drones_power(Combat* c, int side, int power) {
    Ship* s = c->ship[side];
    for (int i = 0; i < s->droneCount; i++) s->drones[i].powered = s->drones[i].wanted = false;
    drones_sync(s);
    for (int i = 0; i < s->droneCount; i++)
        if (drones_used(s) + s->drones[i].def->power <= power) combat_toggle_drone(c, side, i);
}

static void destroy_drone(Combat* c, DroneEntity* e) {
    if (e->tempShots > 0) {  // a surge drone: just gone
        add_effect(c, e->space, FX_EXPLOSION, e->x, e->y);
        e->active = false;
        return;
    }
    DroneSlot* d = &c->ship[e->side]->drones[e->slot];
    d->deployed = false;
    d->powered = false;
    d->redeploy = DRONE_REDEPLOY;
    drones_sync(c->ship[e->side]);
    add_effect(c, e->space, FX_EXPLOSION, e->x, e->y);
    e->active = false;
}

static void drone_fire(Combat* c, DroneEntity* e) {
    const DroneSlot* ds = &c->ship[e->side]->drones[e->slot];
    const WeaponDef* w = ds->def->weapon;
    Ship* o = c->ship[e->space];
    if (!w || o->roomCount == 0 || o->cloakTime > 0.0f) return;
    int room = rand() % o->roomCount;
    if (w->type == WEAPON_BEAM) {
        for (int i = 0; i < MAX_BEAMS; i++) {
            Beam* b = &c->beams[i];
            if (b->active) continue;
            memset(b, 0, sizeof(*b));
            b->active = true;
            b->from = e->side;
            b->weapon = -1;
            b->drone = e->slot;
            const Room* r = &o->rooms[room];
            b->sx = (o->xOffset + r->x + r->w * 0.5f) * TILE;
            b->sy = (o->yOffset + r->y + r->h * 0.5f) * TILE;
            float a = (rand() % 8) * (float)M_PI / 4;
            b->ex = b->sx + cosf(a) * w->length;
            b->ey = b->sy + sinf(a) * w->length;
            b->duration = DRONE_BEAM_TIME;
            break;
        }
        e->pause += DRONE_BEAM_TIME;  // stays put while the beam is on
    } else {
        for (int i = 0; i < MAX_SHOTS; i++) {
            Projectile* p = &c->shots[i];
            if (p->active) continue;
            memset(p, 0, sizeof(*p));
            p->evade = -1;
            p->active = true;
            p->from = e->side;
            p->weapon = -1;
            p->drone = e->slot;
            p->inTarget = true;
            p->room = room;
            p->sx = e->x;
            p->sy = e->y;
            room_point(o, room, &p->tx, &p->ty);
            float dist = hypotf(p->tx - p->sx, p->ty - p->sy);
            p->speed = (float)(w->speed > 0 ? w->speed : 60);
            p->duration = dist / (p->speed * DRONE_SHOT_PX) + 0.05f;
            roll_miss(p, evasion(c, o));
            break;
        }
    }
    play_random(w->launch, w->launchCount);
}

// Defence drones: shoot the next eligible incoming shot (or, for anti-drones, an enemy drone).
static void drone_defend(Combat* c, DroneEntity* e, const DroneDef* d) {
    if (d->target == DRONE_TARGET_DRONES) {
        for (int i = 0; i < MAX_DRONE_ENTITIES; i++) {
            DroneEntity* t = &c->droneEnts[i];
            if (!t->active || t->side == e->side || t->space != e->space || t->stun > 0.0f) continue;
            t->stun = 5.0f;  // one point of ion: stunned for 5 s
            e->shotX = t->x;
            e->shotY = t->y;
            e->shotFlash = 0.15f;
            e->cooldown = d->cooldown;
            play_random(d->weapon->launch, d->weapon->launchCount);
            return;
        }
        return;
    }
    for (int i = 0; i < MAX_SHOTS; i++) {
        Projectile* p = &c->shots[i];
        if (!p->active || p->from == e->side || p->drone >= 0 || p->shotDown || p->t < 0.55f) continue;
        if (p->weapon == SHOT_PDS) continue;  // battery shots can't be shot down
        const WeaponDef* w = combat_shot_def(c, p->from, p->weapon, p->drone);
        if (!w) continue;
        bool missile = w->type == WEAPON_MISSILES || w->type == WEAPON_BURST;
        bool laser = w->type == WEAPON_LASER || w->type == WEAPON_ION;
        if (!(missile || (laser && d->target == DRONE_TARGET_LASERS))) continue;
        p->shotDown = true;
        e->shotX = p->tx;
        e->shotY = p->ty;
        e->shotFlash = 0.15f;
        e->cooldown = d->cooldown;
        if (d->weapon) play_random(d->weapon->launch, d->weapon->launchCount);
        return;
    }
}

static void update_drones(Combat* c, float dt) {
    for (int side = 0; side < 2; side++) {
        Ship* s = c->ship[side];
        for (int i = 0; i < s->droneCount; i++) {
            DroneSlot* d = &s->drones[i];
            if (d->redeploy > 0.0f) d->redeploy -= dt;
            if (d->deployed && is_crew_drone(d->def)) {
                if (d->boardTimer > 0.0f) {
                    d->boardTimer -= dt;
                    if (d->boardTimer <= 0.0f) {
                        // Zoltan shields destroy boarding drones on contact (wiki).
                        if (c->ship[!side]->superShield > 0 || !spawn_crew_drone(c, side, i)) d->deployed = false;
                    }
                    continue;
                }
                Crew* m = crew_drone(c, side, i);
                if (!m) {  // destroyed
                    d->deployed = d->powered = d->wanted = false;
                    d->redeploy = DRONE_REDEPLOY;
                    continue;
                }
                m->dormant = !d->powered;
            }
            if (d->wanted && !d->powered) power_drone(c, side, i);
        }
        drones_sync(s);
    }
    for (int i = 0; i < MAX_DRONE_ENTITIES; i++) {
        DroneEntity* e = &c->droneEnts[i];
        if (!e->active) continue;
        Ship* owner = c->ship[e->side];
        Ship* space = c->ship[e->space];
        const DroneDef* d = owner->drones[e->slot].def;
        if (e->shotFlash > 0.0f) e->shotFlash -= dt;
        if (e->stun > 0.0f) {
            e->stun -= dt;
            e->stunTime += dt;
            if (e->stunTime > 1.0f && rand() / (float)RAND_MAX < 0.15f * dt) destroy_drone(c, e);
            if (e->stun <= 0.0f) e->stunTime = 0.0f;
            continue;
        }
        if ((!owner->drones[e->slot].powered && e->tempShots <= 0) || c->result != COMBAT_FIGHTING) continue;
        if (e->side == SIDE_ENEMY && d->type == DRONE_DEFENSE) gFeats.fight.enemyDefenseDrone = true;
        float speed = drone_speed(space, d);

        if (d->type == DRONE_DEFENSE || d->type == DRONE_SHIELD) {
            // Orbit the shield bubble.
            float cx, cy, rw, rh;
            combat_shield_ellipse(space, &cx, &cy, &rw, &rh);
            e->angle += speed / (rw > rh ? rw : rh) * dt;
            e->x = cx + cosf(e->angle) * rw;
            e->y = cy + sinf(e->angle) * rh;
            if (e->cooldown > 0.0f) e->cooldown -= dt;
            bool scrambled = c->ship[!e->side] && c->ship[!e->side]->aug[AUG_SCRAMBLER];  // Defense Scrambler
            if (d->type == DRONE_DEFENSE && e->cooldown <= 0.0f && !scrambled) drone_defend(c, e, d);
            continue;
        }

        // Combat and hull-repair drones fly stop to stop.
        if (e->pause > 0.0f) {
            e->pause -= dt;
            if (e->pause > 0.0f) continue;
            if (d->type == DRONE_COMBAT && !e->fired) {
                // One shot per stop: its speed is its rate of fire (blueprints.xml).
                e->fired = true;
                drone_fire(c, e);
                if (e->tempShots > 0 && --e->tempShots == 0) {  // a surge drone has had its two shots
                    e->active = false;
                    continue;
                }
                if (e->pause > 0.0f) continue;  // beam drones wait for their beam
            } else if (d->type == DRONE_SHIP_REPAIR) {
                // Lasts for 3-5 points actually repaired (wiki: Drone Control); stops cost nothing
                // while the hull is whole.
                bool repaired = owner->hullHp < owner->maxHull;
                if (repaired) owner->hullHp++;
                if (repaired && --e->repairsLeft <= 0) {
                    // Used up: gone, and needs a new part to replace.
                    owner->drones[e->slot].deployed = false;
                    owner->drones[e->slot].powered = false;
                    owner->drones[e->slot].wanted = false;
                    drones_sync(owner);
                    e->active = false;
                    continue;
                }
            }
            pick_destination(e, space);
            continue;
        }
        float vx = e->dx - e->x, vy = e->dy - e->y, dist = hypotf(vx, vy);
        if (dist <= speed * dt) {
            e->x = e->dx;
            e->y = e->dy;
            e->pause = DRONE_PAUSE;
            e->fired = false;
        } else {
            e->x += vx / dist * speed * dt;
            e->y += vy / dist * speed * dt;
        }
    }
}

// Shield drones absorb a shot aimed at their ship when charged. Returns true if it did.
static bool shield_drone_absorbs(Combat* c, int side) {
    for (int i = 0; i < MAX_DRONE_ENTITIES; i++) {
        DroneEntity* e = &c->droneEnts[i];
        if (!e->active || e->side != side || e->stun > 0.0f) continue;
        const DroneDef* d = c->ship[side]->drones[e->slot].def;
        if (d->type != DRONE_SHIELD || !c->ship[side]->drones[e->slot].powered || e->cooldown > 0.0f) continue;
        e->cooldown = d->cooldown;
        return true;
    }
    return false;
}


// ---- Boarding ----------------------------------------------------------------------
// Crew combat from xftl (doc/damage-notes, doc/crew-ai) and the FTL wiki (Crew, Teleporter):
// a hit does 3-7 damage x the race's multiplier, once per 1.0-1.3 s; boarders with no one to
// fight sabotage the room's system at one bar per 12.5 s; the teleporter recharges for 20/15/10 s
// by level. Boarders pick rooms with xftl's PrioritizeIntruderRoom penalties.

#define SABOTAGE_SECONDS 12.5f
static const float TELEPORT_COOLDOWN[4] = {20.0f, 20.0f, 15.0f, 10.0f};

static float crew_damage_mult(const char* race) {
    if (!strcmp(race, "mantis")) return 1.5f;
    if (!strcmp(race, "engi")) return 0.5f;
    return 1.0f;
}

static int crew_room(const Ship* s, const Crew* m) {
    return ship_room_at(s, (int)floorf(m->x / TILE), (int)floorf(m->y / TILE));
}

// Whether a crew member standing on ship `side` belongs to the player (mind control flips
// `foreign` but not who they belong to).
static bool crew_is_ours(const Combat* c, int side, const Crew* m) {
    (void)c;
    bool native = m->foreign == (m->mind > 0.0f);
    return (native ? side : !side) == SIDE_PLAYER;
}

static bool sFistFight;  // someone is punching someone this frame (the "brawling" loop plays)

// Fights, sabotage, for everyone standing on ship `side`.
static void update_brawls(Combat* c, int side, float dt) {
    Ship* s = c->ship[side];
    CrewSet* cs = c->crew[side];
    for (int i = 0; i < cs->count; i++) {
        Crew* m = &cs->crew[i];
        m->brawling = false;
        if (m->pathPos < m->pathLen || m->stun > 0.0f || m->dormant) continue;
        if (m->drone && !strcmp(cs->races[m->race].name, "repair")) continue;
        int room = crew_room(s, m);
        if (room < 0) continue;
        if (is_intruder(cs, m)) {
            // Charges wherever it is (so a pulse can be stored), fires in a working system's room.
            if (m->brawlTimer > 0.0f) m->brawlTimer -= dt;
            int si = ship_system_in_room(s, room);
            if (m->brawlTimer <= 0.0f && si >= 0 && s->systems[si].damage < s->systems[si].def->level && room + 1 != m->ionLast) {
                m->brawlTimer = ION_PULSE_MIN + frand(0.0f, ION_PULSE_SPREAD);
                m->ionLast = (short)(room + 1);
                apply_ion(s, si, ION_INTRUDER_ION);
                for (int k2 = 0; k2 < cs->count; k2++) {
                    Crew* f = &cs->crew[k2];
                    if (f->foreign != m->foreign && crew_room(s, f) == room && f->stun < ION_INTRUDER_STUN) f->stun = ION_INTRUDER_STUN;
                }
                float px = (s->rooms[room].x + s->rooms[room].w * 0.5f + s->xOffset) * TILE;
                float py = (s->rooms[room].y + s->rooms[room].h * 0.5f + s->yOffset) * TILE;
                add_effect(c, side, FX_SHIELD, px, py);
                sound_play(rand() % 2 ? "ionHit1" : "ionHit2");
            }
            continue;
        }
        int foe = -1;
        for (int k = 0; k < cs->count && foe < 0; k++)
            if (cs->crew[k].foreign != m->foreign && crew_room(s, &cs->crew[k]) == room) foe = k;
        if (foe >= 0) {
            m->brawling = true;
            sFistFight = true;
            m->repairing = m->fighting = false;
            Crew* f = &cs->crew[foe];
            m->dir = f->y < m->y - 4 ? DIR_UP : DIR_DOWN;
            m->brawlTimer -= dt;
            if (m->brawlTimer <= 0.0f) {
                m->brawlTimer = 1.0f + (rand() % 300) / 1000.0f;
                float boost = m->mind > 0.0f ? (m->mindBoost >= 3 ? 2.0f : m->mindBoost >= 2 ? 1.25f : 1.0f) : 1.0f;
                bool alive = f->health > 0.0f;
                f->health -= (3 + rand() % 5) * crew_damage_mult(cs->races[m->race].name) * boost *
                             COMBAT_SKILL_DAMAGE[crew_skill_level(cs, m, SKILL_COMBAT)];
                f->lastHit = !crew_is_ours(c, side, m)                                     ? HIT_NONE
                             : m->drone && !strcmp(cs->races[m->race].name, "battle") && m->foreign ? HIT_BOARDER + m->droneSlot
                             : m->drone                                                     ? HIT_WEAPON
                                                                                            : HIT_CREW;
                f->lastHitBio = false;
                // The killing blow trains Combat for our crew (not on drones).
                if (alive && f->health <= 0.0f && !f->drone && crew_is_ours(c, side, m)) crew_add_xp(cs, m, SKILL_COMBAT);
                // deaths are handled by life_update
            }
            continue;
        }
        m->brawlTimer = 0.5f;
        // Boarders with no one to fight wreck the room's system.
        int si = ship_system_in_room(s, room);
        if (m->foreign && si >= 0 && s->systems[si].damage < s->systems[si].def->level) {
            ShipSystem* y = &s->systems[si];
            m->brawling = true;
            y->sabotage += dt / SABOTAGE_SECONDS;
            if (y->sabotage >= 1.0f) {
                y->sabotage = 0.0f;
                y->damage++;
                y->repair = 0.0f;
                if (crew_is_ours(c, side, m)) crew_add_xp(cs, m, SKILL_COMBAT);  // a bar of sabotage
                if (y->damage >= y->def->level) s->hullHp -= 1;  // wiki (Ship, Hull): a system wrecked by sabotage costs 1 hull
                if (y->def->type == SYS_WEAPONS) weapons_sync(s);
            }
        }
    }
}

// Boarders choose where to go (lower is better), as in xftl's PrioritizeIntruderRoom.
static int intruder_room_score(const Ship* s, const CrewSet* cs, const Crew* m, int room) {
    int score = 0;
    bool foes = false;
    for (int k = 0; k < cs->count && !foes; k++) foes = !cs->crew[k].foreign && crew_room(s, &cs->crew[k]) == room;
    if (foes && crew_room(s, m) == room) return -10;
    bool fire = life_room_fires(s, room) > 0, noAir = s->oxygen[room] < 10.0f;
    if (fire && strcmp(cs->races[m->race].name, "rock")) score += 100;
    if (noAir && cs->races[m->race].breathes) score += 70;
    int si = ship_system_in_room(s, room);
    if (si < 0 || s->systems[si].damage >= s->systems[si].def->level) score += 20;
    return score;
}

static void boarder_ai(Combat* c, int side) {
    Ship* s = c->ship[side];
    CrewSet* cs = c->crew[side];
    int target = -1;
    for (int i = 0; i < cs->count; i++) {
        Crew* m = &cs->crew[i];
        if (!m->foreign || m->brawling) continue;
        if (is_intruder(cs, m)) {
            // On to another working system after each pulse (wiki: Drone Control).
            int at = ship_room_at(s, m->destX, m->destY), si = at >= 0 ? ship_system_in_room(s, at) : -1;
            bool good = si >= 0 && s->systems[si].damage < s->systems[si].def->level && at + 1 != m->ionLast;
            if (good || m->pathPos < m->pathLen || s->systemCount == 0) continue;
            int picks[MAX_SYSTEMS], n = 0;
            for (int k2 = 0; k2 < s->systemCount; k2++)
                if (s->systems[k2].damage < s->systems[k2].def->level && s->systems[k2].room + 1 != m->ionLast) picks[n++] = s->systems[k2].room;
            if (n) crew_order(cs, s, i, picks[rand() % n]);
            continue;
        }
        int cur = ship_room_at(s, m->destX, m->destY);
        int best = target >= 0 ? target : cur, bestScore = best >= 0 ? intruder_room_score(s, cs, m, best) : 1000;
        for (int r = 0; r < s->roomCount; r++) {
            int sc = intruder_room_score(s, cs, m, r);
            if (sc < bestScore || (sc == bestScore && rand() % 4 == 0 && r != best)) {
                bestScore = sc;
                best = r;
            }
        }
        target = best;  // the rest follow the first, as in the game
        if (best >= 0 && best != cur) crew_order(cs, s, i, best);
    }
}

// The ship's own crew go after boarders (the enemy's do; ours only fight where they stand).
static void defender_ai(Combat* c, int side) {
    Ship* s = c->ship[side];
    CrewSet* cs = c->crew[side];
    for (int i = 0; i < cs->count; i++) {
        if (!cs->crew[i].foreign) continue;
        int room = crew_room(s, &cs->crew[i]);
        int sent = 0;
        for (int k = 0; k < cs->count && sent < 2; k++) {
            Crew* d = &cs->crew[k];
            if (d->foreign || d->drone) continue;
            if (ship_room_at(s, d->destX, d->destY) == room) {
                sent++;
                continue;
            }
            if (d->manning && rand() % 2) continue;  // keep some stations manned
            if (crew_order(cs, s, k, room)) sent++;
        }
    }
}

// Free tiles of a room on a ship, up to max. Returns how many were written.
static int free_tiles(const Ship* s, const CrewSet* cs, int room, int out[][2], int max, bool foreign) {
    const Room* r = &s->rooms[room];
    int n = 0;
    for (int y = r->y; y < r->y + r->h && n < max; y++)
        for (int x = r->x; x < r->x + r->w && n < max; x++) {
            bool used = false;
            for (int k = 0; k < cs->count && !used; k++) used = cs->crew[k].foreign == foreign && cs->crew[k].destX == x && cs->crew[k].destY == y;
            if (!used) {
                out[n][0] = x;
                out[n][1] = y;
                n++;
            }
        }
    return n;
}

int combat_teleporter_state(const Combat* c, int side) {
    const Ship* s = c->ship[side];
    int t = ship_find_system(s, SYS_TELEPORTER);
    if (t < 0) return -1;
    if (s->systems[t].ionTimer > 0.0f) return 1;
    if (s->systems[t].power <= 0) return 2;
    return 0;
}

static void teleport_cooldown(Ship* s) {
    int t = ship_find_system(s, SYS_TELEPORTER);
    int lvl = ship_system_capacity(s, t);
    s->systems[t].ionTimer += TELEPORT_COOLDOWN[lvl < 3 ? lvl : 3];
    sound_play("teleport");
}

int combat_teleport_send(Combat* c, int side, int room) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    if (!o || combat_teleporter_state(c, side) != 0 || room < 0 || room >= o->roomCount || o->hullHp <= 0 ||
        (o->superShield > 0 && !s->aug[AUG_ZOLTAN_BYPASS]))
        return 0;
    CrewSet* home = c->crew[side];
    CrewSet* away = c->crew[!side];
    int troom = s->systems[ship_find_system(s, SYS_TELEPORTER)].room;
    int tiles[8][2], n = free_tiles(o, away, room, tiles, 4, true), sent = 0;
    for (int i = home->count - 1; i >= 0 && sent < n; i--) {
        Crew* m = &home->crew[i];
        // Drones never teleport (boarding drones fly over on their own).
        if (m->foreign || m->drone || m->pathPos < m->pathLen || crew_room(s, m) != troom) continue;
        if (crew_transfer(home, i, away, tiles[sent][0], tiles[sent][1])) {
            sent++;
            Crew* t = &away->crew[away->count - 1];  // Reconstructive Teleport: arrives fully healed
            if (s->aug[AUG_TELEPORT_HEAL] && t->health < away->races[t->race].maxHealth) t->health = away->races[t->race].maxHealth;
        }
    }
    if (sent) teleport_cooldown(s);
    if (sent && side == SIDE_PLAYER) gFeats.teleports++;
    return sent;
}

int combat_teleport_return(Combat* c, int side) {
    Ship* s = c->ship[side];
    if (!c->ship[!side] || !c->crew[!side] || combat_teleporter_state(c, side) != 0) return 0;  // nobody over there
    CrewSet* home = c->crew[side];
    CrewSet* away = c->crew[!side];
    int troom = s->systems[ship_find_system(s, SYS_TELEPORTER)].room;
    int back = 0;
    for (int i = away->count - 1; i >= 0 && back < 4; i--) {
        if (!away->crew[i].foreign || away->crew[i].drone) continue;  // boarding drones stay
        int tiles[8][2], n = free_tiles(s, home, troom, tiles, 1, false);
        for (int r = 0; n == 0 && r < s->roomCount; r++) n = free_tiles(s, home, r, tiles, 1, false);
        if (n && crew_transfer(away, i, home, tiles[0][0], tiles[0][1])) {
            back++;
            Crew* t = &home->crew[home->count - 1];
            if (s->aug[AUG_TELEPORT_HEAL] && t->health < home->races[t->race].maxHealth) t->health = home->races[t->race].maxHealth;
        }
    }
    if (back) teleport_cooldown(s);
    return back;
}

int combat_debug_board(Combat* c, int side, int n) {
    Ship* o = c->ship[!side];
    CrewSet* home = c->crew[side];
    CrewSet* away = c->crew[!side];
    if (!o || o->roomCount == 0) return 0;
    int room = o->systemCount ? o->systems[rand() % o->systemCount].room : 0;
    int tiles[8][2], free = free_tiles(o, away, room, tiles, n, true), moved = 0;
    for (int i = home->count - 1; i >= 0 && moved < free; i--)
        if (!home->crew[i].foreign && crew_transfer(home, i, away, tiles[moved][0], tiles[moved][1])) moved++;
    if (moved) sound_play("teleport");
    return moved;
}

// Repair drones go to fires, breaches and damage; battle drones go to boarders; both return to
// the drone room when there's nothing to do (wiki: Drone Control).
static void crew_drone_ai(Combat* c, int side) {
    Ship* s = c->ship[side];
    CrewSet* cs = c->crew[side];
    int home = ship_find_system(s, SYS_DRONES);
    for (int i = 0; i < cs->count; i++) {
        Crew* m = &cs->crew[i];
        if (!m->drone || m->foreign || m->dormant || m->stun > 0.0f) continue;
        bool repair = !strcmp(cs->races[m->race].name, "repair");
        int best = -1;
        if (repair) {
            for (int r = 0; r < s->roomCount && best < 0; r++)
                if (life_room_fires(s, r) > 0 && s->oxygen[r] >= 10.0f) best = r;
            for (int r = 0; r < s->roomCount && best < 0; r++)
                if (life_room_breaches(s, r) > 0) best = r;
            for (int k = 0; k < s->systemCount && best < 0; k++)
                if (s->systems[k].damage > 0) best = s->systems[k].room;
        } else {
            for (int k = 0; k < cs->count && best < 0; k++)
                if (cs->crew[k].foreign) best = crew_room(s, &cs->crew[k]);
        }
        if (best < 0 && home >= 0) best = s->systems[home].room;
        if (best >= 0 && ship_room_at(s, m->destX, m->destY) != best) crew_order(cs, s, i, best);
    }
}

// Enemy boarding: twice per fight, two crew from the teleporter room to a random system room;
// boarders below 25% health are recalled (wiki: Teleporter).
static void enemy_teleport_ai(Combat* c) {
    Ship* e = c->ship[SIDE_ENEMY];
    Ship* p = c->ship[SIDE_PLAYER];
    CrewSet* cs = c->crew[SIDE_ENEMY];
    int t = ship_find_system(e, SYS_TELEPORTER);
    if (t < 0 || c->result != COMBAT_FIGHTING) return;
    CrewSet* pc = c->crew[SIDE_PLAYER];
    for (int i = 0; i < pc->count; i++)
        if (pc->crew[i].foreign && pc->crew[i].health < 0.25f * pc->races[pc->crew[i].race].maxHealth &&
            combat_teleporter_state(c, SIDE_ENEMY) == 0) {
            combat_teleport_return(c, SIDE_ENEMY);
            return;
        }
    if (c->enemyBoardings >= 2 || crew_native_count(cs) < 3 || crew_foreign_count(pc) > 0 || p->superShield > 0) return;
    int troom = e->systems[t].room, inRoom = 0;
    for (int i = 0; i < cs->count; i++)
        if (!cs->crew[i].foreign && cs->crew[i].pathPos >= cs->crew[i].pathLen && crew_room(e, &cs->crew[i]) == troom) inRoom++;
    if (inRoom >= 2 && combat_teleporter_state(c, SIDE_ENEMY) == 0) {
        int room = p->systems[rand() % p->systemCount].room;
        if (combat_teleport_send(c, SIDE_ENEMY, room)) c->enemyBoardings++;
        return;
    }
    // Walk two crew to the pads.
    int called = inRoom;
    for (int i = 0; i < cs->count && called < 2; i++) {
        Crew* m = &cs->crew[i];
        if (m->foreign || ship_room_at(e, m->destX, m->destY) == troom) continue;
        if (crew_order(cs, e, i, troom)) called++;
    }
}


// ---- Hacking and mind control ------------------------------------------------------
// xftl Hacking.kt / MindControl.kt and the FTL wiki: a hacking drone (1 drone part) lands on an
// enemy system; while attached the system can't be manned and repairs at half speed. A pulse lasts
// 1 + 3 x power seconds (4/7/10) and disables the system (shields lose a layer every 2 s, weapons
// lose charge), then the hacking system recharges for 20 s. Mind control turns one enemy crew
// member in a room for 14/20/28 s by power, then recharges for 20 s; Slugs are immune; Zoltan
// shields block both.

#define HACK_FLIGHT 2.5f   // our choice: seconds for the hacking drone to reach the enemy
#define HACK_RECHARGE 20.0f
#define MIND_RECHARGE 20.0f
static const float MIND_SECONDS[4] = {0.0f, 14.0f, 20.0f, 28.0f};

int combat_hack_state(const Combat* c, int side) {
    const Ship* s = c->ship[side];
    const Ship* o = c->ship[!side];
    int h = ship_find_system(s, SYS_HACKING);
    if (h < 0) return HACK_NONE;
    if (o && o->hackPulse > 0.0f && o->hackedSys >= 0) return HACK_PULSING;
    if (s->hackFlight > 0.0f) return HACK_FLYING;
    if (!o || o->hackedSys < 0) return HACK_NO_DRONE;
    if (s->systems[h].ionTimer > 0.0f) return HACK_RECHARGING;
    if (ship_system_power(s, h) <= 0) return HACK_NO_POWER;
    return HACK_READY;
}

bool combat_hack_launch(Combat* c, int side, int room) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    int h = ship_find_system(s, SYS_HACKING);
    int target = o ? ship_system_in_room(o, room) : -1;
    if (h < 0 || target < 0 || o->hackedSys >= 0 || s->hackFlight > 0.0f || s->droneParts <= 0 || o->superShield > 0)
        return false;
    s->droneParts--;
    s->hackFlight = HACK_FLIGHT;
    if (side == SIDE_PLAYER) {
        gFeats.dronesUsed++;
        gFeats.offensiveDrones++;
    }
    s->hackAim = target;
    sound_play("droneLaunch");
    return true;
}

bool combat_hack_pulse(Combat* c, int side) {
    Ship* o = c->ship[!side];
    if (combat_hack_state(c, side) != HACK_READY) return false;
    Ship* s = c->ship[side];
    o->hackPulse = 1.0f + 3.0f * ship_system_power(s, ship_find_system(s, SYS_HACKING));
    sound_play("hackStart");
    return true;
}

int combat_mind_state(const Combat* c, int side) {
    const Ship* s = c->ship[side];
    int m = ship_find_system(s, SYS_MIND);
    if (m < 0) return MIND_NONE;
    if (s->systems[m].ionTimer > 0.0f) return MIND_RECHARGING;
    if (ship_system_power(s, m) <= 0) return MIND_NO_POWER;
    return MIND_READY;
}

// Controls a random enemy (of `side`) standing in `room` of the other ship, or on our own ship.
bool combat_mind_control(Combat* c, int side, int room, bool onOwnShip) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    if (combat_mind_state(c, side) != MIND_READY) return false;
    if (!onOwnShip && (!o || (o->superShield > 0 && !s->aug[AUG_ZOLTAN_BYPASS]))) return false;
    int ms = ship_find_system(s, SYS_MIND);
    int power = ship_system_power(s, ms);
    Ship* where = onOwnShip ? s : o;
    CrewSet* cs = c->crew[onOwnShip ? side : !side];
    if (!cs) return false;
    int picks[MAX_CREW], n = 0;
    for (int i = 0; i < cs->count; i++) {
        Crew* m = &cs->crew[i];
        // Hostile to `side`: natives over there, or boarders here.
        bool hostile = onOwnShip ? m->foreign : !m->foreign;
        if (hostile && !m->drone && m->mind <= 0.0f && strcmp(cs->races[m->race].name, "slug") &&
            crew_room(where, m) == room)
            picks[n++] = i;
    }
    if (n == 0) return false;
    Crew* m = &cs->crew[picks[rand() % n]];
    m->mind = MIND_SECONDS[power < 3 ? power : 3];
    m->foreign = !m->foreign;  // now fights for the other side
    m->mindBoost = power;
    m->health += power >= 3 ? 30.0f : power >= 2 ? 15.0f : 0.0f;
    s->systems[ms].ionTimer += MIND_RECHARGE;
    sound_play("mindControl");
    return true;
}

static void update_hack_mind(Combat* c, int side, float dt) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    // Our hacking drone in flight lands on its system.
    if (s->hackFlight > 0.0f) {
        s->hackFlight -= dt;
        if (s->hackFlight <= 0.0f) {
            s->hackFlight = 0.0f;
            if (o->superShield <= 0) {
                o->hackedSys = s->hackAim;
                sound_play("hackLand");
            }
        }
    }
    // A pulse against this ship.
    if (s->hackPulse > 0.0f) {
        s->hackPulse -= dt;
        if (s->hackedSys >= 0) {
            int type = s->systems[s->hackedSys].def->type;
            if (type == SYS_CLOAKING && s->cloakTime > 0.0f) s->cloakTime = 0.001f;
            if (type == SYS_SHIELDS) {
                s->hackDrain += dt;
                s->shieldCharge = 0.0f;
                if (s->hackDrain >= 2.0f) {
                    s->hackDrain -= 2.0f;
                    if (s->shieldLayers > 0) s->shieldLayers--;
                }
            }
            if (type == SYS_DRONES)
                for (int i = 0; i < s->droneCount; i++) s->drones[i].powered = false;
            // Hacking Stun: everyone in the hacked room, either side's, stays stunned for the pulse.
            if (o && o->aug[AUG_HACK_STUN]) {
                CrewSet* hs = c->crew[side];
                int hr = s->systems[s->hackedSys].room;
                for (int i = 0; hs && i < hs->count; i++) {
                    Crew* m = &hs->crew[i];
                    if (ship_room_at(s, (int)floorf(m->x / TILE), (int)floorf(m->y / TILE)) == hr && m->stun < s->hackPulse)
                        m->stun = s->hackPulse;
                }
            }
        }
        if (s->hackPulse <= 0.0f) {
            s->hackPulse = 0.0f;
            s->hackDrain = 0.0f;
            int h = ship_find_system(o, SYS_HACKING);
            if (h >= 0) o->systems[h].ionTimer += HACK_RECHARGE;
        }
    }
    // Mind control wears off (both ships' crews at once: this is called for each side, so only once).
    if (side != SIDE_PLAYER) return;
    CrewSet* sets[2] = {c->crew[0], c->crew[1]};
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < sets[k]->count; i++) {
            Crew* m = &sets[k]->crew[i];
            if (m->mind <= 0.0f) continue;
            m->mind -= dt;
            if (m->mind <= 0.0f) {
                m->mind = 0.0f;
                m->foreign = !m->foreign;
                m->mindBoost = 0;
                float maxHp = sets[k]->races[m->race].maxHealth;
                if (m->health > maxHp) m->health = maxHp;
                sound_play("mindControlEnd");
            }
        }
}

// Enemy: launch a hacking drone at our shields (or weapons), pulse whenever ready; mind-control
// whoever is in a room with our crew.
static void enemy_special_ai(Combat* c) {
    Ship* e = c->ship[SIDE_ENEMY];
    Ship* p = c->ship[SIDE_PLAYER];
    if (ship_find_system(e, SYS_HACKING) >= 0) {
        if (p->hackedSys < 0 && e->hackFlight <= 0.0f) {
            int t = ship_find_system(p, SYS_SHIELDS);
            if (t < 0) t = ship_find_system(p, SYS_WEAPONS);
            if (t >= 0) {
                if (e->droneParts <= 0) e->droneParts = 1;
                combat_hack_launch(c, SIDE_ENEMY, p->systems[t].room);
            }
        }
        combat_hack_pulse(c, SIDE_ENEMY);
    }
    if (combat_mind_state(c, SIDE_ENEMY) == MIND_READY) {
        CrewSet* pc = c->crew[SIDE_PLAYER];
        for (int i = 0; i < pc->count; i++)
            if (!pc->crew[i].foreign && !pc->crew[i].drone && strcmp(pc->races[pc->crew[i].race].name, "slug")) {
                combat_mind_control(c, SIDE_ENEMY, crew_room(p, &pc->crew[i]), false);
                break;
            }
    }
}

// Living crew (not drones) that belong to ship `owner`, wherever they are and whoever controls
// them right now: mind control flips `foreign` but not who they belong to (xftl: "check the crew
// owners, so that mind-controlling the last crew doesn't break it").
static int crew_owned_by(const Combat* c, int owner) {
    int n = 0;
    for (int k = 0; k < 2; k++) {
        const CrewSet* cs = c->crew[k];
        for (int i = 0; i < cs->count; i++) {
            const Crew* m = &cs->crew[i];
            if (m->drone) continue;
            bool native = m->foreign == (m->mind > 0.0f);  // native to the ship of this set
            if ((native ? k : !k) == owner) n++;
        }
    }
    return n;
}

// The ship's sounds that loop while something is going on.
static void update_loops(Combat* c) {
    bool repairing = false;
    for (int i = 0; i < c->crew[SIDE_PLAYER]->count && !repairing; i++) repairing = c->crew[SIDE_PLAYER]->crew[i].repairing;
    sound_loop("repair", repairing);
    sound_loop("fire", life_ship_fires(c->ship[SIDE_PLAYER]) > 0);
    sound_loop("brawling", sFistFight);
    bool extinguishing = false;
    for (int i = 0; i < c->crew[SIDE_PLAYER]->count; i++) extinguishing |= c->crew[SIDE_PLAYER]->crew[i].fighting;
    sound_loop("extinguish", extinguishing);
    sFistFight = false;
    sound_loop("airLeak", life_ship_breaches(c->ship[SIDE_PLAYER]) > 0);
}

// ---- Environment -------------------------------------------------------------------------
// Beacon hazards (wiki: Environmental Hazards; xftl doc/asteroids, doc/solar-flares and its
// *Environment classes). They work on both ships, with or without a fight.

static float frand(float lo, float hi) { return lo + (hi - lo) * (rand() / (float)RAND_MAX); }

// Projectiles flying and landing, and explosion effects fading.
static void update_shots(Combat* c, float dt) {
    for (int i = 0; i < MAX_SHOTS; i++) {
        Projectile* p = &c->shots[i];
        if (!p->active) continue;
        p->t += dt / p->duration;
        if (p->t >= 1.0f) {
            p->active = false;
            const Ship* target = c->ship[!p->from];
            if (target && target->hullHp > 0) resolve(c, p);
        }
    }
    for (int i = 0; i < MAX_EFFECTS; i++)
        if (c->effects[i].active && (c->effects[i].t += dt) >= EFFECT_TIME) c->effects[i].active = false;
}

static bool env_target_ok(const Combat* c, int side) {
    const Ship* s = c->ship[side];
    return s && s->hullHp > 0 && s->deathTime < 0.0f && s->roomCount > 0;
}

// An asteroid or battery shot from off-screen at a random room of ship `side`.
static void hazard_shot(Combat* c, int side, int kind) {
    if (!env_target_ok(c, side)) return;
    const Ship* s = c->ship[side];
    for (int i = 0; i < MAX_SHOTS; i++) {
        Projectile* p = &c->shots[i];
        if (p->active) continue;
        memset(p, 0, sizeof(*p));
        p->evade = -1;
        const WeaponDef* d = combat_shot_def(c, !side, kind, -1);
        p->active = true;
        p->from = !side;
        p->weapon = kind;
        p->drone = -1;
        p->inTarget = true;
        p->room = rand() % s->roomCount;
        room_point(s, p->room, &p->tx, &p->ty);
        float a = frand(0.0f, 6.2831f), dist = 700.0f;  // in from beyond the edge of the view
        p->sx = p->tx + cosf(a) * dist;
        p->sy = p->ty + sinf(a) * dist;
        p->speed = (float)d->speed;
        p->duration = dist / (p->speed * DRONE_SHOT_PX);
        roll_miss(p, evasion(c, s));
        return;
    }
}

// Asteroid waves by our shield bubbles when we arrived (xftl AsteroidEnvironment).
static float asteroid_phase_time(int phase, int shields) {
    if (phase == 0) return shields <= 2 ? frand(5, 10) : frand(12, 15);
    if (phase == 1)
        return shields <= 1 ? frand(8, 16) : shields == 2 ? frand(10, 20) : shields == 3 ? frand(16, 28) : frand(16, 30);
    return shields <= 1 ? frand(5, 8) : shields == 2 ? frand(5, 10) : shields == 3 ? frand(8, 13) : frand(8, 11);
}

static float asteroid_period(int phase, int shields) {
    if (phase == 1)
        return shields <= 1   ? frand(1.8f, 2.2f)
               : shields == 2 ? frand(1.0f, 1.8f)
               : shields == 3 ? frand(0.9f, 1.4f)
                              : frand(0.72f, 1.35f);
    return shields <= 1 ? frand(1.4f, 1.6f) : shields <= 3 ? frand(0.9f, 1.3f) : frand(0.54f, 0.95f);
}

// A solar flare: 3-6 fires with the shields down, 1-2 with them up, in ones and twos per room;
// each such room may take a point of hull and system damage (33% / 66%), never twice.
static void solar_flare(Combat* c, int side) {
    if (!env_target_ok(c, side)) return;
    Ship* s = c->ship[side];
    hazard_defs_init();
    bool shielded = s->shieldLayers > 0 || s->superShield > 0;
    int fires = shielded ? 1 + rand() % 2 : 3 + rand() % 4;
    unsigned long long damaged = 0;
    while (fires > 0) {
        int room = rand() % s->roomCount, started = 1;
        life_ignite(s, room);
        if (fires > 1 && rand() % 2) {
            life_ignite(s, room);
            started++;
        }
        if (rand() % 100 < started * 33 && !(damaged & (1ull << room))) {
            damaged |= 1ull << room;
            sDmgSrc = SRC_HAZARD;
            damage_room(c, side, room, 1, &sFlareDef, false);  // no crew damage
        }
        fires -= started;
    }
}

// A pulsar's ion pulse: a Zoltan shield soaks it up; otherwise powered shields and one random
// system, or two random systems, each take 1 + power/2 ion. Reverse Ion Field resists all or none.
static void pulsar_pulse(Combat* c, int side) {
    if (!env_target_ok(c, side)) return;
    Ship* s = c->ship[side];
    bool resist = ion_resisted(s);
    int sh = ship_find_system(s, SYS_SHIELDS);
    if (s->superShield > 0 && sh >= 0) {
        if (!resist) {
            s->superShield -= 3 + rand() % 2;
            if (s->superShield < 0) s->superShield = 0;
        }
        return;
    }
    if (resist || s->systemCount == 0) return;
    int targets[2];
    targets[0] = sh >= 0 && s->systems[sh].power > 0 ? sh : rand() % s->systemCount;
    targets[1] = rand() % s->systemCount;
    for (int k = 0; k < 2; k++) {
        const ShipSystem* y = &s->systems[targets[k]];
        int power = system_is_subsystem(y->def->type) ? ship_system_capacity(s, targets[k]) : y->power;
        ion_system(s, targets[k], 1 + power / 2);
    }
}

// An ion storm halves a ship's reactor (rounded up for what's left); what no longer fits is
// switched off: weapons and drones first, then systems, shields last.
static void storm_power(Combat* c, int side, bool storm) {
    Ship* s = c->ship[side];
    if (!s) return;
    s->reactorCut = storm ? s->reactor / 2 : 0;
    for (int guard = 0; guard < 64 && ship_reactor_free(s) < 0; guard++) {
        bool cut = false;
        for (int i = s->weaponCount - 1; i >= 0 && !cut; i--)
            if (s->weapons[i].powered) {
                s->weapons[i].powered = false;
                weapons_sync(s);
                cut = true;
            }
        for (int i = s->droneCount - 1; i >= 0 && !cut; i--)
            if (s->drones[i].powered) {
                s->drones[i].powered = s->drones[i].wanted = false;
                drones_sync(s);
                cut = true;
            }
        for (int pass = 0; pass < 2 && !cut; pass++)
            for (int i = s->systemCount - 1; i >= 0 && !cut; i--) {
                ShipSystem* y = &s->systems[i];
                int t = y->def->type;
                if (system_is_subsystem(t) || t == SYS_WEAPONS || t == SYS_DRONES || y->power <= 0) continue;
                if ((t == SYS_SHIELDS) != (pass == 1)) continue;
                y->power--;
                cut = true;
            }
        if (!cut) break;
    }
}

void combat_set_environment(Combat* c, int env, int targets) {
    if (c->env == env && c->envTargets == targets) return;
    c->env = env;
    c->envTargets = targets;
    c->envPhase = 1;  // xftl starts asteroid fields with a slow wave
    c->envAtEnemy = true;
    c->envWarned = c->envSounded = false;
    const Ship* p = c->ship[SIDE_PLAYER];
    c->envShields = p ? combat_max_shields(p) : 0;
    c->envPhaseTime = asteroid_phase_time(c->envPhase, c->envShields);
    c->envNext = env == ENV_SUN ? frand(28, 34) : env == ENV_PULSAR ? frand(11, 18) : env == ENV_PDS ? frand(20, 30) : 0.0f;
}

float combat_env_warning(const Combat* c) {
    float window = c->env == ENV_PDS ? 7.0f : 5.0f;
    if ((c->env == ENV_SUN || c->env == ENV_PULSAR || c->env == ENV_PDS) && c->envNext < window) return c->envNext;
    return -1.0f;
}

static void update_environment(Combat* c, float dt) {
    if (c->envFlash > 0.0f) c->envFlash -= dt;
    for (int side = 0; side < 2; side++) storm_power(c, side, c->env == ENV_STORM);
    switch (c->env) {
        case ENV_ASTEROID:
            c->envPhaseTime -= dt;
            if (c->envPhaseTime <= 0.0f) {
                c->envPhase = c->envPhase ? 0 : 1 + rand() % 2;
                c->envPhaseTime = asteroid_phase_time(c->envPhase, c->envShields);
                if (c->envPhase) c->envNext = 0.0f;  // a wave opens with a shot
            }
            if (c->envPhase) {
                c->envNext -= dt;
                if (c->envNext <= 0.0f) {
                    // Alternately at each ship; with no enemy, its turns are wasted (xftl).
                    hazard_shot(c, c->envAtEnemy ? SIDE_ENEMY : SIDE_PLAYER, SHOT_ASTEROID);
                    c->envAtEnemy = !c->envAtEnemy;
                    c->envNext = asteroid_period(c->envPhase, c->envShields);
                }
            }
            break;
        case ENV_SUN:
        case ENV_PULSAR:
        case ENV_PDS:
            c->envNext -= dt;
            if (c->envNext < (c->env == ENV_PDS ? 7.0f : 5.0f) && !c->envWarned) {
                c->envWarned = true;
                sound_play("environWarning");
            }
            if (c->env != ENV_PDS && c->envNext < 1.2f && !c->envSounded) {
                c->envSounded = true;
                sound_play(c->env == ENV_SUN ? "solarFlare" : "pulsar");
            }
            if (c->envNext <= 0.0f) {
                for (int side = 0; side < 2; side++) {
                    if (c->env == ENV_SUN)
                        solar_flare(c, side);
                    else if (c->env == ENV_PULSAR)
                        pulsar_pulse(c, side);
                    else if (c->envTargets & (1 << side))
                        hazard_shot(c, side, SHOT_PDS);
                }
                if (c->env != ENV_PDS) {
                    c->envFlash = 1.0f;
                    c->envFlashKind = c->env;
                }
                c->envNext = c->env == ENV_SUN ? frand(28, 34) : c->env == ENV_PULSAR ? frand(11, 18) : frand(20, 30);
                c->envWarned = c->envSounded = false;
            }
            break;
        default:
            break;
    }
}

// ---- The rebel flagship ----------------------------------------------------------------------
// Power surges (wiki: The Rebel Flagship): every 20-30 s with a 5 s warning. Stage 2 sends six
// extra drones (combat and beam, the split fixed for the fight) that fire twice and leave; stage 3
// fires seven 1-damage heavy lasers, every fourth surge restoring its Zoltan shield instead.

// Stage 2 surges launch 4 / 6 / 7 drones on easy / normal / hard (wiki).
static int surge_drone_count(const Combat* c) {
    return c->difficulty == DIFF_EASY ? 4 : c->difficulty == DIFF_HARD ? 7 : 6;
}

void combat_set_boss(Combat* c, int stage) {
    c->bossStage = stage;
    c->surgeNext = frand(20, 30);
    c->surgeCount = 0;
    c->surgeBeams = rand() % (surge_drone_count(c) + 1);
    c->surgeWarned = false;
}

float combat_surge_warning(const Combat* c) {
    return c->bossStage >= 2 && c->result == COMBAT_FIGHTING && c->surgeNext < 5.0f ? c->surgeNext : -1.0f;
}

static void surge_laser(Combat* c, int side) {
    Ship* s = c->ship[side];
    Ship* o = c->ship[!side];
    if (!o || o->roomCount == 0) return;
    hazard_defs_init();
    for (int i = 0; i < MAX_SHOTS; i++) {
        Projectile* p = &c->shots[i];
        if (p->active) continue;
        memset(p, 0, sizeof(*p));
        p->evade = -1;
        p->active = true;
        p->from = side;
        p->weapon = SHOT_SURGE;
        p->drone = -1;
        p->room = rand() % o->roomCount;
        p->sx = (float)(s->xOffset * TILE) + s->hullX + s->hull.width * 0.5f;
        p->sy = (float)(s->yOffset * TILE) + s->hullY + s->hull.height * 0.5f;
        room_point(o, p->room, &p->tx, &p->ty);
        float dist = side == SIDE_PLAYER ? DIST_TO_ENEMY : DIST_TO_PLAYER;
        p->speed = (float)(sSurgeDef.speed > 0 ? sSurgeDef.speed : 60);
        p->duration = fire_delay(&sSurgeDef) + dist / p->speed + (rand() % 40) / 100.0f;
        roll_miss(p, evasion(c, o));
        return;
    }
}

static void update_boss(Combat* c, float dt) {
    Ship* e = c->ship[SIDE_ENEMY];
    if (c->bossStage < 2 || !e || e->hullHp <= 0 || c->result != COMBAT_FIGHTING) return;
    c->surgeNext -= dt;
    if (c->surgeNext < 5.0f && !c->surgeWarned) {
        c->surgeWarned = true;
        sound_play("environWarning");
    }
    if (c->surgeNext > 0.0f) return;
    c->surgeNext = frand(20, 30);
    c->surgeWarned = false;
    if (c->bossStage == 2) {
        int combatSlot = -1, beamSlot = -1;
        for (int i = 0; i < e->droneCount; i++) {
            const DroneDef* d = e->drones[i].def;
            if (d->type != DRONE_COMBAT || !d->weapon) continue;
            if (d->weapon->type == WEAPON_BEAM) beamSlot = i;
            else combatSlot = i;
        }
        for (int k = 0; k < surge_drone_count(c); k++) {
            int slot = k < c->surgeBeams ? (beamSlot >= 0 ? beamSlot : combatSlot) : (combatSlot >= 0 ? combatSlot : beamSlot);
            if (slot < 0) break;
            DroneEntity* d = spawn_drone(c, SIDE_ENEMY, slot);
            if (d) d->tempShots = 2;
        }
        sound_play("droneLaunch");
    } else {
        if (c->surgeCount % 4 == 3) {
            e->superShield = e->superShieldMax;  // the Zoltan shield comes back
            c->shieldFlash[SIDE_ENEMY] = 0.4f;
        } else {
            for (int k = 0; k < 7; k++) surge_laser(c, SIDE_ENEMY);
            play_random(sSurgeDef.launch, sSurgeDef.launchCount);
        }
    }
    c->surgeCount++;
}

void combat_update(Combat* c, float dt) {
    if (!c->ship[SIDE_PLAYER]) return;
    if (!c->ship[SIDE_ENEMY]) {
        // No enemy: the crew still repair, fight fires and seal breaches, intruders left aboard
        // still fight, ion wears off and the battery / cloak run out. Mind control ends with the
        // ship that cast it.
        CrewSet* pc = c->crew[SIDE_PLAYER];
        for (int i = 0; i < pc->count; i++) {
            Crew* m = &pc->crew[i];
            if (m->mind <= 0.0f) continue;
            m->mind = 0.0f;
            m->foreign = !m->foreign;
            m->mindBoost = 0;
            if (m->health > pc->races[m->race].maxHealth) m->health = pc->races[m->race].maxHealth;
        }
        update_systems(c, SIDE_PLAYER, dt);
        update_shields(c, SIDE_PLAYER, dt);
        update_environment(c, dt);  // asteroids, flares and the like don't need an enemy
        update_shots(c, dt);
        update_repairs(c, SIDE_PLAYER, dt);
        update_brawls(c, SIDE_PLAYER, dt);
        update_loops(c);
        return;
    }
    gFeats.fight.clock += dt;
    for (int side = 0; side < 2; side++) {
        update_ship(c, side, dt);
        update_repairs(c, side, dt);
        if (ai_controlled(c, c->ship[side])) update_ai_repairs(c->ship[side], dt);
        if (c->shieldFlash[side] > 0.0f) c->shieldFlash[side] -= dt;
    }
    bool repairing = false;
    for (int i = 0; i < c->crew[SIDE_PLAYER]->count && !repairing; i++) repairing = c->crew[SIDE_PLAYER]->crew[i].repairing;
    sound_loop("repair", repairing);
    sound_loop("fire", life_ship_fires(c->ship[SIDE_PLAYER]) > 0);
    // Looping sounds in sounds.xml, like fire: punching while any fight goes on, the extinguisher
    // while our crew fight a fire.
    sound_loop("brawling", sFistFight);
    bool extinguishing = false;
    for (int i = 0; i < c->crew[SIDE_PLAYER]->count; i++) extinguishing |= c->crew[SIDE_PLAYER]->crew[i].fighting;
    sound_loop("extinguish", extinguishing);
    sFistFight = false;
    sound_loop("airLeak", life_ship_breaches(c->ship[SIDE_PLAYER]) > 0);
    if (c->result == COMBAT_FIGHTING) enemy_ai(c, dt);
    for (int side = 0; side < 2; side++) {
        update_brawls(c, side, dt);
        update_hack_mind(c, side, dt);
    }
    c->boardTimer -= dt;
    if (c->boardTimer <= 0.0f && c->result == COMBAT_FIGHTING) {
        c->boardTimer = 1.0f;
        for (int side = 0; side < 2; side++) {
            boarder_ai(c, side);
            crew_drone_ai(c, side);
        }
        enemy_special_ai(c);
        defender_ai(c, SIDE_ENEMY);
        enemy_teleport_ai(c);
    }
    update_beams(c, dt);
    update_drones(c, dt);

    update_environment(c, dt);
    update_boss(c, dt);
    update_shots(c, dt);

    if (c->result == COMBAT_FIGHTING) {
        // All crew dead also ends the fight: the enemy's (if it had any) is a win without the
        // explosion; ours, wherever they are, is a loss.
        const Ship* es = c->ship[SIDE_ENEMY];
        if (es->hullHp > 0 && es->bp->crewKinds > 0 && c->enemyHadCrew && !c->bossStage && crew_owned_by(c, SIDE_ENEMY) == 0) {
            c->result = COMBAT_WON;
            c->crewKilled = true;
            // Our boarders come home; boarding drones are left behind.
            CrewSet* away = c->crew[SIDE_ENEMY];
            for (int i = away->count - 1; i >= 0; i--) {
                if (away->crew[i].drone) {
                    crew_remove(away, i);
                    continue;
                }
                int tiles[8][2], n = 0;
                for (int r = 0; n == 0 && r < c->ship[SIDE_PLAYER]->roomCount; r++)
                    n = free_tiles(c->ship[SIDE_PLAYER], c->crew[SIDE_PLAYER], r, tiles, 1, false);
                if (n) crew_transfer(away, i, c->crew[SIDE_PLAYER], tiles[0][0], tiles[0][1]);
            }
            return;
        }
        if (crew_owned_by(c, SIDE_PLAYER) == 0 && c->crew[SIDE_PLAYER]->cloneCount == 0) {
            c->result = COMBAT_LOST;
            return;
        }
        int dead = c->ship[SIDE_ENEMY]->hullHp <= 0 ? SIDE_ENEMY : c->ship[SIDE_PLAYER]->hullHp <= 0 ? SIDE_PLAYER : -1;
        if (dead >= 0) {
            c->result = dead == SIDE_ENEMY ? COMBAT_WON : COMBAT_LOST;
            sound_play(dead == SIDE_ENEMY ? "explosion1" : "explosion2");
            // The ship goes up in a burst of explosions across its rooms, and breaks apart.
            Ship* s = c->ship[dead];
            s->deathTime = 0.0f;
            if (dead == SIDE_ENEMY) {
                // Our crew still aboard go down with it: lost, but a clone bay can bring them back.
                CrewSet* away = c->crew[SIDE_ENEMY];
                CrewSet* home = c->crew[SIDE_PLAYER];
                Ship* ps = c->ship[SIDE_PLAYER];
                int cb = ship_find_system(ps, SYS_CLONEBAY);
                bool clone = cb >= 0 && (ship_system_capacity(ps, cb) > 0 || ps->aug[AUG_BACKUP_DNA]);
                for (int i = 0; i < away->count; i++) {
                    const Crew* m = &away->crew[i];
                    if (m->drone || !crew_is_ours(c, SIDE_ENEMY, m)) continue;
                    gFeats.fight.ourCrewLost++;
                    gFeats.ourDeaths++;
                    int race = crew_race_look(home, away->races[m->race].name, away->races[m->race].look);
                    if (!clone || race < 0 || home->cloneCount >= MAX_CREW) continue;
                    home->cloneQueue[home->cloneCount].race = race;
                    home->cloneQueue[home->cloneCount].female = m->female;
                    memcpy(home->cloneQueue[home->cloneCount].name, m->name, sizeof(home->cloneQueue[0].name));
                    memcpy(home->cloneQueue[home->cloneCount].xp, m->xp, sizeof(m->xp));
                    home->cloneCount++;
                }
            }
            c->crew[dead]->count = 0;
            for (int i = 0; i < s->gibCount; i++) {
                Gib* g = &s->gibs[i];
                float r1 = rand() / (float)RAND_MAX, r2 = rand() / (float)RAND_MAX, r3 = rand() / (float)RAND_MAX;
                g->v = g->vmin + (g->vmax - g->vmin) * r1;
                g->dir = g->dmin + (g->dmax - g->dmin) * r2;
                g->spin = g->amin + (g->amax - g->amin) * r3;
            }
            for (int i = 0; i < 8; i++) {
                float x, y;
                room_point(s, rand() % s->roomCount, &x, &y);
                add_effect(c, dead, FX_EXPLOSION, x, y);
            }
        }
    } else {
        c->resultTime += dt;
        for (int side = 0; side < 2; side++)
            if (c->ship[side]->deathTime >= 0.0f) c->ship[side]->deathTime += dt;
    }
}
