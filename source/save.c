#include "save.h"

#include <stdio.h>
#include <string.h>

#define SAVE_FILE "sdmc:/3ds/ftl3ds/save.dat"
#define SAVE_VERSION 1

// A save only fits the build that wrote it: the structures are stored as they are in memory.
typedef struct {
    char magic[8];
    unsigned version, runSize, shipSize;
} SaveHeader;

static void header_init(SaveHeader* h) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "FTL3DSSV", 8);
    h->version = SAVE_VERSION;
    h->runSize = sizeof(Run);
    h->shipSize = sizeof(SaveShip);
}

void save_ship_capture(SaveShip* out, const Ship* s, const CrewSet* cs) {
    memset(out, 0, sizeof(*out));
    snprintf(out->bp, sizeof(out->bp), "%s", s->bp->name);
    out->hullHp = s->hullHp;
    out->maxHull = s->maxHull;
    out->missiles = s->missiles;
    out->droneParts = s->droneParts;
    out->reactor = s->reactor;
    out->systemCount = s->systemCount;
    for (int i = 0; i < s->systemCount; i++) {
        out->sys[i].type = s->systems[i].def->type;
        out->sys[i].level = s->systems[i].def->level;
        out->sys[i].power = s->systems[i].power;
        out->sys[i].damage = s->systems[i].damage;
    }
    out->weaponCount = s->weaponCount;
    for (int i = 0; i < s->weaponCount; i++) {
        snprintf(out->weapons[i], sizeof(out->weapons[i]), "%s", s->weapons[i].def->name);
        out->weaponOn[i] = s->weapons[i].powered;
    }
    out->droneCount = s->droneCount;
    for (int i = 0; i < s->droneCount; i++) snprintf(out->drones[i], sizeof(out->drones[i]), "%s", s->drones[i].def->name);
    for (int i = 0; i < cs->count && out->crewCount < SAVE_CREW; i++) {
        const Crew* m = &cs->crew[i];
        if (m->foreign || m->drone) continue;
        const Race* r = &cs->races[m->race];
        typeof(out->crew[0])* c = &out->crew[out->crewCount++];
        snprintf(c->race, sizeof(c->race), "%s", r->name);
        snprintf(c->look, sizeof(c->look), "%s", r->look);
        snprintf(c->name, sizeof(c->name), "%s", m->name);
        c->female = m->female;
        c->x = m->destX;
        c->y = m->destY;
        c->homeX = m->homeX;
        c->homeY = m->homeY;
        c->health = m->health;
        memcpy(c->xp, m->xp, sizeof(c->xp));
    }
}

bool save_store(const Run* run, const SaveShip* ship) {
    static Run packed;  // big: not on the stack
    packed = *run;
    run_pack(&packed);
    SaveHeader h;
    header_init(&h);
    FILE* f = fopen(SAVE_FILE, "wb");
    if (!f) return false;
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(&packed, sizeof(packed), 1, f) == 1 && fwrite(ship, sizeof(*ship), 1, f) == 1;
    fclose(f);
    if (!ok) remove(SAVE_FILE);
    return ok;
}

static bool read_header(FILE* f) {
    SaveHeader want, got;
    header_init(&want);
    return fread(&got, sizeof(got), 1, f) == 1 && !memcmp(&want, &got, sizeof(got));
}

bool save_load(Run* run, SaveShip* ship) {
    FILE* f = fopen(SAVE_FILE, "rb");
    if (!f) return false;
    static Run packed;
    bool ok = read_header(f) && fread(&packed, sizeof(packed), 1, f) == 1 && fread(ship, sizeof(*ship), 1, f) == 1;
    fclose(f);
    if (!ok) return false;
    run_unpack(&packed);
    *run = packed;
    return true;
}

bool save_exists(void) {
    FILE* f = fopen(SAVE_FILE, "rb");
    if (!f) return false;
    bool ok = read_header(f);
    fclose(f);
    return ok;
}

void save_delete(void) { remove(SAVE_FILE); }
