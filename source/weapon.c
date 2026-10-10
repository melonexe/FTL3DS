#include "weapon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text.h"
#include "xml.h"

#define MAX_WEAPONS 160
#define MAX_LISTS 48
#define MAX_LIST_NAMES 24

typedef struct {
    char name[32];
    char items[MAX_LIST_NAMES][32];
    int count;
} BlueprintList;

static WeaponDef sWeapons[MAX_WEAPONS];
static int sWeaponCount;

#define MAX_DRONES 40
static DroneDef sDrones[MAX_DRONES];
static char sDroneWeapon[MAX_DRONES][32];
static int sDroneCount;
static BlueprintList sLists[MAX_LISTS];
static int sListCount;

static int child_int(const char* body, const char* limit, const char* tag, int fallback) {
    const char* e;
    char buf[16];
    if (xml_find_tag_before(body, limit, tag, &e) && xml_text(e, buf, sizeof(buf))) return atoi(buf);
    return fallback;
}

static void child_str(const char* body, const char* limit, const char* tag, char* out, size_t outlen) {
    const char* e;
    out[0] = '\0';
    if (xml_find_tag_before(body, limit, tag, &e)) xml_text(e, out, outlen);
}

// <launchSounds><sound>a</sound>...</launchSounds>
static int child_sounds(const char* body, const char* limit, const char* tag, char out[][24]) {
    const char* e;
    const char* t = xml_find_tag_before(body, limit, tag, &e);
    if (!t) return 0;
    char close[32];
    snprintf(close, sizeof(close), "</%s>", tag);
    const char* stop = strstr(e, close);
    if (!stop || stop > limit) return 0;
    int n = 0;
    const char* p = e;
    while (n < MAX_WEAPON_SOUNDS && (t = xml_find_tag_before(p, stop, "sound", &e)) != NULL) {
        p = e + 1;
        if (xml_text(e, out[n], 24)) n++;
    }
    return n;
}

static int type_from(const char* s) {
    if (!strcmp(s, "LASER")) return WEAPON_LASER;
    if (!strcmp(s, "MISSILES")) return WEAPON_MISSILES;
    if (!strcmp(s, "BEAM")) return WEAPON_BEAM;
    if (!strcmp(s, "BOMB")) return WEAPON_BOMB;
    if (!strcmp(s, "BURST")) return WEAPON_BURST;  // flak
    return WEAPON_OTHER;
}

static void scan(const char* xml) {
    const char* p = xml;
    const char* end;
    const char* tag;
    while ((tag = xml_find_tag(p, "weaponBlueprint", &end)) != NULL) {
        p = end + 1;
        const char* limit = strstr(p, "</weaponBlueprint>");
        if (!limit) break;
        char name[32];
        if (!xml_attr(tag, end, "name", name, sizeof(name))) continue;

        // Later definitions replace earlier ones, as in the game.
        WeaponDef* w = NULL;
        for (int i = 0; i < sWeaponCount && !w; i++)
            if (!strcmp(sWeapons[i].name, name)) w = &sWeapons[i];
        if (!w) {
            if (sWeaponCount >= MAX_WEAPONS) continue;
            w = &sWeapons[sWeaponCount++];
        }
        memset(w, 0, sizeof(*w));
        snprintf(w->name, sizeof(w->name), "%s", name);
        char buf[32];
        child_str(p, limit, "type", buf, sizeof(buf));
        w->type = type_from(buf);
        w->damage = child_int(p, limit, "damage", 0);
        w->ion = child_int(p, limit, "ion", 0);
        if (w->type == WEAPON_LASER && w->damage == 0 && w->ion > 0) w->type = WEAPON_ION;
        w->shots = child_int(p, limit, "shots", 1);
        w->sp = child_int(p, limit, "sp", 0);
        w->power = child_int(p, limit, "power", 1);
        w->missiles = child_int(p, limit, "missiles", 0);
        w->cooldown = (float)child_int(p, limit, "cooldown", 10);
        // Default speeds per type, from Mike Hopley's measurements (mikehopley.github.io/FTL-weapons-travel).
        int defSpeed = w->type == WEAPON_MISSILES ? 35 : w->type == WEAPON_BURST ? 26 : w->type == WEAPON_ION ? 30 : 60;
        w->speed = child_int(p, limit, "speed", defSpeed);
        w->length = child_int(p, limit, "length", 60);
        w->radius = child_int(p, limit, "radius", 40);
        w->sysDamage = child_int(p, limit, "sysDamage", 0);
        w->persDamage = child_int(p, limit, "persDamage", 0);
        w->hullBust = child_int(p, limit, "hullBust", 0);
        w->chargeLevels = child_int(p, limit, "chargeLevels", 1);
        if (w->chargeLevels < 1) w->chargeLevels = 1;
        w->stun = (float)child_int(p, limit, "stun", 0);
        w->stunChance = child_int(p, limit, "stunChance", 0);
        w->lockdown = child_int(p, limit, "lockdown", 0);
        {
            const char* be;
            const char* bt = xml_find_tag_before(p, limit, "boost", &be);
            const char* bEnd = bt ? strstr(be, "</boost>") : NULL;
            if (bEnd && bEnd < limit) {
                char btype[16] = "";
                child_str(be, bEnd, "type", btype, sizeof(btype));
                if (!strcmp(btype, "cooldown")) {
                    w->boostAmount = (float)child_int(be, bEnd, "amount", 0);
                    w->boostCount = child_int(be, bEnd, "count", 0);
                }
            }
        }
        w->fireChance = child_int(p, limit, "fireChance", 0);
        w->breachChance = child_int(p, limit, "breachChance", 0);
        child_str(p, limit, "image", w->image, sizeof(w->image));
        // Flak lists its debris under <projectiles>; only non-fake ones can hit.
        {
            const char* e2;
            const char* pl = xml_find_tag_before(p, limit, "projectiles", &e2);
            const char* plEnd = pl ? strstr(e2, "</projectiles>") : NULL;
            const char* q = e2;
            const char* t2;
            while (pl && plEnd && (t2 = xml_find_tag_before(q, plEnd, "projectile", &e2)) != NULL) {
                q = e2 + 1;
                char fake[8] = "false", img[32];
                int count = 1;
                xml_attr(t2, e2, "fake", fake, sizeof(fake));
                xml_attr_int(t2, e2, "count", &count);
                if (strcmp(fake, "true") == 0) continue;
                w->flakShots += count;
                if (!w->image[0] && xml_text(e2, img, sizeof(img))) snprintf(w->image, sizeof(w->image), "%s", img);
            }
        }
        child_str(p, limit, "weaponArt", w->art, sizeof(w->art));
        child_str(p, limit, "explosion", w->explosion, sizeof(w->explosion));
        w->launchCount = child_sounds(p, limit, "launchSounds", w->launch);
        w->hitShipCount = child_sounds(p, limit, "hitShipSounds", w->hitShip);
        w->hitShieldCount = child_sounds(p, limit, "hitShieldSounds", w->hitShield);

        // Display name: <short id="..."/> looked up in the text files, or inline <short>text</short>.
        const char* e;
        const char* t = xml_find_tag_before(p, limit, "short", &e);
        char id[64] = "";
        if (t && xml_attr(t, e, "id", id, sizeof(id)))
            snprintf(w->shortName, sizeof(w->shortName), "%s", text_get(id, name));
        else if (!t || !xml_text(e, w->shortName, sizeof(w->shortName)))
            snprintf(w->shortName, sizeof(w->shortName), "%.23s", name);
        p = limit;
    }

    p = xml;
    while ((tag = xml_find_tag(p, "droneBlueprint", &end)) != NULL) {
        p = end + 1;
        const char* limit = strstr(p, "</droneBlueprint>");
        if (!limit) break;
        char name[32];
        if (!xml_attr(tag, end, "name", name, sizeof(name))) continue;
        int idx = -1;
        for (int i = 0; i < sDroneCount && idx < 0; i++)
            if (!strcmp(sDrones[i].name, name)) idx = i;
        if (idx < 0) {
            if (sDroneCount >= MAX_DRONES) continue;
            idx = sDroneCount++;
        }
        DroneDef* d = &sDrones[idx];
        memset(d, 0, sizeof(*d));
        snprintf(d->name, sizeof(d->name), "%s", name);
        char buf[32];
        child_str(p, limit, "type", buf, sizeof(buf));
        d->type = !strcmp(buf, "COMBAT")        ? DRONE_COMBAT
                  : !strcmp(buf, "DEFENSE")     ? DRONE_DEFENSE
                  : !strcmp(buf, "SHIP_REPAIR") ? DRONE_SHIP_REPAIR
                  : !strcmp(buf, "SHIELD")      ? DRONE_SHIELD
                  : !strcmp(buf, "REPAIR")      ? DRONE_CREW_REPAIR
                  : !strcmp(buf, "BATTLE")      ? DRONE_BATTLE
                  : !strcmp(buf, "BOARDER")     ? (strcmp(name, "BOARDER_ION") ? DRONE_BOARDER : DRONE_ION_INTRUDER)
                                                : DRONE_INTERNAL;
        d->power = child_int(p, limit, "power", 1);
        d->cooldown = child_int(p, limit, "cooldown", 1000) / 1000.0f;
        d->speed = child_int(p, limit, "speed", 5);
        child_str(p, limit, "target", buf, sizeof(buf));
        d->target = !strcmp(buf, "LASERS") ? DRONE_TARGET_LASERS : !strcmp(buf, "DRONES") ? DRONE_TARGET_DRONES : DRONE_TARGET_MISSILES;
        child_str(p, limit, "droneImage", d->image, sizeof(d->image));
        child_str(p, limit, "weaponBlueprint", sDroneWeapon[idx], sizeof(sDroneWeapon[idx]));
        const char* e;
        const char* t = xml_find_tag_before(p, limit, "short", &e);
        char id[64] = "";
        if (t && xml_attr(t, e, "id", id, sizeof(id)))
            snprintf(d->shortName, sizeof(d->shortName), "%s", text_get(id, name));
        else
            snprintf(d->shortName, sizeof(d->shortName), "%.23s", name);
        p = limit;
    }

    p = xml;
    while (sListCount < MAX_LISTS && (tag = xml_find_tag(p, "blueprintList", &end)) != NULL) {
        p = end + 1;
        const char* limit = strstr(p, "</blueprintList>");
        if (!limit) break;
        BlueprintList* l = &sLists[sListCount];
        if (!xml_attr(tag, end, "name", l->name, sizeof(l->name))) continue;
        l->count = 0;
        const char* q = p;
        const char* e;
        while (l->count < MAX_LIST_NAMES && xml_find_tag_before(q, limit, "name", &e)) {
            q = e + 1;
            if (xml_text(e, l->items[l->count], sizeof(l->items[0]))) l->count++;
        }
        sListCount++;
        p = limit;
    }
}

bool weapons_load(FtlDat* dat) {
    static const char* const files[] = {"data/blueprints.xml", "data/autoBlueprints.xml", "data/dlcBlueprints.xml",
                                        "data/dlcBlueprintsOverwrite.xml"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char* xml = (char*)ftldat_read(dat, files[i], NULL);
        if (xml) scan(xml);
        free(xml);
    }
    for (int i = 0; i < sDroneCount; i++) sDrones[i].weapon = sDroneWeapon[i][0] ? weapon_find(sDroneWeapon[i]) : NULL;
    return sWeaponCount > 0;
}

void weapons_free(void) {
    sWeaponCount = 0;
    sDroneCount = 0;
    sListCount = 0;
}

const WeaponDef* weapon_find(const char* name) {
    for (int i = 0; i < sWeaponCount; i++)
        if (!strcmp(sWeapons[i].name, name)) return &sWeapons[i];
    return NULL;
}

const DroneDef* drone_find(const char* name) {
    for (int i = 0; i < sDroneCount; i++)
        if (!strcmp(sDrones[i].name, name)) return &sDrones[i];
    return NULL;
}

bool drone_supported(const DroneDef* d) {
    if (d->type == DRONE_INTERNAL) return false;
    if (d->type == DRONE_COMBAT || d->type == DRONE_DEFENSE) return d->weapon != NULL;
    return true;
}

int weapon_list(const char* listName, const char** out, int max) {
    for (int i = sListCount - 1; i >= 0; i--) {  // the last definition wins
        if (strcmp(sLists[i].name, listName)) continue;
        int n = 0;
        for (int j = 0; j < sLists[i].count && n < max; j++) out[n++] = sLists[i].items[j];
        return n;
    }
    return 0;
}

bool weapon_supported(const WeaponDef* w) { return w->type != WEAPON_OTHER; }
