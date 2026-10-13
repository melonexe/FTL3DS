// Invariant checks (test builds only): after every frame, the game state is checked against rules
// that must always hold, whatever a scenario does: hull, power, oxygen, fires, weapons, crew and the
// run's resources. A scenario's own checks look at a few values at a few moments; these look at
// everything, all the time, in every run of the seed sweeps too.
//
// The game updates things in an order (damage, then the power it takes away, ...), so a rule may be
// broken for a frame or two by design. A rule only counts once it's broken for INV_HOLD_FRAMES
// frames in a row; NaN and infinite values count at once. Each rule is reported once per ship per
// run, as "ERROR invariant: <rule> ...", which the runners treat as a serious failure.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "combat.h"
#include "invariants.h"
#include "testhooks.h"

#define INV_HOLD_FRAMES 30  // half a second at 60 fps
#define EPS 0.01f

enum {
    INV_NAN, INV_HULL, INV_RESOURCES, INV_SHIELDS, INV_SYSTEM, INV_POWER, INV_OXYGEN, INV_FIRE, INV_WEAPON,
    INV_SLOTS, INV_DOOR, INV_CREW_HEALTH, INV_CREW_PLACE, INV_RUN, INV_COUNT
};
static const char* const NAMES[INV_COUNT] = {"not-a-number", "hull", "resources", "shields", "system", "power",
                                             "oxygen", "fire/breach", "weapon", "slots", "door", "crew health",
                                             "crew position", "run"};

static struct {
    int streak;          // frames in a row it's been broken
    bool seen, reported;  // broken this frame; already logged
    char detail[160];
} sRule[INV_COUNT][3];  // [rule][0 ours, 1 enemy, 2 the run]

static int sSide;

static void bad(int rule, const char* fmt, ...) {
    if (sRule[rule][sSide].seen) return;  // the first problem this frame is the one reported
    sRule[rule][sSide].seen = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sRule[rule][sSide].detail, sizeof(sRule[rule][sSide].detail), fmt, ap);
    va_end(ap);
}

#define FINITE(v, what, ...)                                   \
    do {                                                       \
        if (!isfinite(v)) bad(INV_NAN, what, ##__VA_ARGS__);  \
    } while (0)

static void check_ship(const Ship* s) {
    if (s->maxHull <= 0 || s->hullHp > s->maxHull) bad(INV_HULL, "hull %d / %d", s->hullHp, s->maxHull);
    if (s->missiles < 0 || s->droneParts < 0) bad(INV_RESOURCES, "missiles %d, drone parts %d", s->missiles, s->droneParts);

    FINITE(s->shieldCharge, "shield charge");
    int maxShields = combat_max_shields(s);
    if (s->shieldLayers < 0 || s->shieldLayers > maxShields)
        bad(INV_SHIELDS, "%d layers with shields powered for %d", s->shieldLayers, maxShields);
    int superMax = s->superShieldMax > 0 ? s->superShieldMax : 5;
    if (s->superShield < 0 || s->superShield > superMax) bad(INV_SHIELDS, "Zoltan shield %d / %d", s->superShield, superMax);

    for (int i = 0; i < s->systemCount; i++) {
        const ShipSystem* y = &s->systems[i];
        int level = y->def->level;
        if (y->damage < 0 || y->damage > level || y->power < 0 || y->power > level || y->zoltan < 0)
            bad(INV_SYSTEM, "system %d: power %d, damage %d, level %d, zoltan %d", y->def->type, y->power, y->damage, level, y->zoltan);
        if (y->repair < -EPS || y->repair > 1 + EPS || y->burn < -EPS || y->burn > 1 + EPS || y->sabotage < -EPS ||
            y->sabotage > 1 + EPS || y->ionTimer < -EPS)
            bad(INV_SYSTEM, "system %d: repair %.2f, burn %.2f, sabotage %.2f, ion %.2f", y->def->type, y->repair, y->burn,
                y->sabotage, y->ionTimer);
        FINITE(y->repair + y->burn + y->sabotage + y->ionTimer, "system %d timers", y->def->type);
    }
    if (ship_reactor_free(s) < 0) bad(INV_POWER, "%d more power in use than the reactor gives", -ship_reactor_free(s));

    for (int r = 0; r < s->roomCount; r++) {
        FINITE(s->oxygen[r], "oxygen in room %d", r);
        if (s->oxygen[r] < -EPS || s->oxygen[r] > 100 + EPS) bad(INV_OXYGEN, "room %d at %.1f%%", r, s->oxygen[r]);
    }
    for (int y = 0; y < GRID_MAX; y++)
        for (int x = 0; x < GRID_MAX; x++) {
            float f = s->fire[y][x], b = s->breach[y][x];
            FINITE(f + b, "fire/breach at %d,%d", x, y);
            if (f < -EPS || f > 100 + EPS || b < -EPS || b > 100 + EPS) bad(INV_FIRE, "tile %d,%d: fire %.1f, breach %.1f", x, y, f, b);
            bool inRoom = x < s->gridW && y < s->gridH && s->tileRoom[y][x] >= 0;
            if ((f > 0 || b > 0) && !inRoom) bad(INV_FIRE, "fire %.1f / breach %.1f outside the rooms at %d,%d", f, b, x, y);
        }

    // Slots limit the player's equipment; enemies (the flagship's drones) can carry more.
    if (sSide == 0 && (s->weaponCount > s->weaponSlots || s->droneCount > s->droneSlots || s->weaponCount < 0 || s->droneCount < 0))
        bad(INV_SLOTS, "%d weapons in %d slots, %d drones in %d", s->weaponCount, s->weaponSlots, s->droneCount, s->droneSlots);
    for (int i = 0; i < s->weaponCount; i++) {
        const WeaponSlot* w = &s->weapons[i];
        FINITE(w->charge, "weapon %d charge", i);
        if (w->charge < -EPS || (w->fullCharge > 0 && w->charge > w->fullCharge + 0.05f))
            bad(INV_WEAPON, "weapon %d (%s): charge %.2f of %.2f", i, w->def->name, w->charge, w->fullCharge);
    }
    for (int i = 0; i < s->doorCount; i++)
        if (s->doorState[i].anim < -EPS || s->doorState[i].anim > 1 + EPS) bad(INV_DOOR, "door %d at %.2f", i, s->doorState[i].anim);
}

static void check_crew(const Ship* s, const CrewSet* cs) {
    for (int i = 0; i < cs->count; i++) {
        const Crew* m = &cs->crew[i];
        FINITE(m->x + m->y + m->health, "crew %d (%s)", i, m->name);
        if (m->race < 0 || m->race >= cs->raceCount) {
            bad(INV_CREW_HEALTH, "crew %d (%s): race %d of %d", i, m->name, m->race, cs->raceCount);
            continue;
        }
        float max = cs->races[m->race].maxHealth;
        if (m->health > max + EPS || m->health < -EPS) bad(INV_CREW_HEALTH, "%s: health %.1f of %.0f", m->name, m->health, max);
        int tx = (int)floorf(m->x / TILE), ty = (int)floorf(m->y / TILE);
        if (ship_room_at(s, tx, ty) < 0) bad(INV_CREW_PLACE, "%s at %.0f,%.0f, outside the rooms", m->name, m->x, m->y);
    }
}

static void check_run(const Run* r) {
    if (r->scrap < 0 || r->fuel < 0) bad(INV_RUN, "scrap %d, fuel %d", r->scrap, r->fuel);
    if (r->sector < 0 || r->sector >= RUN_SECTORS) bad(INV_RUN, "sector %d", r->sector);
    if (r->beaconCount > 0 && (r->current < 0 || r->current >= r->beaconCount))
        bad(INV_RUN, "at beacon %d of %d", r->current, r->beaconCount);
}

void test_invariants(float now, const Ship* ship, const CrewSet* crew, const Ship* foe, const CrewSet* foeCrew,
                     const Run* run) {
    for (int k = 0; k < INV_COUNT; k++)
        for (int side = 0; side < 3; side++) sRule[k][side].seen = false;
    sSide = 0;
    if (ship) {
        check_ship(ship);
        if (crew) check_crew(ship, crew);
    }
    sSide = 1;
    if (foe && foe->deathTime < 0.0f && foe->hullHp > 0) {
        check_ship(foe);
        if (foeCrew) check_crew(foe, foeCrew);
    }
    sSide = 2;
    if (run) check_run(run);

    static const char* const WHOSE[3] = {"our ship", "the enemy", "the run"};
    for (int k = 0; k < INV_COUNT; k++)
        for (int side = 0; side < 3; side++) {
            typeof(sRule[0][0])* r = &sRule[k][side];
            r->streak = r->seen ? r->streak + 1 : 0;
            if (!r->reported && r->streak >= (k == INV_NAN ? 1 : INV_HOLD_FRAMES)) {
                r->reported = true;
                test_log(now, "ERROR invariant: %s (%s): %s", NAMES[k], WHOSE[side], r->detail);
            }
        }
}
