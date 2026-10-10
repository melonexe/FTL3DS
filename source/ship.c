#include "ship.h"

#include "items.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "xml.h"

const char* const AUG_NAMES[AUG_COUNT] = {
    "AUTO_COOLDOWN", "SHIELD_RECHARGE", "FTL_BOOSTER", "ION_ARMOR", "EXPLOSIVE_REPLICATOR", "WEAPON_PREIGNITE",
    "O2_MASKS", "BATTERY_BOOSTER", "DRONE_RECOVERY", "CLOAK_FIRE", "DEFENSE_SCRAMBLER", "HACKING_STUN",
    "ZOLTAN_BYPASS", "FIRE_EXTINGUISHERS", "BACKUP_DNA", "TELEPORT_HEAL", "FTL_JUMPER", "FLEET_DISTRACTION",
    "FTL_JAMMER", "LIFE_SCANNER", "ADV_SCANNERS", "CRYSTAL_SHARDS", "NANO_MEDBAY", "DRONE_SPEED", "CREW_STIMS",
    "ROCK_ARMOR", "SLUG_GEL", "SYSTEM_CASING", "ENERGY_SHIELD"};

// ---- Blueprints --------------------------------------------------------------

static void parse_crew(ShipBlueprint* bp, const char* body, const char* limit) {
    static const char* const stations[4] = {"pilot", "engines", "weapons", "shields"};
    const char* end;
    const char* tag;
    for (int i = 0; i < 4; i++) {
        bp->stationRooms[i] = -1;
        if ((tag = xml_find_tag_before(body, limit, stations[i], &end)))
            xml_attr_int(tag, end, "room", &bp->stationRooms[i]);
    }
    bp->crewKinds = 0;
    while (bp->crewKinds < MAX_CREW_KINDS && (tag = xml_find_tag_before(body, limit, "crewCount", &end)) != NULL) {
        body = end + 1;
        CrewCount* c = &bp->crew[bp->crewKinds];
        if (!xml_attr(tag, end, "class", c->race, sizeof(c->race)) || !xml_attr_int(tag, end, "amount", &c->amount))
            continue;
        c->max = c->amount;
        xml_attr_int(tag, end, "max", &c->max);
        if (c->amount > 0) bp->crewKinds++;
    }
}

// Reads <systemList>: one child tag per system, optionally containing a <slot>.
static void parse_systems(ShipBlueprint* bp, const char* body, const char* limit) {
    bp->systemCount = bp->optionalCount = 0;
    const char* end;
    const char* t0;
    char buf0[8];
    bp->weaponSlots = 4;
    bp->droneSlots = 2;
    if ((t0 = xml_find_tag_before(body, limit, "weaponSlots", &end)) && xml_text(end, buf0, sizeof(buf0))) bp->weaponSlots = atoi(buf0);
    if ((t0 = xml_find_tag_before(body, limit, "droneSlots", &end)) && xml_text(end, buf0, sizeof(buf0))) bp->droneSlots = atoi(buf0);
    if (bp->weaponSlots > MAX_WEAPON_SLOTS) bp->weaponSlots = MAX_WEAPON_SLOTS;
    if (bp->droneSlots > MAX_WEAPON_SLOTS) bp->droneSlots = MAX_WEAPON_SLOTS;
    const char* list = xml_find_tag_before(body, limit, "systemList", &end);
    if (!list) return;
    const char* listEnd = strstr(end, "</systemList>");
    if (!listEnd || listEnd > limit) return;

    for (const char* p = end + 1; (p = strchr(p, '<')) != NULL && p < listEnd;) {
        if (!strncmp(p, "<!--", 4)) {
            const char* c = strstr(p, "-->");
            p = c ? c + 3 : listEnd;
            continue;
        }
        char name[16];
        size_t n = strcspn(p + 1, " \t\r\n/>");
        const char* tagEnd = strchr(p, '>');
        if (!tagEnd || n == 0 || n >= sizeof(name) || p[1] == '/') {
            p++;
            continue;
        }
        memcpy(name, p + 1, n);
        name[n] = '\0';
        int type = system_from_name(name);
        // The tag's own body ends at its closing tag, or right away if self-closing.
        char close[24];
        snprintf(close, sizeof(close), "</%s>", name);
        const char* bodyEnd = tagEnd[-1] == '/' ? tagEnd : strstr(tagEnd, close);
        if (!bodyEnd || bodyEnd > listEnd) bodyEnd = listEnd;

        char start[8] = "true";
        xml_attr(p, tagEnd, "start", start, sizeof(start));
        bool starts = !strcmp(start, "true");
        if (type >= 0 && (starts ? bp->systemCount : bp->optionalCount) < MAX_SYSTEMS) {
            SystemDef* d = starts ? &bp->systems[bp->systemCount] : &bp->optional[bp->optionalCount];
            memset(d, 0, sizeof(*d));
            d->type = type;
            if (xml_attr_int(p, tagEnd, "power", &d->level) && xml_attr_int(p, tagEnd, "room", &d->roomId)) {
                xml_attr(p, tagEnd, "img", d->img, sizeof(d->img));
                xml_attr(p, tagEnd, "weapon", d->weapon, sizeof(d->weapon));
                xml_attr_int(p, tagEnd, "max", &d->max);
                system_default_slot(type, &d->slot, &d->slotDir);
                const char* e;
                const char* t;
                char buf[16];
                if (system_is_mannable(type) && xml_find_tag_before(tagEnd, bodyEnd, "slot", &e)) {
                    if ((t = xml_find_tag_before(tagEnd, bodyEnd, "number", &e)) && xml_text(e, buf, sizeof(buf)))
                        d->slot = atoi(buf);
                    if ((t = xml_find_tag_before(tagEnd, bodyEnd, "direction", &e)) && xml_text(e, buf, sizeof(buf))) {
                        static const char* const dirs[4] = {"down", "right", "up", "left"};  // DIR_* order
                        for (int i = 0; i < 4; i++)
                            if (!strcmp(buf, dirs[i])) d->slotDir = i;
                    }
                }
                if (starts)
                    bp->systemCount++;
                else
                    bp->optionalCount++;
            }
        }
        p = bodyEnd + 1;
    }

    bp->reactor = 8;
    const char* t = xml_find_tag_before(listEnd, limit, "maxPower", &end);
    if (t) xml_attr_int(t, end, "amount", &bp->reactor);
    bp->zoltanShield = false;
    bp->augCount = 0;
    for (const char* q = body; (t = xml_find_tag_before(q, limit, "aug", &end)) != NULL; q = end + 1) {
        char aug[32];
        if (!xml_attr(t, end, "name", aug, sizeof(aug))) continue;
        if (!strcmp(aug, "ENERGY_SHIELD")) bp->zoltanShield = true;
        if (bp->augCount < 3) snprintf(bp->augs[bp->augCount++], sizeof(bp->augs[0]), "%s", aug);
    }
    bp->classId[0] = '\0';
    if ((t = xml_find_tag_before(body, limit, "class", &end))) xml_attr(t, end, "id", bp->classId, sizeof(bp->classId));
    bp->health = 30;
    if ((t = xml_find_tag_before(body, limit, "health", &end))) xml_attr_int(t, end, "amount", &bp->health);

    // Drones, the same way.
    bp->droneCount = 0;
    bp->droneParts = 0;
    bp->droneLoad[0] = '\0';
    if ((t = xml_find_tag_before(body, limit, "droneList", &end))) {
        xml_attr_int(t, end, "drones", &bp->droneParts);
        xml_attr(t, end, "load", bp->droneLoad, sizeof(bp->droneLoad));
        const char* dl = end[-1] == '/' ? end : strstr(end, "</droneList>");
        const char* q = end;
        const char* e;
        const char* dn;
        while (dl && bp->droneCount < MAX_WEAPON_SLOTS && (dn = xml_find_tag_before(q, dl, "drone", &e))) {
            q = e + 1;
            if (xml_attr(dn, e, "name", bp->drones[bp->droneCount], sizeof(bp->drones[0]))) bp->droneCount++;
        }
    }

    // Weapons: either named <weapon> children (player ships) or a random list (enemies).
    bp->weaponCount = 0;
    bp->missiles = 0;
    bp->weaponLoad[0] = '\0';
    if ((t = xml_find_tag_before(body, limit, "weaponList", &end))) {
        xml_attr_int(t, end, "missiles", &bp->missiles);
        xml_attr(t, end, "load", bp->weaponLoad, sizeof(bp->weaponLoad));
        const char* wl = end[-1] == '/' ? end : strstr(end, "</weaponList>");
        const char* q = end;
        const char* e;
        const char* w;
        while (wl && bp->weaponCount < MAX_WEAPON_SLOTS && (w = xml_find_tag_before(q, wl, "weapon", &e))) {
            q = e + 1;
            if (xml_attr(w, e, "name", bp->weapons[bp->weaponCount], sizeof(bp->weapons[0]))) bp->weaponCount++;
        }
    }
}

static int scan_blueprints(FtlDat* dat, const char* file, ShipBlueprint* out, int count, int max) {
    char* xml = (char*)ftldat_read(dat, file, NULL);
    if (!xml) return count;

    const char* p = xml;
    const char* end;
    const char* tag;
    while ((tag = xml_find_tag(p, "shipBlueprint", &end)) != NULL) {
        p = end + 1;
        ShipBlueprint bp;
        memset(&bp, 0, sizeof(bp));
        if (!xml_attr(tag, end, "name", bp.name, sizeof(bp.name)) ||
            !xml_attr(tag, end, "layout", bp.layout, sizeof(bp.layout)) ||
            !xml_attr(tag, end, "img", bp.img, sizeof(bp.img)))
            continue;
        if (strncmp(bp.name, "PLAYER_SHIP_", 12) != 0 || strcmp(bp.name, "PLAYER_SHIP_TUTORIAL") == 0) continue;
        // PLAYER_SHIP_EASY is an old, untranslated Kestrel; PLAYER_SHIP_HARD is the real one.
        char noloc[8];
        if (xml_attr(tag, end, "NOLOC", noloc, sizeof(noloc)) && !strcmp(noloc, "1")) continue;
        const char* limit = strstr(p, "</shipBlueprint>");
        parse_crew(&bp, p, limit);
        parse_systems(&bp, p, limit);

        // Some blueprints are defined more than once; like the game, the last definition wins.
        // Some reference files that don't exist.
        int dup = -1;
        for (int i = 0; i < count && dup < 0; i++)
            if (strcmp(out[i].name, bp.name) == 0) dup = i;
        if (dup >= 0) {
            out[dup] = bp;
            continue;
        }
        char path[96];
        snprintf(path, sizeof(path), "data/%s.txt", bp.layout);
        if (count >= max || !ftldat_find(dat, path)) continue;

        out[count++] = bp;
    }
    free(xml);
    return count;
}

enum { CAT_OFFENSIVE, CAT_DEFENSIVE, CAT_GENERAL };

static int system_category(int type) {
    switch (type) {
        case SYS_SHIELDS: case SYS_ENGINES: case SYS_CLOAKING: return CAT_DEFENSIVE;
        case SYS_WEAPONS: case SYS_DRONES: case SYS_TELEPORTER: case SYS_ARTILLERY: return CAT_OFFENSIVE;
        default: return CAT_GENERAL;
    }
}

void ship_generate_enemy(ShipBlueprint* out, const ShipBlueprint* base, int sector) {
    *out = *base;
    // Crew: min + (max - min) / 8 * sector.
    for (int i = 0; i < out->crewKinds; i++) {
        CrewCount* c = &out->crew[i];
        c->amount = c->amount + (int)((c->max - c->amount) / 8.0f * sector);
    }
    int pools[3] = {sector + 1, sector + 2, (sector + 1) / 2 > 1 ? (sector + 1) / 2 : 1};
    // Optional systems: (sector * 10 + 20)% each, paid for from their category's pool.
    int chance = sector * 10 + 20;
    for (int i = 0; i < base->optionalCount && out->systemCount < MAX_SYSTEMS; i++) {
        if (rand() % 100 >= chance) continue;
        bool taken = false;  // its room may already hold a system
        for (int k = 0; k < out->systemCount; k++) taken |= out->systems[k].roomId == base->optional[i].roomId;
        if (taken) continue;
        out->systems[out->systemCount++] = base->optional[i];
        int cat = system_category(base->optional[i].type);
        pools[cat] -= cat == CAT_OFFENSIVE && base->optional[i].type != SYS_ARTILLERY ? 1 : 2;
    }
    // Soft caps: lerp from the starting power towards `max` by sector, plus 0-2.
    int cap[MAX_SYSTEMS];
    for (int i = 0; i < out->systemCount; i++) {
        SystemDef* d = &out->systems[i];
        if (d->level < 1) d->level = 1;
        const SysBlueprint* sb = item_system(d->type);
        int mx = d->max > 0 ? d->max : sb ? sb->maxPower : d->level;
        int limit = d->level + (int)((float)sector / 8 * (mx - d->level)) + rand() % 3;
        cap[i] = limit < mx ? limit : mx;
    }
    // Spend the pools: offensive, then defensive, then the rest (plus leftovers) on anything.
    for (int stage = 0; stage < 3; stage++) {
        int budget = stage == 2 ? pools[0] + pools[1] + pools[2] : pools[stage];
        if (stage < 2) pools[stage] = 0;
        while (budget > 0) {
            int options[MAX_SYSTEMS], n = 0;
            for (int i = 0; i < out->systemCount; i++)
                if ((stage == 2 || system_category(out->systems[i].type) == stage) && out->systems[i].level < cap[i]) options[n++] = i;
            if (!n) break;
            SystemDef* d = &out->systems[options[rand() % n]];
            d->level++;
            budget--;
            if (!system_is_subsystem(d->type)) out->reactor++;
        }
        if (stage < 2) pools[stage] = budget;  // unspent power goes to the general pool
    }
}

int ship_list_enemies(FtlDat* dat, ShipBlueprint* out, int max) {
    char* xml = (char*)ftldat_read(dat, "data/autoBlueprints.xml", NULL);
    if (!xml) return 0;
    int count = 0;
    const char* p = xml;
    const char* end;
    const char* tag;
    while (count < max && (tag = xml_find_tag(p, "shipBlueprint", &end)) != NULL) {
        p = end + 1;
        ShipBlueprint bp;
        memset(&bp, 0, sizeof(bp));
        if (!xml_attr(tag, end, "name", bp.name, sizeof(bp.name)) ||
            !xml_attr(tag, end, "layout", bp.layout, sizeof(bp.layout)) ||
            !xml_attr(tag, end, "img", bp.img, sizeof(bp.img)))
            continue;
        if (strstr(bp.name, "BOSS") || strstr(bp.name, "FLAGSHIP")) continue;
        const char* limit = strstr(p, "</shipBlueprint>");
        parse_crew(&bp, p, limit);
        parse_systems(&bp, p, limit);
        bp.enemy = true;
        char path[96];
        snprintf(path, sizeof(path), "data/%s.txt", bp.layout);
        if (!ftldat_find(dat, path)) continue;
        out[count++] = bp;
    }
    free(xml);
    return count;
}

bool ship_find_boss(FtlDat* dat, const char* name, ShipBlueprint* out) {
    char* xml = (char*)ftldat_read(dat, "data/bosses.xml", NULL);
    if (!xml) return false;
    bool found = false;
    const char* p = xml;
    const char* end;
    const char* tag;
    while (!found && (tag = xml_find_tag(p, "shipBlueprint", &end)) != NULL) {
        p = end + 1;
        ShipBlueprint bp;
        memset(&bp, 0, sizeof(bp));
        if (!xml_attr(tag, end, "name", bp.name, sizeof(bp.name)) || strcmp(bp.name, name) ||
            !xml_attr(tag, end, "layout", bp.layout, sizeof(bp.layout)) || !xml_attr(tag, end, "img", bp.img, sizeof(bp.img)))
            continue;
        const char* limit = strstr(p, "</shipBlueprint>");
        parse_crew(&bp, p, limit);
        parse_systems(&bp, p, limit);
        bp.enemy = true;
        *out = bp;
        found = true;
    }
    free(xml);
    return found;
}

int ship_list_blueprints(FtlDat* dat, ShipBlueprint* out, int max) {
    int n = scan_blueprints(dat, "data/blueprints.xml", out, 0, max);
    n = scan_blueprints(dat, "data/dlcBlueprints.xml", out, n, max);
    return scan_blueprints(dat, "data/dlcBlueprintsOverwrite.xml", out, n, max);
}

// ---- Layout ------------------------------------------------------------------

static bool parse_layout_txt(Ship* s, char* text) {
    // Whitespace-separated keywords, each followed by a fixed number of integers.
    int vals[5];
    char* save = NULL;
    for (char* tok = strtok_r(text, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        int n;
        if (!strcmp(tok, "X_OFFSET") || !strcmp(tok, "Y_OFFSET") || !strcmp(tok, "VERTICAL") ||
            !strcmp(tok, "HORIZONTAL"))
            n = 1;
        else if (!strcmp(tok, "ELLIPSE"))
            n = 4;
        else if (!strcmp(tok, "ROOM") || !strcmp(tok, "DOOR"))
            n = 5;
        else
            continue;  // unknown keyword or stray number

        for (int i = 0; i < n; i++) {
            char* v = strtok_r(NULL, " \t\r\n", &save);
            if (!v) return false;
            vals[i] = atoi(v);
        }

        if (!strcmp(tok, "X_OFFSET")) s->xOffset = vals[0];
        else if (!strcmp(tok, "Y_OFFSET")) s->yOffset = vals[0];
        else if (!strcmp(tok, "VERTICAL")) s->vertical = vals[0];
        else if (!strcmp(tok, "HORIZONTAL")) s->horizontal = vals[0];
        else if (!strcmp(tok, "ELLIPSE")) memcpy(s->ellipse, vals, sizeof(s->ellipse));
        else if (!strcmp(tok, "ROOM") && s->roomCount < MAX_ROOMS)
            s->rooms[s->roomCount++] = (Room){vals[0], vals[1], vals[2], vals[3], vals[4]};
        else if (!strcmp(tok, "DOOR") && s->doorCount < MAX_DOORS)
            s->doors[s->doorCount++] = (Door){vals[0], vals[1], vals[2], vals[3], vals[4] != 0};
    }
    return s->roomCount > 0;
}

static void parse_layout_xml(Ship* s, const char* xml) {
    const char* end;
    const char* tag;
    const char* p = xml;
    char buf[8];
    while (s->mountCount < MAX_MOUNTS && (tag = xml_find_tag(p, "mount", &end))) {
        p = end + 1;
        WeaponMount* m = &s->mounts[s->mountCount];
        if (!xml_attr_int(tag, end, "x", &m->x) || !xml_attr_int(tag, end, "y", &m->y)) continue;
        m->rotate = xml_attr(tag, end, "rotate", buf, sizeof(buf)) && !strcmp(buf, "true");
        m->mirror = xml_attr(tag, end, "mirror", buf, sizeof(buf)) && !strcmp(buf, "true");
        s->mountCount++;
    }
    if ((tag = xml_find_tag(xml, "img", &end))) {
        xml_attr_int(tag, end, "x", &s->hullX);
        xml_attr_int(tag, end, "y", &s->hullY);
    }
    if ((tag = xml_find_tag(xml, "floor", &end))) {
        xml_attr_int(tag, end, "x", &s->floorX);
        xml_attr_int(tag, end, "y", &s->floorY);
    }
    // Gibs: <explosion><gib1><velocity min max/><direction .../><angular .../><x/><y/></gib1>...
    const char* ex = xml_find_tag(xml, "explosion", &end);
    const char* exEnd = ex ? strstr(end, "</explosion>") : NULL;
    for (int n = 1; exEnd && n <= MAX_GIBS; n++) {
        char name[8], close[12];
        snprintf(name, sizeof(name), "gib%d", n);
        snprintf(close, sizeof(close), "</gib%d>", n);
        const char* ge;
        const char* g = xml_find_tag_before(end, exEnd, name, &ge);
        const char* gEnd = g ? strstr(ge, close) : NULL;
        if (!g || !gEnd) break;
        Gib* gib = &s->gibs[s->gibCount];
        memset(gib, 0, sizeof(*gib));
        const char* e;
        const char* t;
        char buf[16];
        if ((t = xml_find_tag_before(ge, gEnd, "velocity", &e))) {
            xml_attr_float(t, e, "min", &gib->vmin);
            xml_attr_float(t, e, "max", &gib->vmax);
        }
        if ((t = xml_find_tag_before(ge, gEnd, "direction", &e))) {
            xml_attr_float(t, e, "min", &gib->dmin);
            xml_attr_float(t, e, "max", &gib->dmax);
        }
        if ((t = xml_find_tag_before(ge, gEnd, "angular", &e))) {
            xml_attr_float(t, e, "min", &gib->amin);
            xml_attr_float(t, e, "max", &gib->amax);
        }
        if ((t = xml_find_tag_before(ge, gEnd, "x", &e)) && xml_text(e, buf, sizeof(buf))) gib->x = atoi(buf);
        if ((t = xml_find_tag_before(ge, gEnd, "y", &e)) && xml_text(e, buf, sizeof(buf))) gib->y = atoi(buf);
        s->gibCount++;
    }
    if ((tag = xml_find_tag(xml, "cloak", &end))) {
        xml_attr_int(tag, end, "x", &s->cloakX);
        xml_attr_int(tag, end, "y", &s->cloakY);
    }
}

// Marks which tiles belong to which room and where crew can walk.
static bool build_grid(Ship* s) {
    memset(s->tileRoom, -1, sizeof(s->tileRoom));
    memset(s->open, 0, sizeof(s->open));
    memset(s->doorRight, -1, sizeof(s->doorRight));
    memset(s->doorDown, -1, sizeof(s->doorDown));
    for (int i = 0; i < s->roomCount; i++) {
        const Room* r = &s->rooms[i];
        if (r->x < 0 || r->y < 0 || r->x + r->w > GRID_MAX || r->y + r->h > GRID_MAX) return false;
        if (r->x + r->w > s->gridW) s->gridW = r->x + r->w;
        if (r->y + r->h > s->gridH) s->gridH = r->y + r->h;
        for (int y = r->y; y < r->y + r->h; y++)
            for (int x = r->x; x < r->x + r->w; x++) {
                s->tileRoom[y][x] = (signed char)i;
                if (x + 1 < r->x + r->w) s->open[y][x] |= OPEN_RIGHT;
                if (x > r->x) s->open[y][x] |= OPEN_LEFT;
                if (y + 1 < r->y + r->h) s->open[y][x] |= OPEN_DOWN;
                if (y > r->y) s->open[y][x] |= OPEN_UP;
            }
    }
    // A vertical door sits in the left wall of its tile, a horizontal one in the top wall.
    // Doors to space (airlocks) have no room on one side and stay closed to walking.
    for (int i = 0; i < s->doorCount; i++) {
        const Door* d = &s->doors[i];
        int ax = d->vertical ? d->x - 1 : d->x, ay = d->vertical ? d->y : d->y - 1;
        if (ax >= 0 && ay >= 0 && ax < GRID_MAX && ay < GRID_MAX) {
            if (d->vertical)
                s->doorRight[ay][ax] = (signed char)i;
            else
                s->doorDown[ay][ax] = (signed char)i;
        }
        if (ship_room_at(s, ax, ay) < 0 || ship_room_at(s, d->x, d->y) < 0) continue;
        if (d->vertical) {
            s->open[ay][ax] |= OPEN_RIGHT;
            s->open[d->y][d->x] |= OPEN_LEFT;
        } else {
            s->open[ay][ax] |= OPEN_DOWN;
            s->open[d->y][d->x] |= OPEN_UP;
        }
    }
    return true;
}

int ship_door_between(const Ship* s, int ax, int ay, int bx, int by) {
    if (bx < ax || by < ay) return ship_door_between(s, bx, by, ax, ay);
    if (ax < 0 || ay < 0 || ax >= GRID_MAX || ay >= GRID_MAX) return -1;
    if (bx == ax + 1 && by == ay) return s->doorRight[ay][ax];
    if (bx == ax && by == ay + 1) return s->doorDown[ay][ax];
    return -1;
}

bool ship_door_is_airlock(const Ship* s, int door) {
    const Door* d = &s->doors[door];
    int ax = d->vertical ? d->x - 1 : d->x, ay = d->vertical ? d->y : d->y - 1;
    return ship_room_at(s, ax, ay) < 0 || ship_room_at(s, d->x, d->y) < 0;
}

int ship_find_system(const Ship* s, int type) {
    for (int i = 0; i < s->systemCount; i++)
        if (s->systems[i].def->type == type) return i;
    return -1;
}

int ship_room_at(const Ship* s, int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= GRID_MAX || ty >= GRID_MAX) return -1;
    return s->tileRoom[ty][tx];
}

static void load_texture(FtlDat* dat, Texture* t, const char* path) {
    uint32_t size;
    uint8_t* png = ftldat_read(dat, path, &size);
    if (png) texture_load_png(t, png, size);
    free(png);
}

// At half size (2x2 pixels averaged), smoothly filtered.
static void load_texture_half(FtlDat* dat, Texture* t, const char* path) {
    uint32_t size;
    uint8_t* png = ftldat_read(dat, path, &size);
    if (png && size >= 24) {
        int w = (png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19];  // PNG IHDR
        int h = (png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23];
        if (texture_load_png_fit(t, png, size, w / 2 > 0 ? w / 2 : 1, h / 2 > 0 ? h / 2 : 1)) texture_set_smooth(t);
    }
    free(png);
}

void ship_load_gibs(Ship* s) {
    if (s->gibsLoaded || !s->dat) return;
    s->gibsLoaded = true;
    char path[96];
    for (int i = 0; i < s->gibCount; i++) {
        snprintf(path, sizeof(path), "img/ship/%s_gib%d.png", s->bp->img, i + 1);
        if (!ftldat_find(s->dat, path)) snprintf(path, sizeof(path), "img/ships_glow/%s_gib%d.png", s->bp->img, i + 1);
        load_texture(s->dat, &s->gibs[i].tex, path);
    }
}

void ship_load_cloak(Ship* s) {
    if (s->cloakTried || !s->dat) return;
    s->cloakTried = true;
    char path[96];
    snprintf(path, sizeof(path), "img/ship/%s_cloak.png", s->bp->img);
    if (ftldat_find(s->dat, path)) load_texture(s->dat, &s->cloakImage, path);
}

bool ship_load_layout(Ship* s, FtlDat* dat, const ShipBlueprint* bp) {
    memset(s, 0, sizeof(*s));
    s->bp = bp;
    char path[96];
    snprintf(path, sizeof(path), "data/%s.txt", bp->layout);
    char* txt = (char*)ftldat_read(dat, path, NULL);
    bool ok = txt && parse_layout_txt(s, txt);
    free(txt);
    if (!ok) return false;
    snprintf(path, sizeof(path), "data/%s.xml", bp->layout);
    char* xml = (char*)ftldat_read(dat, path, NULL);
    if (xml) parse_layout_xml(s, xml);
    free(xml);
    return true;
}

bool ship_load(Ship* s, FtlDat* dat, const ShipBlueprint* bp, char* err, size_t errlen) {
    memset(s, 0, sizeof(*s));
    s->bp = bp;
    char path[96];

    snprintf(path, sizeof(path), "data/%s.txt", bp->layout);
    char* txt = (char*)ftldat_read(dat, path, NULL);
    bool ok = txt && parse_layout_txt(s, txt);
    free(txt);
    if (!ok) {
        snprintf(err, errlen, "Bad layout %s", path);
        return false;
    }

    if (!build_grid(s)) {
        snprintf(err, errlen, "Layout %s is too large", bp->layout);
        return false;
    }

    snprintf(path, sizeof(path), "data/%s.xml", bp->layout);
    char* xml = (char*)ftldat_read(dat, path, NULL);
    if (xml) parse_layout_xml(s, xml);
    free(xml);

    snprintf(path, sizeof(path), bp->enemy ? "img/ships_glow/%s_base.png" : "img/ship/%s_base.png", bp->img);
    load_texture(dat, &s->hull, path);
    if (!bp->enemy) {
        snprintf(path, sizeof(path), "img/ship/%s_floor.png", bp->img);
        load_texture(dat, &s->floor, path);
    }
    // Shield bubble: the ship's own image (kestral_2 uses kestral's), or the generic enemy one. It's
    // a soft glow, so half resolution with smooth filtering looks the same at a quarter the memory.
    if (bp->enemy) {
        load_texture_half(dat, &s->shieldImage, "img/ship/enemy_shields.png");
    } else {
        char base[48];
        snprintf(base, sizeof(base), "%s", bp->img);
        for (int tries = 0; tries < 2 && !s->shieldImage.loaded; tries++) {
            snprintf(path, sizeof(path), "img/ship/%s_shields1.png", base);
            if (ftldat_find(dat, path)) load_texture_half(dat, &s->shieldImage, path);
            char* us = strrchr(base, '_');
            if (us && us[1] >= '0' && us[1] <= '9') *us = '\0';
        }
    }
    s->maxHull = s->hullHp = bp->health;
    s->superShield = s->superShieldMax = bp->zoltanShield ? 5 : 0;
    s->missiles = bp->missiles;
    load_texture(dat, &s->doorSheet, "img/effects/door_sheet.png");
    s->deathTime = -1.0f;
    s->hackedSys = -1;
    s->dat = dat;  // gibs and the cloak glow load later (ship_load_gibs / ship_load_cloak)
    for (int i = 0; i < s->roomCount; i++) s->oxygen[i] = 100.0f;

    // Systems: subsystems run at their full level; reactor systems are powered in blueprint
    // order while bars last. Weapons and drones start off, as they power individual items.
    s->reactor = bp->reactor;
    s->weaponSlots = bp->weaponSlots;
    s->droneSlots = bp->droneSlots;
    for (int i = 0; i < bp->systemCount; i++) ship_install_system(s, dat, &bp->systems[i]);
    return true;
}

int ship_install_system(Ship* s, FtlDat* dat, const SystemDef* src) {
    char path[96];
    if (s->systemCount >= MAX_SYSTEMS) return -1;
    {
        int idx = s->systemCount;
        s->sysDefs[idx] = *src;
        const SystemDef* d = &s->sysDefs[idx];
        ShipSystem* sys = &s->systems[idx];
        memset(sys, 0, sizeof(*sys));
        sys->def = d;
        sys->room = -1;
        for (int r = 0; r < s->roomCount; r++)
            if (s->rooms[r].id == d->roomId) sys->room = r;
        if (sys->room < 0) return -1;

        sys->stationX = sys->stationY = -1;
        sys->ionLimit = -1;
        const Room* room = &s->rooms[sys->room];
        if (d->slot >= 0 && d->slot < room->w * room->h) {
            sys->stationX = room->x + d->slot % room->w;
            sys->stationY = room->y + d->slot / room->w;
        }
        if (d->img[0]) {
            snprintf(path, sizeof(path), "img/ship/interior/%s.png", d->img);
            load_texture(dat, &sys->interior, path);
        }
        snprintf(path, sizeof(path), "img/icons/s_%s_overlay.png", system_name(d->type));
        load_texture(dat, &sys->icon, path);
        snprintf(path, sizeof(path), "img/icons/s_%s_green1.png", system_name(d->type));
        load_texture(dat, &sys->panelOn, path);
        {
            // The panel icons pad a small symbol into 64x64; find the symbol so it can be scaled up.
            uint32_t size;
            uint8_t* png = ftldat_read(dat, path, &size);
            int* b = sys->iconBounds;
            if (!png || !sys->panelOn.loaded ||
                !texture_png_alpha_bounds(png, size, 0, 0, sys->panelOn.width, sys->panelOn.height, &b[0], &b[1], &b[2], &b[3])) {
                b[0] = b[1] = 0;
                b[2] = b[3] = 63;
            }
            free(png);
        }
        snprintf(path, sizeof(path), "img/icons/s_%s_grey1.png", system_name(d->type));
        load_texture(dat, &sys->panelOff, path);
        s->systemCount++;

        if (system_is_subsystem(d->type))
            sys->power = d->level;
        else if (d->type != SYS_WEAPONS && d->type != SYS_DRONES)
            ship_set_power(s, s->systemCount - 1, d->level);
        return idx;
    }
}

int ship_replace_system(Ship* s, FtlDat* dat, int idx, const SystemDef* src) {
    if (idx < 0 || idx >= s->systemCount) return -1;
    bool roomOk = false;
    for (int r = 0; r < s->roomCount; r++) roomOk |= s->rooms[r].id == src->roomId;
    if (!roomOk) return -1;
    ShipSystem* old = &s->systems[idx];
    texture_free(&old->interior);
    texture_free(&old->icon);
    texture_free(&old->panelOn);
    texture_free(&old->panelOff);
    SystemDef def = *src;
    def.level = old->def->level;  // the new system keeps the old one's upgrades (wiki: Clone Bay)
    // Install over the old entry: ship_install_system fills slot `systemCount`.
    int count = s->systemCount;
    s->systemCount = idx;
    int got = ship_install_system(s, dat, &def);
    s->systemCount = count;
    // It powered up seeing only the systems before it: take that power again against them all.
    if (got >= 0 && !system_is_subsystem(def.type) && def.type != SYS_WEAPONS && def.type != SYS_DRONES) {
        int want = s->systems[got].power;
        s->systems[got].power = 0;
        ship_set_power(s, got, want);
    }
    return got;
}

void ship_free(Ship* s) {
    texture_free(&s->hull);
    texture_free(&s->floor);
    texture_free(&s->doorSheet);
    texture_free(&s->shieldImage);
    texture_free(&s->cloakImage);
    for (int i = 0; i < s->gibCount; i++) texture_free(&s->gibs[i].tex);
    s->gibCount = 0;
    for (int i = 0; i < s->weaponCount; i++) {
        texture_free(&s->weapons[i].art);
        texture_free(&s->weapons[i].proj);
    }
    for (int i = 0; i < s->droneCount; i++) {
        texture_free(&s->drones[i].base);
        texture_free(&s->drones[i].on);
        texture_free(&s->drones[i].charged);
        texture_free(&s->drones[i].proj);
    }
    s->droneCount = 0;
    s->weaponCount = 0;
    for (int k = 0; k < s->artilleryCount; k++) {
        texture_free(&s->artillery[k].art);
        texture_free(&s->artillery[k].proj);
    }
    s->artilleryCount = 0;
    for (int i = 0; i < s->systemCount; i++) {
        texture_free(&s->systems[i].interior);
        texture_free(&s->systems[i].icon);
        texture_free(&s->systems[i].panelOn);
        texture_free(&s->systems[i].panelOff);
    }
    s->systemCount = 0;
}

int ship_system_in_room(const Ship* s, int room) {
    for (int i = 0; i < s->systemCount; i++)
        if (s->systems[i].room == room) return i;
    return -1;
}

float ship_artillery_time(const Ship* s, int k) {
    if (k < 0 || k >= s->artilleryCount || !s->artillery[k].def) return 0.0f;
    int p = ship_system_power(s, s->artSys[k]);
    if (p <= 0) return 0.0f;
    return s->artillery[k].def->cooldown * (1.5f - 0.25f * (p < 4 ? p : 4));
}

int ship_artillery_index(const Ship* s, int sys) {
    for (int k = 0; k < s->artilleryCount; k++)
        if (s->artSys[k] == sys) return k;
    return -1;
}

int ship_system_power(const Ship* s, int sys) {
    if (sys < 0) return 0;
    const ShipSystem* y = &s->systems[sys];
    if (s->hackPulse > 0.0f && s->hackedSys == sys && y->def->type != SYS_SHIELDS && y->def->type != SYS_WEAPONS) return 0;
    int p = y->power + y->zoltan, cap = ship_system_capacity(s, sys);
    return p < cap ? p : cap;
}

int ship_reactor_free(const Ship* s) {
    int used = 0;
    for (int i = 0; i < s->systemCount; i++)
        if (!system_is_subsystem(s->systems[i].def->type)) used += s->systems[i].power;
    return s->reactor - s->reactorCut + s->batteryBonus - used;
}

int ship_system_ion(const Ship* s, int sys) {
    const ShipSystem* y = &s->systems[sys];
    int undamaged = y->def->level - y->damage;
    return y->ionLimit >= 0 && y->ionLimit < undamaged ? undamaged - y->ionLimit : 0;
}

int ship_system_capacity(const Ship* s, int sys) {
    const ShipSystem* y = &s->systems[sys];
    int c = y->def->level - y->damage;
    if (y->ionLimit >= 0 && y->ionLimit < c) c = y->ionLimit;
    return c > 0 ? c : 0;
}

void ship_set_power(Ship* s, int sys, int power) {
    ShipSystem* y = &s->systems[sys];
    if (system_is_subsystem(y->def->type)) return;
    if (y->ionTimer > 0.0f) return;  // ion locks the system's power
    if (power < 0) power = 0;
    int cap = ship_system_capacity(s, sys);
    if (power > cap) power = cap;
    int avail = y->power + ship_reactor_free(s);
    y->power = power < avail ? power : avail;
    y->targetPower = y->power;
}

void ship_bounds(const Ship* s, float* x0, float* y0, float* x1, float* y1) {
    float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    *x0 = *y0 = 1e9f;
    *x1 = *y1 = -1e9f;
    for (int i = 0; i < s->roomCount; i++) {
        const Room* r = &s->rooms[i];
        float rx = ox + r->x * TILE, ry = oy + r->y * TILE;
        if (rx < *x0) *x0 = rx;
        if (ry < *y0) *y0 = ry;
        if (rx + r->w * TILE > *x1) *x1 = rx + r->w * TILE;
        if (ry + r->h * TILE > *y1) *y1 = ry + r->h * TILE;
    }
    if (s->hull.loaded) {
        float hx = ox + s->hullX, hy = oy + s->hullY;
        if (hx < *x0) *x0 = hx;
        if (hy < *y0) *y0 = hy;
        if (hx + s->hull.width > *x1) *x1 = hx + s->hull.width;
        if (hy + s->hull.height > *y1) *y1 = hy + s->hull.height;
    }
}
