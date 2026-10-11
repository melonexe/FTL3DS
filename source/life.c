#include "life.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "feats.h"
#include "sound.h"

// Numbers from the FTL wiki (Oxygen, Medbay, Fires) and xftl's reverse engineering of the game
// (gitlab.com/znixian/xftl, doc/oxygen and doc/fires).

// Oxygen: refill 1.2%/s x 1/4/7 by power; -1.2%/s with the system off.
static const float O2_REFILL[4] = {0.0f, 1.2f, 4.8f, 8.4f};
#define O2_LOSS_UNPOWERED 1.2f
#define O2_SPREAD 0.08f          // rooms joined by open doors move 8%/s of the gap to their average
#define O2_AIRLOCK_LOSS 16.0f    // %/s per open airlock door ...
#define O2_BREACH_LOSS 8.0f      // ... and per hull breach or Lanius,
#define O2_LOSS_FALLOFF 0.75f    // times 0.75 per room of distance through open doors
#define O2_FIRE_LOSS 0.96f       // %/s per fire, in its own room only

#define SUFFOCATE_DPS 6.4f
static const float MEDBAY_HPS[4] = {0.0f, 6.4f, 9.6f, 19.2f};

// Fires.
#define FIRE_CREW_DPS 2.128f     // per fire in the room (Rock crew are immune)
#define FIRE_SYSTEM_DPS 0.08f    // bars per second per fire in the room
#define FIRE_STARVE_BELOW 10.0f  // fires start burning out below this much oxygen
#define FIRE_SPREAD_OPEN 1.6f    // spread countdown per second per adjacent fire: same room / open door
#define FIRE_SPREAD_DOOR1 0.9143f  // closed level-1 door
#define FIRE_SPREAD_DOOR2 0.16f    // closed level-2+ door

#define DOOR_SPEED 5.0f          // our choice: door open/close animations per second

// Clone bay (xftl Clonebay.kt, wiki Clone Bay): 12/9/7 s per clone by power; an unpowered bay loses
// the newest queued clone after 3 s.
static const float CLONE_SECONDS[3] = {12.0f, 9.0f, 7.0f};
#define CLONE_LOSS_SECONDS 3.0f

static DeadBoarder sDead[8];
static int sDeadCount;

int life_take_dead_boarders(DeadBoarder* out, int max) {
    int n = sDeadCount < max ? sDeadCount : max;
    if (out) memcpy(out, sDead, sizeof(DeadBoarder) * n);
    sDeadCount = 0;
    return out ? n : 0;
}

static float room_area(const Ship* s, int r) { return (float)(s->rooms[r].w * s->rooms[r].h); }

// The rooms on either side of a door; -1 for space.
static void door_rooms(const Ship* s, int door, int* a, int* b) {
    const Door* d = &s->doors[door];
    int ax = d->vertical ? d->x - 1 : d->x, ay = d->vertical ? d->y : d->y - 1;
    *a = ship_room_at(s, ax, ay);
    *b = ship_room_at(s, d->x, d->y);
}

static bool door_open(const Ship* s, int door) { return s->doorState[door].anim > 0.0f; }

static void play_death(const CrewSet* cs, const Crew* c) {
    const char* race = cs->races[c->race].name;
    static char name[24];
    if (!strcmp(race, "human")) {
        if (c->female)
            snprintf(name, sizeof(name), "femaleDeath%d", 1 + rand() % 3);
        else
            snprintf(name, sizeof(name), "death%d", 1 + rand() % 2);
    } else if (!strcmp(race, "anaerobic")) {
        snprintf(name, sizeof(name), "shrikeDeath");
    } else if (!strcmp(race, "slug")) {
        snprintf(name, sizeof(name), "death%d", 1 + rand() % 2);
    } else {
        snprintf(name, sizeof(name), "%sDeath", race);  // rockDeath, mantisDeath, engiDeath, ...
    }
    sound_play(name);
}

// ---- Oxygen ----------------------------------------------------------------------

// Rooms reachable through open interior doors from `start`, with their distance (-1 if not).
static void room_distances(const Ship* s, int start, int dist[MAX_ROOMS]) {
    int queue[MAX_ROOMS], head = 0, tail = 0;
    for (int r = 0; r < s->roomCount; r++) dist[r] = -1;
    dist[start] = 0;
    queue[tail++] = start;
    while (head < tail) {
        int r = queue[head++];
        for (int i = 0; i < s->doorCount; i++) {
            int a, b;
            door_rooms(s, i, &a, &b);
            if (a < 0 || b < 0 || !door_open(s, i)) continue;
            int other = a == r ? b : b == r ? a : -1;
            if (other >= 0 && dist[other] < 0) {
                dist[other] = dist[r] + 1;
                queue[tail++] = other;
            }
        }
    }
}

// Air lost to space at `room` (airlock, breach, Lanius), spreading through open doors.
static void air_loss(Ship* s, int room, float rate, float dt) {
    int dist[MAX_ROOMS];
    room_distances(s, room, dist);
    for (int r = 0; r < s->roomCount; r++)
        if (dist[r] >= 0) s->oxygen[r] -= rate * powf(O2_LOSS_FALLOFF, (float)dist[r]) * dt;
}

static void update_oxygen(Ship* s, const CrewSet* cs, float dt) {
    int o2 = ship_find_system(s, SYS_OXYGEN);
    int o2Power = ship_system_power(s, o2);
    float change = o2Power > 0 ? O2_REFILL[o2Power < 3 ? o2Power : 3] : -O2_LOSS_UNPOWERED;
    for (int r = 0; r < s->roomCount; r++) s->oxygen[r] += change * dt;

    // Rooms joined by open doors share their air.
    bool done[MAX_ROOMS] = {false};
    for (int r = 0; r < s->roomCount; r++) {
        if (done[r]) continue;
        int dist[MAX_ROOMS];
        room_distances(s, r, dist);
        float sum = 0.0f;
        int n = 0;
        for (int k = 0; k < s->roomCount; k++)
            if (dist[k] >= 0) {
                done[k] = true;
                sum += s->oxygen[k];
                n++;
            }
        float avg = sum / n;
        for (int k = 0; k < s->roomCount; k++)
            if (dist[k] >= 0) s->oxygen[k] += (avg - s->oxygen[k]) * O2_SPREAD * dt;
    }

    // Open airlocks, breaches and Lanius lose air to space.
    for (int i = 0; i < s->doorCount; i++) {
        int a, b;
        door_rooms(s, i, &a, &b);
        if ((a < 0) != (b < 0) && door_open(s, i))
            air_loss(s, a >= 0 ? a : b, O2_AIRLOCK_LOSS * s->doorState[i].anim, dt);
    }
    for (int y = 0; y < s->gridH; y++)
        for (int x = 0; x < s->gridW; x++) {
            if (s->breach[y][x] > 0.0f) air_loss(s, s->tileRoom[y][x], O2_BREACH_LOSS, dt);
            if (s->fire[y][x] > 0.0f) s->oxygen[s->tileRoom[y][x]] -= O2_FIRE_LOSS * dt;
        }
    for (int i = 0; i < cs->count; i++) {
        const Crew* c = &cs->crew[i];
        int r = ship_room_at(s, (int)floorf(c->x / TILE), (int)floorf(c->y / TILE));
        if (r >= 0 && cs->races[c->race].drainsAir) air_loss(s, r, O2_BREACH_LOSS, dt);
    }
    for (int r = 0; r < s->roomCount; r++) {
        if (s->oxygen[r] < 0.0f) s->oxygen[r] = 0.0f;
        if (s->oxygen[r] > 100.0f) s->oxygen[r] = 100.0f;
    }
}

// ---- Fires -----------------------------------------------------------------------

static const int DX[4] = {1, 0, -1, 0}, DY[4] = {0, 1, 0, -1};
static const unsigned char OPEN_BIT[4] = {OPEN_RIGHT, OPEN_DOWN, OPEN_LEFT, OPEN_UP};

// How fast a fire next to tile (x,y), in direction d, spreads onto it; 0 through a wall.
static float spread_rate(const Ship* s, int x, int y, int d) {
    if (!(s->open[y][x] & OPEN_BIT[d])) return 0.0f;
    int door = ship_door_between(s, x, y, x + DX[d], y + DY[d]);
    if (door < 0 || door_open(s, door)) return FIRE_SPREAD_OPEN;
    int ds = ship_find_system(s, SYS_DOORS);
    int level = ds >= 0 ? s->systems[ds].def->level : 0;
    return level >= 2 ? FIRE_SPREAD_DOOR2 : FIRE_SPREAD_DOOR1;
}

static int adjacent_fires(const Ship* s, int x, int y) {
    int n = 0;
    for (int d = 0; d < 4; d++)
        if ((s->open[y][x] & OPEN_BIT[d]) && s->fire[y + DY[d]][x + DX[d]] > 0.0f) n++;
    return n;
}

static void update_fires(Ship* s, CrewSet* cs, float dt) {
    float roomFires[MAX_ROOMS] = {0};

    for (int y = 0; y < s->gridH; y++)
        for (int x = 0; x < s->gridW; x++) {
            int r = s->tileRoom[y][x];
            if (r < 0) continue;
            if (s->fire[y][x] > 0.0f) {
                roomFires[r] += 1.0f;
                // Starved of air, a fire burns out after 5-14 "seconds", slower with fires around it.
                float* t = &s->fireBurnout[y][x];
                if (*t <= 0.0f && s->oxygen[r] < FIRE_STARVE_BELOW) *t = 5.0f + rand() % 10;
                if (*t > 0.0f) {
                    *t -= (5 - adjacent_fires(s, x, y)) * 0.48f * dt;
                    if (*t <= 0.0f) {
                        if (s->oxygen[r] < FIRE_STARVE_BELOW) {
                            s->fire[y][x] = 0.0f;
                            *t = 0.0f;
                        } else {
                            *t = 0.0001f;  // goes out the moment the air drops again
                        }
                    }
                }
                continue;
            }
            // Not burning: count down while there's fire next to it, then catch.
            float rate = 0.0f;
            for (int d = 0; d < 4; d++) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx >= 0 && ny >= 0 && nx < GRID_MAX && ny < GRID_MAX && s->fire[ny][nx] > 0.0f)
                    rate += spread_rate(s, x, y, d);
            }
            if (rate <= 0.0f) {
                s->fireSpread[y][x] = 0.0f;
                continue;
            }
            if (s->fireSpread[y][x] <= 0.0f) s->fireSpread[y][x] = 10.0f + rand() % 40;
            s->fireSpread[y][x] -= rate * dt;
            if (s->fireSpread[y][x] <= 0.0f) {
                s->fire[y][x] = 100.0f;
                s->fireSpread[y][x] = 0.0f;
                s->fireBurnout[y][x] = 0.0f;
            }
        }

    // Damage: crew and systems in burning rooms. A system burned all the way out costs 1 hull.
    for (int i = 0; i < cs->count; i++) {
        Crew* c = &cs->crew[i];
        int r = ship_room_at(s, (int)floorf(c->x / TILE), (int)floorf(c->y / TILE));
        if (r >= 0 && roomFires[r] > 0.0f && !cs->races[c->race].fireImmune)
            c->health -= FIRE_CREW_DPS * roomFires[r] * dt;
    }
    for (int i = 0; i < s->systemCount; i++) {
        ShipSystem* y = &s->systems[i];
        float n = roomFires[y->room];
        if (n <= 0.0f || y->damage >= y->def->level) continue;
        y->burn += FIRE_SYSTEM_DPS * n * dt;
        if (y->burn >= 1.0f) {
            y->burn -= 1.0f;
            y->damage++;
            y->repair = 0.0f;
            if (y->damage >= y->def->level) s->hullHp -= 1;
        }
    }
}

static bool random_tile(const Ship* s, int room, float grid[GRID_MAX][GRID_MAX], int* tx, int* ty) {
    const Room* r = &s->rooms[room];
    int free = 0;
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++)
            if (grid[y][x] <= 0.0f) free++;
    if (free == 0) return false;
    int pick = rand() % free;
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++)
            if (grid[y][x] <= 0.0f && pick-- == 0) {
                *tx = x;
                *ty = y;
                return true;
            }
    return false;
}

bool life_ignite(Ship* s, int room) {
    int x, y;
    if (room < 0 || !random_tile(s, room, s->fire, &x, &y)) return false;
    s->fire[y][x] = 100.0f;
    s->fireBurnout[y][x] = 0.0f;
    return true;
}

bool life_breach(Ship* s, int room) {
    int x, y;
    if (room < 0 || !random_tile(s, room, s->breach, &x, &y)) return false;
    s->breach[y][x] = 100.0f;
    sound_play("hullBreach");
    return true;
}

static int count_in_room(const Ship* s, int room, const float grid[GRID_MAX][GRID_MAX]) {
    const Room* r = &s->rooms[room];
    int n = 0;
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++)
            if (grid[y][x] > 0.0f) n++;
    return n;
}

int life_room_fires(const Ship* s, int room) { return count_in_room(s, room, s->fire); }
int life_room_breaches(const Ship* s, int room) { return count_in_room(s, room, s->breach); }

int life_ship_fires(const Ship* s) {
    int n = 0;
    for (int r = 0; r < s->roomCount; r++) n += life_room_fires(s, r);
    return n;
}

int life_ship_breaches(const Ship* s) {
    int n = 0;
    for (int r = 0; r < s->roomCount; r++) n += life_room_breaches(s, r);
    return n;
}

static bool roomFires(const Ship* s, int room) { return life_room_fires(s, room) > 0; }

// ---- Update ----------------------------------------------------------------------

void life_update_doors(Ship* s, float dt, bool paused) {
    for (int i = 0; i < s->doorCount; i++) {
        DoorState* d = &s->doorState[i];
        bool open = d->forced || d->hold > 0.0f;
        if (!paused) d->hold -= dt;  // crew walking through: frozen while paused, so their doors stay open
        d->anim += (open ? DOOR_SPEED : -DOOR_SPEED) * dt;
        if (d->anim < 0.0f) d->anim = 0.0f;
        if (d->anim > 1.0f) d->anim = 1.0f;
    }
}

void life_update(Ship* s, CrewSet* cs, float dt) {
    life_update_doors(s, dt, false);

    update_oxygen(s, cs, dt);
    update_fires(s, cs, dt);

    // Crew: suffocation and healing.
    int med = ship_find_system(s, SYS_MEDBAY);
    for (int i = 0; i < cs->count; i++) {
        Crew* c = &cs->crew[i];
        const Race* race = &cs->races[c->race];
        int r = ship_room_at(s, (int)floorf(c->x / TILE), (int)floorf(c->y / TILE));
        if (r < 0) continue;
        if (race->breathes && s->oxygen[r] < LIFE_SUFFOCATE_BELOW)
            c->health -= SUFFOCATE_DPS * dt * (!strcmp(race->name, "crystal") ? 0.5f : 1.0f) *
                         (!c->foreign && s->aug[AUG_O2_MASKS] ? 0.5f : 1.0f);  // Emergency Respirators
        // The medbay heals the ship's own crew, not intruders (FTL wiki: Medbay).
        if (med >= 0 && s->systems[med].room == r && !c->foreign && !c->drone) {
            int p = ship_system_power(s, med);
            c->health += MEDBAY_HPS[p < 3 ? p : 3] * dt;
        } else if (med >= 0 && s->aug[AUG_NANO_MEDBAY] && !c->foreign && !c->drone && ship_system_power(s, med) > 0) {
            c->health += 1.6f * dt;  // Engi Med-bot Dispersal (wiki): anywhere aboard, while the medbay has power
        }
        if (c->health > race->maxHealth) c->health = race->maxHealth;
        if (c->health <= 0.0f) {
            if (c->drone) {
                sound_play("explosion1");
                crew_remove(cs, i--);
                continue;
            }
            play_death(cs, c);
            // Whose crew they are: mind control flips `foreign` but not who they belong to.
            bool ours = c->foreign == (c->mind > 0.0f);
            // For achievements: an enemy crew member killed (and by what), or one of ours lost.
            bool enemyCrew = ours == s->bp->enemy;
            if (enemyCrew) {
                gFeats.fight.enemyCrewKilled++;
                if (c->lastHit == HIT_CREW && s->bp->enemy && roomFires(s, r)) gFeats.fight.burningKill = true;
                if (c->lastHit >= HIT_BOARDER && c->lastHit - HIT_BOARDER < 4) gFeats.fight.boarderKills[c->lastHit - HIT_BOARDER]++;
                if (c->lastHitBio) {
                    if (c->lastHitSerial == gFeats.fight.bioSerial) {
                        gFeats.fight.bioKills++;
                    } else {
                        gFeats.fight.bioSerial = c->lastHitSerial;
                        gFeats.fight.bioKills = 1;
                    }
                }
            } else {
                gFeats.fight.ourCrewLost++;
                gFeats.ourDeaths++;
            }
            if (!ours && sDeadCount < 8) {  // a boarder: their own ship may clone them
                DeadBoarder* d = &sDead[sDeadCount++];
                snprintf(d->race, sizeof(d->race), "%s", race->name);
                memcpy(d->name, c->name, sizeof(d->name));
                d->female = c->female;
                memcpy(d->xp, c->xp, sizeof(d->xp));
            }
            int cb = ship_find_system(s, SYS_CLONEBAY);
            // Backup DNA Bank: queued even with the clone bay off or broken.
            if (cb >= 0 && ours && (ship_system_capacity(s, cb) > 0 || s->aug[AUG_BACKUP_DNA]) && cs->cloneCount < MAX_CREW) {
                cs->cloneQueue[cs->cloneCount].race = c->race;
                cs->cloneQueue[cs->cloneCount].female = c->female;
                memcpy(cs->cloneQueue[cs->cloneCount].name, c->name, sizeof(cs->cloneQueue[0].name));
                memcpy(cs->cloneQueue[cs->cloneCount].xp, c->xp, sizeof(c->xp));
                cs->cloneCount++;
            }
            crew_remove(cs, i--);
        }
    }

    // Clone bay: bring back the queued dead.
    int cb = ship_find_system(s, SYS_CLONEBAY);
    if (cb >= 0 && cs->cloneCount > 0) {
        int p = ship_system_power(s, cb);
        if (p > 0) {
            cs->cloneDying = 0.0f;
            cs->cloneProgress += dt / CLONE_SECONDS[p < 3 ? p - 1 : 2];
            const Room* r = &s->rooms[s->systems[cb].room];
            int tx = -1, ty = -1;
            for (int y = r->y; y < r->y + r->h && tx < 0; y++)
                for (int x = r->x; x < r->x + r->w && tx < 0; x++) {
                    bool used = false;
                    for (int k = 0; k < cs->count && !used; k++) used = !cs->crew[k].foreign && cs->crew[k].destX == x && cs->crew[k].destY == y;
                    if (!used) {
                        tx = x;
                        ty = y;
                    }
                }
            if (cs->cloneProgress >= 1.0f && tx >= 0 &&
                crew_add(cs, cs->cloneQueue[0].race, cs->cloneQueue[0].name, cs->cloneQueue[0].female, tx, ty)) {
                // Cloning costs some experience (wiki: Crew skills).
                Crew* clone = &cs->crew[cs->count - 1];
                memcpy(clone->xp, cs->cloneQueue[0].xp, sizeof(clone->xp));
                crew_clone_xp(clone->xp);
                for (int k = 1; k < cs->cloneCount; k++) cs->cloneQueue[k - 1] = cs->cloneQueue[k];
                cs->cloneCount--;
                cs->cloneProgress = 0.0f;
                sound_play("cloneArrive");
            }
        } else if (!s->aug[AUG_BACKUP_DNA]) {
            cs->cloneDying += dt / CLONE_LOSS_SECONDS;
            if (cs->cloneDying >= 1.0f) {
                cs->cloneCount--;
                cs->cloneDying = 0.0f;
            }
        }
    }
}

bool life_toggle_door(Ship* s, int door) {
    DoorState* d = &s->doorState[door];
    d->forced = !d->forced;
    sound_play(d->forced ? "doorOpen" : "doorClose");
    return true;
}

bool life_set_all_doors(Ship* s, bool open) {
    bool changed = false;
    for (int i = 0; i < s->doorCount; i++) {
        if (s->doorState[i].forced != open) changed = true;
        s->doorState[i].forced = open;
    }
    if (changed) sound_play(open ? "doorOpen" : "doorClose");
    return changed;
}

float life_ship_oxygen(const Ship* s) {
    float sum = 0.0f, area = 0.0f;
    for (int r = 0; r < s->roomCount; r++) {
        sum += s->oxygen[r] * room_area(s, r);
        area += room_area(s, r);
    }
    return area > 0.0f ? sum / area : 0.0f;
}
