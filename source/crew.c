#include "crew.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xml.h"

// ---- Names -------------------------------------------------------------------

void names_load(NameList* list, FtlDat* dat) {
    memset(list, 0, sizeof(*list));
    char* xml = (char*)ftldat_read(dat, "data/names.xml", NULL);
    if (!xml) return;

    // Two passes: count, then copy.
    for (int pass = 0; pass < 2; pass++) {
        int n = 0;
        const char* p = xml;
        const char* end;
        const char* tag;
        char buf[24];
        while ((tag = xml_find_tag(p, "nameList", &end)) != NULL) {
            p = end + 1;
            if (xml_attr(tag, end, "language", buf, sizeof(buf))) continue;  // translations
            bool female = xml_attr(tag, end, "sex", buf, sizeof(buf)) && strcmp(buf, "female") == 0;
            const char* close = strstr(p, "</nameList>");
            const char* q = p;
            const char* e;
            const char* t;
            while ((t = xml_find_tag_before(q, close, "name", &e)) != NULL) {
                q = e + 1;
                if (!xml_text(e, buf, sizeof(buf)) || !buf[0]) continue;
                if (pass == 1) {
                    strcpy(list->names[n], buf);
                    list->female[n] = female;
                }
                n++;
            }
        }
        if (pass == 0) {
            list->names = malloc(sizeof(*list->names) * (n ? n : 1));
            list->female = malloc(sizeof(bool) * (n ? n : 1));
            if (!list->names || !list->female) {
                names_free(list);
                break;
            }
        } else {
            list->count = n;
        }
    }
    free(xml);
}

void names_free(NameList* list) {
    free(list->names);
    free(list->female);
    memset(list, 0, sizeof(*list));
}

// ---- Races -------------------------------------------------------------------

static float race_speed(const char* race) {
    // Movement relative to humans (FTL wiki, crew races comparison).
    if (!strcmp(race, "mantis")) return 1.2f;
    if (!strcmp(race, "rock")) return 0.5f;
    if (!strcmp(race, "crystal")) return 0.8f;
    if (!strcmp(race, "anaerobic")) return 0.85f;
    return 1.0f;
}

// A Human walks about 2.2 tiles per second (measured by Mike Hopley, slowed slightly by doors).
static const float BASE_SPEED = 2.2f * TILE;  // ship pixels per second

static float race_health(const char* race) {
    if (!strcmp(race, "rock")) return 150.0f;
    if (!strcmp(race, "crystal")) return 125.0f;
    if (!strcmp(race, "energy")) return 70.0f;
    if (!strcmp(race, "repair")) return 25.0f;   // wiki: System Repair Drone
    if (!strcmp(race, "battle")) return 150.0f;  // Anti-Personnel and Boarding Drones
    if (!strcmp(race, "boarder_ion")) return 125.0f;  // wiki: Ion Intruder Drone
    return 100.0f;
}

// Returns the index of the race/look pair, loading it on first use, or -1.
static int race_get(CrewSet* cs, FtlDat* dat, const AnimDb* db, const char* race, const char* look) {
    for (int i = 0; i < cs->raceCount; i++)
        if (!strcmp(cs->races[i].name, race) && !strcmp(cs->races[i].look, look)) return i;
    if (cs->raceCount >= MAX_RACES) return -1;

    Race* r = &cs->races[cs->raceCount];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", race);
    snprintf(r->look, sizeof(r->look), "%s", look);

    static const char* const dirs[4] = {"down", "right", "up", "left"};
    char name[48];
    for (int d = 0; d < 4; d++) {
        snprintf(name, sizeof(name), "%s_walk_%s", race, dirs[d]);
        if (!animdb_anim(db, name, &r->walk[d])) return -1;
        snprintf(name, sizeof(name), "%s_type_%s", race, dirs[d]);
        if (!animdb_anim(db, name, &r->type[d])) r->type[d] = r->walk[d];
        if (d < 2) {
            snprintf(name, sizeof(name), "%s_punch_%s", race, d == 0 ? "down" : "up");
            if (!animdb_anim(db, name, &r->punch[d])) r->punch[d] = r->walk[d == 0 ? DIR_DOWN : DIR_UP];
        }
        snprintf(name, sizeof(name), "%s_fire_%s", race, dirs[d]);
        if (!animdb_anim(db, name, &r->fight[d])) r->fight[d] = r->type[d];
    }
    snprintf(name, sizeof(name), "%s_repair", race);
    if (!animdb_anim(db, name, &r->repair)) r->repair = r->type[DIR_UP];
    SheetInfo sheet;
    if (!animdb_sheet(db, r->walk[0].sheet, &sheet)) return -1;
    r->fw = sheet.fw;
    r->fh = sheet.fh;
    r->rows = sheet.h / sheet.fh;

    char file[64];
    if (strcmp(look, race) != 0)
        snprintf(file, sizeof(file), "img/people/%s_base.png", look);
    else
        snprintf(file, sizeof(file), "%s", sheet.file);
    uint32_t size;
    uint8_t* png = ftldat_read(dat, file, &size);
    bool ok = png && texture_load_png(&r->tex, png, size);
    // Where the figure actually sits in its frame, so portraits can be centred on it.
    int col = r->walk[DIR_DOWN].x, row = r->rows - 1 - r->walk[DIR_DOWN].y;
    if (!ok || !texture_png_alpha_bounds(png, size, col * r->fw, row * r->fh, r->fw, r->fh, &r->portrait[0],
                                         &r->portrait[1], &r->portrait[2], &r->portrait[3])) {
        r->portrait[0] = r->portrait[1] = 0;
        r->portrait[2] = r->fw - 1;
        r->portrait[3] = r->fh - 1;
    }
    free(png);
    // Without its sprite sheet (no memory for it) the race still works: its crew are drawn as
    // markers rather than missing from the ship.
    (void)ok;

    r->speed = BASE_SPEED * race_speed(race);
    r->maxHealth = race_health(race);
    bool droneRace = !strcmp(race, "repair") || !strcmp(race, "battle") || !strcmp(race, "boarder_ion");
    r->drainsAir = !strcmp(race, "anaerobic");
    r->breathes = !r->drainsAir && !droneRace;
    r->fireImmune = !strcmp(race, "rock") || droneRace;
    return cs->raceCount++;
}

// ---- Placement and pathfinding -----------------------------------------------

// A tile holds one crew member from each side, as in FTL (a 2x2 room fits 4 crew and 4 boarders).
static bool tile_taken(const CrewSet* cs, int x, int y, int except, bool foreign) {
    for (int i = 0; i < cs->count; i++)
        if (i != except && cs->crew[i].foreign == foreign && cs->crew[i].destX == x && cs->crew[i].destY == y) return true;
    return false;
}

static int room_index(const Ship* ship, int id) {
    for (int i = 0; i < ship->roomCount; i++)
        if (ship->rooms[i].id == id) return i;
    return -1;
}

// The station tile of the system in `room`, if it has one.
static bool station_in_room(const Ship* ship, int room, int* tx, int* ty) {
    int s = ship_system_in_room(ship, room);
    if (s < 0 || ship->systems[s].stationX < 0) return false;
    *tx = ship->systems[s].stationX;
    *ty = ship->systems[s].stationY;
    return true;
}

static bool free_tile_in_room(const CrewSet* cs, const Ship* ship, int room, int* tx, int* ty) {
    if (station_in_room(ship, room, tx, ty) && !tile_taken(cs, *tx, *ty, -1, false)) return true;
    const Room* r = &ship->rooms[room];
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++)
            if (!tile_taken(cs, x, y, -1, false)) {
                *tx = x;
                *ty = y;
                return true;
            }
    return false;
}

static float tile_centre(int t) { return t * TILE + TILE * 0.5f; }

void crew_spawn(CrewSet* cs, const Ship* ship, FtlDat* dat, const AnimDb* db, const NameList* names) {
    cs->dat = dat;
    cs->db = db;
    cs->count = 0;
    cs->selected = 0;
    if (!cs->destMarker.loaded) {
        uint32_t size;
        uint8_t* png = ftldat_read(dat, "img/people/green_destination.png", &size);
        if (png) texture_load_png(&cs->destMarker, png, size);
        free(png);
    }

    const ShipBlueprint* bp = ship->bp;
    for (int k = 0; k < bp->crewKinds; k++) {
        for (int n = 0; n < bp->crew[k].amount && cs->count < MAX_CREW; n++) {
            const char* race = bp->crew[k].race;
            if (!strcmp(race, "random")) {  // pirates' mixed crews: any race, each its own (uniform: our choice)
                static const char* const races[] = {"human", "engi", "mantis", "rock", "energy", "slug", "anaerobic"};
                race = races[rand() % 7];
            }
            bool female = !strcmp(race, "human") && (rand() & 1);
            int ri = race_get(cs, dat, db, race, female ? "female" : race);
            if (ri < 0) continue;

            Crew* c = &cs->crew[cs->count];
            memset(c, 0, sizeof(*c));
            c->race = ri;
            c->female = female;
            c->health = cs->races[ri].maxHealth;

            // Pick an unused name, matching sex for humans.
            snprintf(c->name, sizeof(c->name), "%s", race);
            for (int tries = 0; tries < 50 && names->count; tries++) {
                int i = rand() % names->count;
                if (!strcmp(race, "human") && names->female[i] != female) continue;
                bool used = false;
                for (int j = 0; j < cs->count && !used; j++) used = !strcmp(cs->crew[j].name, names->names[i]);
                if (used) continue;
                snprintf(c->name, sizeof(c->name), "%s", names->names[i]);
                break;
            }

            // Stations first, then the first room with space.
            int tx = -1, ty = -1;
            int station = cs->count < 4 ? room_index(ship, bp->stationRooms[cs->count]) : -1;
            if (station < 0 || !free_tile_in_room(cs, ship, station, &tx, &ty))
                for (int r = 0; r < ship->roomCount; r++)
                    if (free_tile_in_room(cs, ship, r, &tx, &ty)) break;
            if (tx < 0) break;

            c->destX = c->homeX = tx;  // starting posts count as saved stations
            c->destY = c->homeY = ty;
            c->x = tile_centre(tx);
            c->y = tile_centre(ty);
            c->dir = DIR_DOWN;
            cs->count++;
        }
    }
}

void crew_prune_races(CrewSet* cs) {
    bool used[MAX_RACES] = {false};
    for (int i = 0; i < cs->count; i++) used[cs->crew[i].race] = true;
    for (int k = 0; k < cs->cloneCount; k++) used[cs->cloneQueue[k].race] = true;
    int map[MAX_RACES], n = 0;
    for (int r = 0; r < cs->raceCount; r++) {
        if (!used[r]) {
            texture_free(&cs->races[r].tex);
            map[r] = -1;
            continue;
        }
        map[r] = n;
        if (n != r) cs->races[n] = cs->races[r];
        n++;
    }
    cs->raceCount = n;
    for (int i = 0; i < cs->count; i++) cs->crew[i].race = map[cs->crew[i].race];
    for (int k = 0; k < cs->cloneCount; k++) cs->cloneQueue[k].race = map[cs->cloneQueue[k].race];
}

void crew_free(CrewSet* cs) {
    for (int i = 0; i < cs->raceCount; i++) texture_free(&cs->races[i].tex);
    cs->raceCount = 0;
    cs->count = 0;
    cs->cloneCount = 0;
    cs->selected = 0;
}

static bool crew_order_to(CrewSet* cs, const Ship* ship, int ci, int room, int wantX, int wantY);

bool crew_order(CrewSet* cs, const Ship* ship, int ci, int room) { return crew_order_to(cs, ship, ci, room, -1, -1); }

void crew_save_stations(CrewSet* cs) {
    for (int i = 0; i < cs->count; i++) {
        Crew* c = &cs->crew[i];
        if (c->foreign || c->drone) continue;
        c->homeX = c->destX;
        c->homeY = c->destY;
    }
}

void crew_return_to_stations(CrewSet* cs, const Ship* ship) {
    for (int i = 0; i < cs->count; i++) {
        Crew* c = &cs->crew[i];
        if (c->foreign || c->drone || c->homeX < 0 || c->mind > 0.0f) continue;
        if (c->destX == c->homeX && c->destY == c->homeY) continue;
        int room = ship_room_at(ship, c->homeX, c->homeY);
        if (room >= 0) crew_order_to(cs, ship, i, room, c->homeX, c->homeY);
    }
}

// Sends crew member ci into `room`: to (wantX, wantY) when given and free, else its station, else
// the nearest free tile.
static bool crew_order_to(CrewSet* cs, const Ship* ship, int ci, int room, int wantX, int wantY) {
    Crew* c = &cs->crew[ci];
    int sx = (int)floorf(c->x / TILE), sy = (int)floorf(c->y / TILE);
    if (ship_room_at(ship, sx, sy) < 0) return false;

    // Breadth-first search over the walkable grid.
    static short dist[GRID_MAX][GRID_MAX];
    static unsigned char from[GRID_MAX][GRID_MAX];  // direction we arrived from
    static short queue[GRID_MAX * GRID_MAX][2];
    static const int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
    static const unsigned char bit[4] = {OPEN_RIGHT, OPEN_DOWN, OPEN_LEFT, OPEN_UP};
    memset(dist, -1, sizeof(dist));
    int head = 0, tail = 0;
    dist[sy][sx] = 0;
    queue[tail][0] = (short)sx;
    queue[tail++][1] = (short)sy;

    int bestX = -1, bestY = -1;
    while (head < tail) {
        int x = queue[head][0], y = queue[head++][1];
        if (bestX < 0 && ship->tileRoom[y][x] == room && !tile_taken(cs, x, y, ci, cs->crew[ci].foreign)) {
            bestX = x;  // BFS order, so this is the closest free tile
            bestY = y;
        }
        for (int d = 0; d < 4; d++) {
            if (!(ship->open[y][x] & bit[d])) continue;
            int nx = x + dx[d], ny = y + dy[d];
            if (dist[ny][nx] >= 0) continue;
            dist[ny][nx] = (short)(dist[y][x] + 1);
            from[ny][nx] = (unsigned char)d;
            queue[tail][0] = (short)nx;
            queue[tail++][1] = (short)ny;
        }
    }
    if (bestX < 0) return false;
    // Prefer the room's station, as the game does.
    int stX, stY;
    if (station_in_room(ship, room, &stX, &stY) && dist[stY][stX] >= 0 && !tile_taken(cs, stX, stY, ci, cs->crew[ci].foreign)) {
        bestX = stX;
        bestY = stY;
    }
    if (wantX >= 0 && wantX < GRID_MAX && wantY >= 0 && wantY < GRID_MAX && dist[wantY][wantX] >= 0 &&
        ship->tileRoom[wantY][wantX] == room && !tile_taken(cs, wantX, wantY, ci, cs->crew[ci].foreign)) {
        bestX = wantX;
        bestY = wantY;
    }

    int len = dist[bestY][bestX] + 1;
    if (len > MAX_PATH) return false;
    for (int i = len - 1, x = bestX, y = bestY; i >= 0; i--) {
        c->path[i][0] = x;
        c->path[i][1] = y;
        if (i) {
            int d = from[y][x];
            x -= dx[d];
            y -= dy[d];
        }
    }
    c->pathLen = len;
    c->pathPos = 0;
    c->destX = bestX;
    c->destY = bestY;
    return true;
}

// ---- Skills --------------------------------------------------------------------
// Experience per level from the wiki's skills table (Crew skills): Piloting and Engines 15,
// Shields 55, Weapons 65, Repair 18, Combat 8; Humans need 13, 13, 50, 58, 16, 7.

static const short SKILL_XP[SKILL_COUNT] = {15, 15, 55, 65, 18, 8};
static const short SKILL_XP_HUMAN[SKILL_COUNT] = {13, 13, 50, 58, 16, 7};

int crew_skill_threshold(const CrewSet* cs, const Crew* m, int skill) {
    return !strcmp(cs->races[m->race].name, "human") ? SKILL_XP_HUMAN[skill] : SKILL_XP[skill];
}

int crew_skill_level(const CrewSet* cs, const Crew* m, int skill) {
    if (m->drone) return 0;
    int t = crew_skill_threshold(cs, m, skill);
    return m->xp[skill] >= 2 * t ? 2 : m->xp[skill] >= t ? 1 : 0;
}

int crew_system_skill(int type) {
    switch (type) {
        case SYS_PILOT: return SKILL_PILOT;
        case SYS_ENGINES: return SKILL_ENGINES;
        case SYS_SHIELDS: return SKILL_SHIELDS;
        case SYS_WEAPONS: return SKILL_WEAPONS;
        default: return -1;
    }
}

bool crew_add_xp(CrewSet* cs, Crew* m, int skill) {
    if (m->drone || skill < 0) return false;
    int t = crew_skill_threshold(cs, m, skill);
    if (m->xp[skill] >= 2 * t) return false;  // mastered
    m->xp[skill]++;
    if (m->xp[skill] == t || m->xp[skill] == 2 * t) {
        m->levelUp = 2.5f;
        m->levelUpSkill = (signed char)skill;
        return true;
    }
    return false;
}

void crew_set_skill_level(CrewSet* cs, Crew* m, int skill, int level) {
    int t = crew_skill_threshold(cs, m, skill);
    if (level > 2) level = 2;
    if (m->xp[skill] < level * t) m->xp[skill] = (short)(level * t);
}

void crew_clone_xp(short xp[SKILL_COUNT]) {
    for (int k = 0; k < SKILL_COUNT; k++)
        xp[k] = k == SKILL_COMBAT ? (xp[k] > 0 ? xp[k] - 1 : 0) : (short)(xp[k] * 8 / 10);
}

void crew_update(CrewSet* cs, Ship* ship, float dt) {
    for (int s = 0; s < ship->systemCount; s++) {
        ship->systems[s].manned = false;
        ship->systems[s].manCrew = -1;
        ship->systems[s].manLevel = 0;
        ship->systems[s].zoltan = 0;
    }
    for (int i = 0; i < cs->count; i++) {
        Crew* c = &cs->crew[i];
        if (c->levelUp > 0.0f) c->levelUp -= dt;
        if (c->ability > 0.0f) c->ability -= dt;
        if (c->stun > 0.0f) {
            c->stun -= dt;
            c->animTime = 0.0f;
            continue;
        }
        // Zoltan crew power the system in the room they're in (wiki: each adds one bar; not
        // weapons or drones here, which power per item).
        if (!c->foreign && !c->drone && !strcmp(cs->races[c->race].name, "energy")) {
            int zs = ship_system_in_room(ship, ship_room_at(ship, (int)floorf(c->x / TILE), (int)floorf(c->y / TILE)));
            if (zs >= 0 && !system_is_subsystem(ship->systems[zs].def->type) && ship->systems[zs].def->type != SYS_WEAPONS &&
                ship->systems[zs].def->type != SYS_DRONES)
                ship->systems[zs].zoltan++;
        }
        float boost = cs->boost[c->foreign ? 1 : 0][c->drone ? 1 : 0];
        float step = cs->races[c->race].speed * dt * (boost > 0.0f ? boost : 1.0f);
        bool moved = false;
        while (step > 0.0f && c->pathPos < c->pathLen) {
            // A locked-down room: nobody crosses its doors either way until the crystal melts.
            int fromRoom = ship_room_at(ship, (int)floorf(c->x / TILE), (int)floorf(c->y / TILE));
            int toRoom = ship_room_at(ship, c->path[c->pathPos][0], c->path[c->pathPos][1]);
            if (fromRoom != toRoom && ((fromRoom >= 0 && ship->lockdown[fromRoom] > 0.0f) || (toRoom >= 0 && ship->lockdown[toRoom] > 0.0f)))
                break;
            float tx = tile_centre(c->path[c->pathPos][0]), ty = tile_centre(c->path[c->pathPos][1]);
            float ddx = tx - c->x, ddy = ty - c->y;
            float d = fabsf(ddx) + fabsf(ddy);  // moves are axis-aligned
            if (d > 0.01f) {
                if (fabsf(ddx) > fabsf(ddy))
                    c->dir = ddx > 0 ? DIR_RIGHT : DIR_LEFT;
                else
                    c->dir = ddy > 0 ? DIR_DOWN : DIR_UP;
                moved = true;
            }
            if (d <= step) {
                c->x = tx;
                c->y = ty;
                step -= d;
                c->pathPos++;
            } else {
                c->x += ddx / d * step;
                c->y += ddy / d * step;
                step = 0.0f;
            }
        }
        if (c->pathPos < c->pathLen) {
            int cx = (int)floorf(c->x / TILE), cy = (int)floorf(c->y / TILE);
            int door = ship_door_between(ship, cx, cy, c->path[c->pathPos][0], c->path[c->pathPos][1]);
            if (door >= 0) ship->doorState[door].hold = 0.4f;
        }

        c->manning = false;
        if (!moved && c->pathPos >= c->pathLen) {
            int s = ship_system_in_room(ship, ship_room_at(ship, c->destX, c->destY));
            ShipSystem* sys = s >= 0 ? &ship->systems[s] : NULL;
            if (sys && !c->foreign && !c->drone && system_is_mannable(sys->def->type) && sys->stationX == c->destX &&
                sys->stationY == c->destY) {
                c->manning = true;
                c->dir = sys->def->slotDir;
                sys->manned = true;
                sys->manCrew = (signed char)i;
                int sk = crew_system_skill(sys->def->type);
                sys->manLevel = (signed char)(sk >= 0 ? crew_skill_level(cs, c, sk) : 0);
            } else {
                c->dir = DIR_DOWN;
            }
        }
        if (moved || c->manning || c->repairing || c->fighting || c->brawling)
            c->animTime += dt;
        else
            c->animTime = 0.0f;
    }
}

bool crew_transfer(CrewSet* from, int ci, CrewSet* to, int tx, int ty) {
    if (to->count >= MAX_CREW) return false;
    const Crew src = from->crew[ci];
    const Race* r = &from->races[src.race];
    int race = race_get(to, to->dat, to->db, r->name, r->look);
    if (race < 0) return false;
    Crew* c = &to->crew[to->count++];
    *c = src;
    c->race = race;
    c->foreign = !src.foreign;
    c->pathLen = c->pathPos = 0;
    c->destX = tx;
    c->destY = ty;
    c->x = tx * TILE + TILE * 0.5f;
    c->y = ty * TILE + TILE * 0.5f;
    c->manning = c->repairing = c->fighting = c->brawling = false;
    crew_remove(from, ci);
    return true;
}

int crew_native_count(const CrewSet* cs) {
    int n = 0;
    if (!cs) return 0;
    for (int i = 0; i < cs->count; i++) n += !cs->crew[i].foreign && !cs->crew[i].drone;
    return n;
}

int crew_foreign_count(const CrewSet* cs) {
    int n = 0;
    if (!cs) return 0;
    for (int i = 0; i < cs->count; i++) n += cs->crew[i].foreign && !cs->crew[i].drone;
    return n;
}

int crew_race(CrewSet* cs, const char* race) { return race_get(cs, cs->dat, cs->db, race, race); }

int crew_race_look(CrewSet* cs, const char* race, const char* look) { return race_get(cs, cs->dat, cs->db, race, look); }

bool crew_add(CrewSet* cs, int race, const char* name, bool female, int tx, int ty) {
    if (cs->count >= MAX_CREW) return false;
    Crew* c = &cs->crew[cs->count++];
    memset(c, 0, sizeof(*c));
    c->homeX = c->homeY = -1;
    c->race = race;
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->female = female;
    c->health = cs->races[race].maxHealth;
    c->destX = tx;
    c->destY = ty;
    c->x = tx * TILE + TILE * 0.5f;
    c->y = ty * TILE + TILE * 0.5f;
    c->dir = DIR_DOWN;
    return true;
}

void crew_order_selected(CrewSet* cs, const Ship* ship, int room) {
    for (int i = 0; i < cs->count; i++)
        if (crew_is_selected(cs, i) && !cs->crew[i].foreign && !cs->crew[i].drone) crew_order(cs, ship, i, room);
}

void crew_remove(CrewSet* cs, int ci) {
    for (int i = ci; i < cs->count - 1; i++) cs->crew[i] = cs->crew[i + 1];
    cs->count--;
    unsigned below = cs->selected & ((1u << ci) - 1u), above = (cs->selected >> (ci + 1)) << ci;
    cs->selected = below | above;
}

int crew_pick(const CrewSet* cs, float px, float py, float radius) {
    int best = -1;
    float bestD = radius * radius;
    for (int i = 0; i < cs->count; i++) {
        if (cs->crew[i].foreign || cs->crew[i].drone) continue;
        float dx = cs->crew[i].x - px, dy = cs->crew[i].y - py;
        float d = dx * dx + dy * dy;
        if (d <= bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void crew_draw(const CrewSet* cs, int ci, float x, float y, float scale) {
    const Crew* c = &cs->crew[ci];
    const Race* r = &cs->races[c->race];
    const AnimInfo* a = c->brawling    ? &r->punch[c->dir == DIR_UP ? 1 : 0]
                        : c->fighting  ? &r->fight[c->dir]
                        : c->repairing ? &r->repair
                        : c->manning   ? &r->type[c->dir]
                                       : &r->walk[c->dir];
    if (a->length <= 0 || a->time <= 0.0f) a = &r->walk[c->dir];  // a race without this animation
    if (a->length <= 0) return;
    float t = a->time > 0.0f ? a->time : 1.0f;
    int frame = (int)(c->animTime / t * a->length) % a->length;
    sheet_draw_frame(&r->tex, r->fw, r->fh, a->x + frame, r->rows - 1 - a->y, x - r->fw * scale * 0.5f,
                     y - r->fh * scale * 0.5f, scale);
}
