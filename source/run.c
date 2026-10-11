#include "run.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "combat.h"
#include "items.h"
#include "life.h"
#include "profile.h"
#include "sound.h"
#include "text.h"
#include "weapon.h"

// Map generation, sector choice and the rebel fleet follow xftl (doc/sector-map, sector/*.kt);
// reward amounts are xftl's tables from the FTL binary (game/LootDropGenerator.kt); store prices
// and stock from the FTL wiki (Stores) and xftl (doc/stores).

// ---- Event database -----------------------------------------------------------------

#define MAX_DOCS 20
#define MAX_NAMED 2600

typedef struct {
    const char* name;
    const XNode* node;
} Named;

static XDoc sDocs[MAX_DOCS];
static int sDocCount;
static Named sEvents[MAX_NAMED];  // <event name> and <eventList name>
static int sEventCount;
static Named sTextLists[400];
static int sTextListCount;
static Named sShips[300];
static int sShipCount;
static Named sImageLists[60];
static int sImageListCount;
static XDoc sSectorDoc;
static const XNode* sShipDef;  // the ship the current beacon's event is about (<ship load=...>)
static EnemyStatus sEnemyStatus[8];  // <status target="enemy">, for the beacon's enemy ship
static int sEnemyStatusCount;

int run_take_enemy_status(EnemyStatus* out, int max) {
    int n = sEnemyStatusCount < max ? sEnemyStatusCount : max;
    memcpy(out, sEnemyStatus, sizeof(EnemyStatus) * n);
    sEnemyStatusCount = 0;
    return n;
}

// ---- Saving: event pointers as numbers ----------------------------------------------------------

static bool walk_find(const XNode* n, const XNode* want, int* id) {
    for (; n; n = n->next) {
        if (n == want) return true;
        (*id)++;
        if (walk_find(n->child, want, id)) return true;
    }
    return false;
}

static const XNode* walk_at(const XNode* n, int* left) {
    for (; n; n = n->next) {
        if ((*left)-- == 0) return n;
        const XNode* f = walk_at(n->child, left);
        if (f) return f;
    }
    return NULL;
}

// A node's number (1-based; 0 for NULL or not found) in a walk through every event file.
static intptr_t node_id(const XNode* n) {
    if (!n) return 0;
    int id = 0;
    for (int i = 0; i < sDocCount; i++)
        if (walk_find(sDocs[i].root, n, &id)) return id + 1;
    return 0;
}

static const XNode* node_at(intptr_t id) {
    if (id <= 0) return NULL;
    int left = (int)id - 1;
    for (int i = 0; i < sDocCount; i++) {
        const XNode* n = walk_at(sDocs[i].root, &left);
        if (n) return n;
    }
    return NULL;
}

void run_pack(Run* run) {
    memset(&run->view, 0, sizeof(run->view));
    for (int i = 0; i < MAX_BEACONS; i++) run->beacons[i].event = (const XNode*)node_id(run->beacons[i].event);
    for (int i = 0; i < 4; i++) run->delayedQuests[i] = (const XNode*)node_id(run->delayedQuests[i]);
}

void run_unpack(Run* run) {
    for (int i = 0; i < MAX_BEACONS; i++) run->beacons[i].event = node_at((intptr_t)run->beacons[i].event);
    for (int i = 0; i < 4; i++) run->delayedQuests[i] = node_at((intptr_t)run->delayedQuests[i]);
    sShipDef = NULL;
}

static int cmp_named(const void* a, const void* b) { return strcmp(((const Named*)a)->name, ((const Named*)b)->name); }

// Adds or replaces (later files override earlier ones, e.g. dlcEventsOverwrite).
static void add_named(Named* arr, int* count, int max, const char* name, const XNode* node) {
    for (int i = 0; i < *count; i++)
        if (!strcmp(arr[i].name, name)) {
            arr[i].node = node;
            return;
        }
    if (*count < max) arr[(*count)++] = (Named){name, node};
}

static const XNode* find_named(const Named* arr, int count, const char* name) {
    if (!name) return NULL;
    Named key = {name, NULL};
    const Named* r = bsearch(&key, arr, count, sizeof(Named), cmp_named);
    return r ? r->node : NULL;
}

bool run_load(FtlDat* dat) {
    static const char* const files[] = {
        "data/events.xml",        "data/newEvents.xml",       "data/events_boss.xml",     "data/events_crystal.xml",
        "data/events_engi.xml",   "data/events_fuel.xml",     "data/events_mantis.xml",   "data/events_nebula.xml",
        "data/events_pirate.xml", "data/events_rebel.xml",    "data/events_rock.xml",     "data/events_ships.xml",
        "data/events_slug.xml",   "data/events_zoltan.xml",   "data/nameEvents.xml",      "data/dlcEvents.xml",
        "data/dlcEvents_anaerobic.xml", "data/dlcEventsOverwrite.xml", "data/events_imageList.xml"};
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]) && sDocCount < MAX_DOCS; f++) {
        char* buf = (char*)ftldat_read(dat, files[f], NULL);
        if (!buf) continue;
        XDoc* doc = &sDocs[sDocCount];
        if (!xdoc_parse(doc, buf)) continue;
        sDocCount++;
        // Top-level elements live under <FTL> (or straight under the root in some files).
        const XNode* top = xchild(doc->root, "FTL") ? xchild(doc->root, "FTL") : doc->root;
        for (const XNode* n = top->child; n; n = n->next) {
            const char* name = xattr(n, "name");
            if (!name) continue;
            if (!strcmp(n->name, "event") || !strcmp(n->name, "eventList"))
                add_named(sEvents, &sEventCount, MAX_NAMED, name, n);
            else if (!strcmp(n->name, "textList"))
                add_named(sTextLists, &sTextListCount, 400, name, n);
            else if (!strcmp(n->name, "ship"))
                add_named(sShips, &sShipCount, 300, name, n);
            else if (!strcmp(n->name, "imageList") && !xattr(n, "ui"))  // skip the iPad variants
                add_named(sImageLists, &sImageListCount, 60, name, n);
        }
    }
    qsort(sEvents, sEventCount, sizeof(Named), cmp_named);
    qsort(sTextLists, sTextListCount, sizeof(Named), cmp_named);
    qsort(sShips, sShipCount, sizeof(Named), cmp_named);
    qsort(sImageLists, sImageListCount, sizeof(Named), cmp_named);
    char* sd = (char*)ftldat_read(dat, "data/sector_data.xml", NULL);
    if (sd) xdoc_parse(&sSectorDoc, sd);
    return sEventCount > 0;
}

void run_free(void) {
    for (int i = 0; i < sDocCount; i++) xdoc_free(&sDocs[i]);
    sDocCount = sEventCount = sTextListCount = sShipCount = sImageListCount = 0;
    xdoc_free(&sSectorDoc);
}

// Follows load="..." and picks from eventLists until it reaches an actual event.
static const XNode* resolve_event(const XNode* n) {
    for (int guard = 0; n && guard < 32; guard++) {
        const char* load = xattr(n, "load");
        if (load) {
            n = find_named(sEvents, sEventCount, load);
            continue;
        }
        if (!strcmp(n->name, "eventList")) {
            int count = 0;
            for (const XNode* c = n->child; c; c = c->next) count += !strcmp(c->name, "event");
            if (!count) return NULL;
            int pick = rand() % count;
            for (const XNode* c = n->child; c; c = c->next)
                if (!strcmp(c->name, "event") && pick-- == 0) {
                    n = c;
                    break;
                }
            continue;
        }
        return n;
    }
    return NULL;
}

// The words of a <text> node: by id, from a textList, or inline.
static const char* resolve_text(const XNode* t) {
    if (!t) return "";
    const char* id = xattr(t, "id");
    if (id) return text_get(id, "");
    const char* load = xattr(t, "load");
    if (load) {
        const XNode* list = find_named(sTextLists, sTextListCount, load);
        if (list) {
            int count = 0;
            for (const XNode* c = list->child; c; c = c->next) count += !strcmp(c->name, "text");
            int pick = count ? rand() % count : 0;
            for (const XNode* c = list->child; c; c = c->next)
                if (!strcmp(c->name, "text") && pick-- == 0) return resolve_text(c);
        }
        return "";
    }
    return t->text;
}

// ---- Backdrops ----------------------------------------------------------------------

// A random <img> from an imageList.
static const XNode* pick_image(const char* list) {
    const XNode* l = find_named(sImageLists, sImageListCount, list);
    int count = 0;
    for (const XNode* c = l ? l->child : NULL; c; c = c->next) count += !strcmp(c->name, "img");
    int pick = count ? rand() % count : 0;
    for (const XNode* c = l ? l->child : NULL; c; c = c->next)
        if (!strcmp(c->name, "img") && pick-- == 0) return c;
    return NULL;
}

void run_backdrop(Run* run, const char* back, const char* planet) {
    if (back) {
        const XNode* img = strcmp(back, "NONE") ? pick_image(back) : NULL;
        snprintf(run->bg, sizeof(run->bg), "%s%s", img ? "img/" : "", img ? img->text : "");
    }
    if (planet) {
        const XNode* img = strcmp(planet, "NONE") ? pick_image(planet) : NULL;
        snprintf(run->planet, sizeof(run->planet), "%s%s", img ? "img/" : "", img ? img->text : "");
        run->planetSize = img ? xattr_int(img, "w", 200) : 0;
        run->planetX = 0.15f + (rand() % 70) / 100.0f;
        run->planetY = 0.2f + (rand() % 60) / 100.0f;
    }
}

// ---- Rewards ---------------------------------------------------------------------------

static const int FUEL_DROPS[3][2] = {{1, 3}, {2, 4}, {3, 6}};
static const int MISSILE_DROPS[3][2] = {{1, 2}, {2, 4}, {4, 8}};
static const int DRONE_DROPS[3][2] = {{1, 1}, {1, 1}, {1, 2}};
static const float SCRAP_MULT[3][2] = {{0.5f, 0.7f}, {0.8f, 1.3f}, {1.3f, 1.55f}};

static int roll(const int r[2]) { return r[0] + rand() % (r[1] - r[0] + 1); }

static int tier_of(const char* level) {
    // FTL's files write these in any case ("low", "MED", "MEDIUM").
    if (!level) return 1;
    if (!strcasecmp(level, "LOW")) return 0;
    if (!strcasecmp(level, "HIGH")) return 2;
    if (!strcasecmp(level, "RANDOM")) return rand() % 3;
    return 1;  // MED, MEDIUM
}

// Scrap is paid as if a sector later on easy and a sector earlier on hard (Mike Hopley's reward
// tables: mikehopley.github.io/ftl-scrap).
static int scrap_sector(const Run* run) {
    int s = run->sector + (run->difficulty == DIFF_EASY ? 1 : run->difficulty == DIFF_HARD ? -1 : 0);
    return s < 0 ? 0 : s;
}

static int gen_scrap(int tier, int sector) {
    float m = SCRAP_MULT[tier][0] + (rand() / (float)RAND_MAX) * (SCRAP_MULT[tier][1] - SCRAP_MULT[tier][0]);
    return (int)((15 + sector * 6) * m);
}

typedef struct {
    int scrap, fuel, missiles, parts, hull, crew;
    char item[64];
    char note[320];  // e.g. a quest marker was added, or a clone bay saved someone
} Gains;

// One of fuel / missiles / drone parts (0/1/2) at a tier.
static void gen_resource(Gains* g, int which, int tier) {
    if (which == 0) g->fuel += roll(FUEL_DROPS[tier]);
    if (which == 1) g->missiles += roll(MISSILE_DROPS[tier]);
    if (which == 2) g->parts += roll(DRONE_DROPS[tier]);
}

static void auto_reward(Run* run, Gains* g, const char* type, int tier);
static RunCtx* sCtx;  // for item rewards inside auto_reward

// Scrap Recovery Arm: +10% each, rounded down; Repair Arm: 2 hull for 15% of the scrap (only when
// the hull needs it). Wiki: Augmentations.
static void apply_scrap_arms(const Run* run, const Ship* s, Gains* g) {
    if (g->scrap <= 0) return;
    int arms = 0, repair = 0;
    for (int i = 0; i < run->augCount; i++) {
        arms += !strcmp(run->augs[i], "SCRAP_COLLECTOR");
        repair += !strcmp(run->augs[i], "REPAIR_ARM");
    }
    g->scrap = (int)(g->scrap * (1.0f + 0.1f * arms));
    if (repair && s->hullHp + g->hull < s->maxHull) {
        g->scrap = (int)(g->scrap * 0.85f);
        g->hull += 2;
    }
}

static void auto_reward(Run* run, Gains* g, const char* type, int tier) {
    int fmd[3] = {0, 1, 2};
    for (int i = 2; i > 0; i--) {
        int j = rand() % (i + 1), t = fmd[i];
        fmd[i] = fmd[j];
        fmd[j] = t;
    }
    char t[24];
    snprintf(t, sizeof(t), "%s", type ? type : "standard");
    for (char* p = t; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    // xftl LootDropGenerator: standard rewards have a 3% chance of an item too, "stuff" 6%; the
    // item comes with more scrap.
    bool item = false;
    if (!strcmp(t, "standard")) {
        g->scrap += gen_scrap(tier, scrap_sector(run));
        gen_resource(g, fmd[0], 0);
        gen_resource(g, fmd[1], 0);
        item = rand() % 100 < 3;
    } else if (!strcmp(t, "stuff")) {
        g->scrap += gen_scrap(0, scrap_sector(run));
        gen_resource(g, fmd[0], tier);
        gen_resource(g, fmd[1], tier);
        item = rand() % 100 < 6;
    }
    if (item) {
        g->scrap += gen_scrap(tier, scrap_sector(run));
        if (sCtx && !g->item[0] && !run_give(run, sCtx, rand() % 3, "RANDOM", g->item, sizeof(g->item))) g->item[0] = '\0';
        return;
    }
    if (!strcmp(t, "standard") || !strcmp(t, "stuff")) {
        return;
    } else if (!strcmp(t, "fuel") || !strcmp(t, "missiles") || !strcmp(t, "droneparts")) {
        g->scrap += gen_scrap(tier, scrap_sector(run));
        gen_resource(g, t[0] == 'f' ? 0 : t[0] == 'm' ? 1 : 2, tier);
    } else if (!strcmp(t, "fuel_only")) {
        gen_resource(g, 0, tier);
    } else if (!strcmp(t, "missiles_only")) {
        gen_resource(g, 1, tier);
    } else if (!strcmp(t, "droneparts_only")) {
        gen_resource(g, 2, tier);
    } else if (!strcmp(t, "scrap_only")) {
        g->scrap += gen_scrap(tier, scrap_sector(run));
    } else {
        // weapon / drone / augment / item: scrap plus the item.
        g->scrap += gen_scrap(tier, scrap_sector(run));
        int kind = t[0] == 'w' ? ITEM_WEAPON : t[0] == 'd' ? ITEM_DRONE : t[0] == 'a' ? ITEM_AUGMENT : rand() % 3;
        if (sCtx && !g->item[0] && !run_give(run, sCtx, kind, "RANDOM", g->item, sizeof(g->item))) g->item[0] = '\0';
    }
}

// ---- Items -------------------------------------------------------------------------------

static bool usable(const ItemDef* it) {
    if (it->kind == ITEM_WEAPON) return weapon_find(it->name) && weapon_supported(weapon_find(it->name));
    if (it->kind == ITEM_DRONE) return drone_find(it->name) && drone_supported(drone_find(it->name));
    return true;
}

static const XNode* sector_description(const char* name);

// An item's rarity in this sector: its sector's <rarityList> entry (wiki: Stores, "every sector
// has a table of loot"), else the item's own.
static int sector_rarity(const Run* run, const ItemDef* it) {
    const XNode* desc = run ? sector_description(run->sectorType) : NULL;
    const XNode* list = desc ? xchild(desc, "rarityList") : NULL;
    for (const XNode* b = list ? list->child : NULL; b; b = b->next)
        if (!strcmp(b->name, "blueprint") && xattr(b, "name") && !strcmp(xattr(b, "name"), it->name))
            return xattr_int(b, "rarity", it->rarity);
    return it->rarity;
}

// A random item of a kind, commoner ones likelier (rarity 1 common .. 5 rare, 0 never), avoiding `skip`.
static const ItemDef* random_item(const Run* run, int kind, const char* const* skip, int skipCount) {
    const ItemDef* all[300];
    int n = items_of_kind(kind, all, 300), total = 0;
    int weight[300];
    for (int i = 0; i < n; i++) {
        int rarity = sector_rarity(run, all[i]);
        weight[i] = usable(all[i]) && rarity > 0 ? 6 - rarity : 0;
        for (int k = 0; k < skipCount; k++)
            if (skip[k] && !strcmp(skip[k], all[i]->name)) weight[i] = 0;
        if (weight[i] < 0) weight[i] = 0;
        total += weight[i];
    }
    if (!total) return NULL;
    int pick = rand() % total;
    for (int i = 0; i < n; i++)
        if ((pick -= weight[i]) < 0) return all[i];
    return NULL;
}

int run_beacon_env(const Run* run, int i) {
    const Beacon* b = &run->beacons[i];
    if (b->visited || !b->event) return b->env;
    static const char* const types[] = {"", "nebula", "storm", "asteroid", "sun", "pulsar", "PDS"};
    for (const XNode* n = b->event->child; n; n = n->next)
        if (!strcmp(n->name, "environment") && xattr(n, "type"))
            for (int k = 1; k < (int)(sizeof(types) / sizeof(types[0])); k++)
                if (!strcmp(xattr(n, "type"), types[k])) return k;
    return ENV_NONE;
}

bool run_beacon_scan(const Run* run, int i, int* env, bool* ship) {
    *env = ENV_NONE;
    *ship = false;
    const Beacon* b = &run->beacons[i];
    if (b->visited || !b->event) return false;
    if (run_beacon_overtaken(run, i)) {
        *ship = true;
        return true;
    }
    static const char* const types[] = {"", "nebula", "storm", "asteroid", "sun", "pulsar", "PDS"};
    for (const XNode* n = b->event->child; n; n = n->next) {
        if (!strcmp(n->name, "environment")) {
            const char* type = xattr(n, "type");
            for (int k = 1; type && k < (int)(sizeof(types) / sizeof(types[0])); k++)
                if (!strcmp(type, types[k])) *env = k;
        } else if (!strcmp(n->name, "ship")) {
            const char* hostile = xattr(n, "hostile");
            if (hostile && !strcmp(hostile, "true")) *ship = true;
        }
    }
    return true;
}

bool run_has_aug(const Run* run, const char* name) {
    for (int i = 0; i < run->augCount; i++)
        if (!strcmp(run->augs[i], name)) return true;
    return false;
}

static void add_crew(RunCtx* ctx, const char* race, const char* name);
void run_add_crew(RunCtx* ctx, const char* race, const char* name) { add_crew(ctx, race, name); }

bool run_give(Run* run, RunCtx* ctx, int kind, const char* name, char* got, size_t gotSize) {
    const ItemDef* it = NULL;
    if (kind == ITEM_CREW) {
        add_crew(ctx, name, NULL);
        snprintf(got, gotSize, "a new crew member");
        return true;
    }
    if (name && strcmp(name, "RANDOM")) {
        it = item_find(name);
        if (!it) {  // a blueprint list
            const char* list[48];
            int n = weapon_list(name, list, 48);
            for (int tries = 0; tries < 8 && n && (!it || !usable(it)); tries++) it = item_find(list[rand() % n]);
        }
    }
    if (!it || !usable(it)) it = random_item(run, kind, NULL, 0);
    if (!it) return false;
    snprintf(got, gotSize, "%s", it->title);
    Ship* s = ctx->ship;
    if (it->kind == ITEM_AUGMENT) {
        if (run->augCount >= MAX_AUGS) return false;
        snprintf(run->augs[run->augCount++], sizeof(run->augs[0]), "%s", it->name);
        return true;
    }
    bool weapon = it->kind == ITEM_WEAPON;
    if (weapon && s->weaponCount < s->weaponSlots && combat_add_weapon(s, ctx->dat, ctx->db, it->name)) return true;
    if (!weapon && s->droneCount < s->droneSlots && ship_find_system(s, SYS_DRONES) >= 0 &&
        combat_add_drone(s, ctx->dat, ctx->db, it->name))
        return true;
    for (int i = 0; i < CARGO_SLOTS; i++)
        if (!run->cargo[i][0]) {
            snprintf(run->cargo[i], sizeof(run->cargo[i]), "%s", it->name);
            return true;
        }
    return false;
}

// <upgrade amount system>: free power for a system or the reactor.
static void upgrade(RunCtx* ctx, const char* system, int amount) {
    Ship* s = ctx->ship;
    if (!system) return;
    if (!strcmp(system, "reactor")) {
        s->reactor += amount;
        if (s->reactor > REACTOR_MAX) s->reactor = REACTOR_MAX;
        return;
    }
    int si = ship_find_system(s, system_from_name(system));
    const SysBlueprint* sb = si >= 0 ? item_system(s->systems[si].def->type) : NULL;
    if (!sb) return;
    s->sysDefs[si].level += amount;
    if (s->sysDefs[si].level > sb->maxPower) s->sysDefs[si].level = sb->maxPower;
    if (system_is_subsystem(s->sysDefs[si].type)) s->systems[si].power = s->sysDefs[si].level;
}

// ---- Stores ------------------------------------------------------------------------------

static int system_total(const Ship* s) { return s->systemCount; }

// Systems the ship could install here (it has a room for them and doesn't have them).
static bool can_install(const Ship* s, int type) {
    if (ship_find_system(s, type) >= 0) return false;
    // A medbay and a clone bay can swap (the new one replaces the old: wiki, Clone Bay).
    for (int i = 0; i < s->bp->optionalCount; i++)
        if (s->bp->optional[i].type == type) return true;
    return false;
}

// Store layout from xftl (doc/stores): 2-4 sections; with fewer than 11 systems a 50% chance the
// first sells systems; no drone section without a drone system unless systems are on sale.
static void stock_store(Run* run, const Ship* s) {
    run->storeFuel = 3 + rand() % 5;
    run->storeMissiles = 2 + rand() % 5;
    run->storeParts = 2 + rand() % 3;
    memset(run->storeItem, 0, sizeof(run->storeItem));
    memset(run->storeSkill, 0, sizeof(run->storeSkill));
    run->storeSectionCount = 2 + rand() % 3;
    bool used[ITEM_KINDS] = {false};
    for (int i = 0; i < run->storeSectionCount; i++) {
        int kind;
        if (i == 0 && system_total(s) < 11 && rand() % 2)
            kind = ITEM_SYSTEM;
        else
            do kind = rand() % ITEM_KINDS; while (used[kind]);
        used[kind] = true;
        run->storeKind[i] = kind;
    }
    if (ship_find_system(s, SYS_DRONES) < 0 && !used[ITEM_SYSTEM])
        for (int i = 0; i < run->storeSectionCount; i++)
            if (run->storeKind[i] == ITEM_DRONE) {
                static const int prefer[4] = {ITEM_WEAPON, ITEM_AUGMENT, ITEM_CREW, ITEM_SYSTEM};
                for (int k = 0; k < 4; k++)
                    if (!used[prefer[k]]) {
                        used[ITEM_DRONE] = false;
                        used[prefer[k]] = true;
                        run->storeKind[i] = prefer[k];
                        break;
                    }
            }
    for (int sec = 0; sec < run->storeSectionCount; sec++) {
        int kind = run->storeKind[sec];
        if (kind == ITEM_SYSTEM) {
            // Forced: drones (if drones are sold), shields, a medical system; then random others.
            int list[SYS_COUNT], n = 0;
            if (used[ITEM_DRONE] && can_install(s, SYS_DRONES)) list[n++] = SYS_DRONES;
            if (can_install(s, SYS_SHIELDS)) list[n++] = SYS_SHIELDS;
            // With neither medical system, one is guaranteed: the clone bay, as in AE (xftl), or
            // the medbay if the ship has no room for a clone bay.
            if (ship_find_system(s, SYS_MEDBAY) < 0 && ship_find_system(s, SYS_CLONEBAY) < 0) {
                if (can_install(s, SYS_CLONEBAY)) list[n++] = SYS_CLONEBAY;
                else if (can_install(s, SYS_MEDBAY)) list[n++] = SYS_MEDBAY;
            }
            int others[SYS_COUNT], m = 0;
            for (int t = 0; t < SYS_COUNT; t++) {
                bool dup = false;
                for (int k = 0; k < n; k++) dup |= list[k] == t;
                // Medical systems only as a swap for the one the ship has (xftl doc/stores).
                bool medical = t == SYS_MEDBAY || t == SYS_CLONEBAY;
                bool hasMedical = ship_find_system(s, SYS_MEDBAY) >= 0 || ship_find_system(s, SYS_CLONEBAY) >= 0;
                if (!dup && item_system(t) && can_install(s, t) && (!medical || hasMedical)) others[m++] = t;
            }
            while (n < STORE_ITEMS && m > 0) {
                int k = rand() % m;
                list[n++] = others[k];
                others[k] = others[--m];
            }
            for (int k = 0; k < n && k < STORE_ITEMS; k++)
                snprintf(run->storeItem[sec][k], sizeof(run->storeItem[0][0]), "%s", system_name(list[k]));
        } else if (kind == ITEM_CREW) {
            for (int k = 0; k < STORE_ITEMS; k++) {  // duplicates allowed
                const ItemDef* it = random_item(run, ITEM_CREW, NULL, 0);
                if (it) snprintf(run->storeItem[sec][k], sizeof(run->storeItem[0][0]), "%s", it->name);
                // Trained hires, commoner later in the run (wiki: Crew skills gives no numbers; ours:
                // 8% a sector after the first, at most 60%; one or two skills; level 2 likelier later).
                int chance = run->sector * 8 > 60 ? 60 : run->sector * 8;
                for (int j = 0; j < 2 && rand() % 100 < chance; j++) {
                    int skill = rand() % SKILL_COUNT, level = rand() % 8 < run->sector ? 2 : 1;
                    if (j == 1 && skill == run->storeSkill[sec][k][0] / 4) break;
                    run->storeSkill[sec][k][j] = (unsigned char)(skill * 4 + level);
                }
            }
        } else {
            const char* taken[STORE_ITEMS] = {NULL};
            for (int k = 0; k < STORE_ITEMS; k++) {
                const ItemDef* it = random_item(run, kind, taken, k);
                if (!it) break;
                taken[k] = it->name;
                snprintf(run->storeItem[sec][k], sizeof(run->storeItem[0][0]), "%s", it->name);
            }
        }
    }
}

static void stock_copy(Run* run, StoreStock* s, bool save) {
#define X(a, b) \
    if (save) memcpy(&(a), &(b), sizeof(a)); \
    else memcpy(&(b), &(a), sizeof(a))
    X(s->fuel, run->storeFuel);
    X(s->missiles, run->storeMissiles);
    X(s->parts, run->storeParts);
    X(s->kind, run->storeKind);
    X(s->sectionCount, run->storeSectionCount);
    X(s->item, run->storeItem);
    X(s->skill, run->storeSkill);
#undef X
}

// Writes the open store's stock back to its beacon (after a purchase).
static void store_commit(Run* run) {
    int slot = run->beacons[run->current].storeSlot;
    if (slot > 0) stock_copy(run, &run->storeSaved[slot - 1], true);
}

void run_store_enter(Run* run, RunCtx* ctx) {
    Beacon* b = &run->beacons[run->current];
    if (b->storeSlot > 0) {
        stock_copy(run, &run->storeSaved[b->storeSlot - 1], false);
        return;
    }
    stock_store(run, ctx->ship);
    if (run->storeSavedCount < MAX_STORES) {
        b->storeSlot = (signed char)++run->storeSavedCount;
        store_commit(run);
    }
}

bool run_beacon_store(const Run* run, int i) {
    const Beacon* b = &run->beacons[i];
    return b->storeSlot > 0 || (b->event && xchild(b->event, "store"));
}

bool run_beacon_distress(const Run* run, int i) {
    const Beacon* b = &run->beacons[i];
    return !b->visited && b->event && xchild(b->event, "distressBeacon");
}

int run_store_price(const Run* run, int sec, int slot) {
    const char* name = run->storeItem[sec][slot];
    if (!name[0]) return 0;
    if (run->storeKind[sec] == ITEM_SYSTEM) {
        const SysBlueprint* sb = item_system(system_from_name(name));
        return sb ? sb->cost : 0;
    }
    const ItemDef* it = item_find(name);
    return it ? it->cost : 0;
}

bool run_store_buy(Run* run, RunCtx* ctx, int sec, int slot) {
    char* name = run->storeItem[sec][slot];
    int price = run_store_price(run, sec, slot);
    if (!name[0] || run->scrap < price) return false;
    Ship* s = ctx->ship;
    int kind = run->storeKind[sec];
    if (kind == ITEM_SYSTEM) {
        int type = system_from_name(name);
        const SystemDef* def = NULL;
        for (int i = 0; i < s->bp->optionalCount; i++)
            if (s->bp->optional[i].type == type) def = &s->bp->optional[i];
        if (!def || !can_install(s, type)) return false;
        int other = type == SYS_MEDBAY ? ship_find_system(s, SYS_CLONEBAY) : type == SYS_CLONEBAY ? ship_find_system(s, SYS_MEDBAY) : -1;
        if (other >= 0) {
            if (ctx->waitGpu) ctx->waitGpu();
            if (ship_replace_system(s, ctx->dat, other, def) < 0) return false;
            if (type == SYS_MEDBAY) ctx->crew->cloneCount = 0;  // no clone bay, no queue
        } else if (ship_install_system(s, ctx->dat, def) < 0) {
            return false;
        }
    } else if (kind == ITEM_CREW) {
        int before = crew_native_count(ctx->crew);
        if (before >= CREW_LIMIT) return false;
        add_crew(ctx, name, NULL);
        if (crew_native_count(ctx->crew) == before) return false;  // nowhere to put them
        Crew* m = &ctx->crew->crew[ctx->crew->count - 1];
        for (int j = 0; j < 2; j++) {
            int v = run->storeSkill[sec][slot][j];
            if (v) crew_set_skill_level(ctx->crew, m, v / 4, v % 4);
        }
    } else {
        char got[64];
        if (!run_give(run, ctx, kind, name, got, sizeof(got))) return false;
    }
    run->scrap -= price;
    name[0] = '\0';
    store_commit(run);
    sound_play("buy");
    return true;
}

// ---- Applying an event ------------------------------------------------------------------

static void append(char* buf, size_t size, const char* s) {
    size_t n = strlen(buf);
    if (n < size - 1) snprintf(buf + n, size - n, "%s", s);
}

// How much of a requirement we have: a system's level (0 without it), reactor bars, how many crew
// of a race, or 1/0 for a weapon, drone or augment we carry (by name or blueprint list).
static int requirement_level(const Run* run, const RunCtx* ctx, const char* req) {
    const Ship* s = ctx->ship;
    int sys = system_from_name(req);
    if (sys >= 0) {
        int si = ship_find_system(s, sys);
        return si >= 0 ? s->systems[si].def->level : 0;
    }
    if (!strcmp(req, "reactor")) return s->reactor;
    const char* race = !strcmp(req, "zoltan") ? "energy" : !strcmp(req, "lanius") ? "anaerobic" : req;
    int crew = 0;
    for (int i = 0; i < ctx->crew->count; i++)
        if (!ctx->crew->crew[i].foreign && !ctx->crew->crew[i].drone && !strcmp(ctx->crew->races[ctx->crew->crew[i].race].name, race))
            crew++;
    if (crew) return crew;
    const char* list[48];
    int n = weapon_list(req, list, 48);
    if (!n) {
        list[0] = req;
        n = 1;
    }
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < s->weaponCount; i++)
            if (!strcmp(s->weapons[i].def->name, list[k])) return 1;
        for (int i = 0; i < s->droneCount; i++)
            if (!strcmp(s->drones[i].def->name, list[k])) return 1;
        for (int i = 0; i < run->augCount; i++)
            if (!strcmp(run->augs[i], list[k])) return 1;
        for (int i = 0; i < CARGO_SLOTS; i++)
            if (!strcmp(run->cargo[i], list[k])) return 1;
    }
    return 0;
}

// FTL's choice requirements: shown when lvl (default 1) <= our level <= max_lvl (if given).
static bool requirement_met(const Run* run, const RunCtx* ctx, const XNode* choice) {
    const char* req = xattr(choice, "req");
    if (!req) return true;
    int have = requirement_level(run, ctx, req);
    int lvl = xattr_int(choice, "lvl", 1), maxLvl = xattr_int(choice, "max_lvl", 1 << 30);
    return have >= lvl && have <= maxLvl;
}

// <boarders min max class breach>: hostile crew beam aboard our ship, all into one room (a random
// one with space), as intruders: they fight our crew and sabotage systems like any boarders.
// "ghost" crew have no art of their own in ftl.dat: they're drawn as humans.
static int add_boarders(RunCtx* ctx, const XNode* n) {
    CrewSet* cs = ctx->crew;
    Ship* s = ctx->ship;
    int mn = xattr_int(n, "min", 1), mx = xattr_int(n, "max", mn);
    int count = mn + (mx > mn ? rand() % (mx - mn + 1) : 0);
    const char* cls = xattr(n, "class");
    if (!cls || !strcmp(cls, "ghost")) cls = cls ? "human" : "random";
    const char* breach = xattr(n, "breach");
    if (s->roomCount == 0) return 0;
    int start = rand() % s->roomCount, added = 0;
    for (int k = 0; k < s->roomCount && added < count; k++) {
        int r = (start + k) % s->roomCount;
        const Room* rm = &s->rooms[r];
        int here = 0;
        for (int y = rm->y; y < rm->y + rm->h && added < count; y++)
            for (int x = rm->x; x < rm->x + rm->w && added < count; x++) {
                bool used = false;
                for (int i = 0; i < cs->count && !used; i++) used = cs->crew[i].foreign && cs->crew[i].destX == x && cs->crew[i].destY == y;
                if (used || cs->count >= MAX_CREW) continue;
                const char* race = cls;
                if (!strcmp(race, "random")) {
                    static const char* const races[] = {"human", "engi", "mantis", "rock", "energy", "slug"};
                    race = races[rand() % 6];
                }
                if (!strcmp(race, "zoltan")) race = "energy";
                if (!strcmp(race, "lanius")) race = "anaerobic";
                bool female = !strcmp(race, "human") && (rand() & 1);
                int ri = crew_race_look(cs, race, female ? "female" : race);
                if (ri < 0) ri = crew_race(cs, race);
                if (ri < 0) continue;
                const char* nm = race;
                if (ctx->names && ctx->names->count) nm = ctx->names->names[rand() % ctx->names->count];
                if (!crew_add(cs, ri, nm, female, x, y)) continue;
                cs->crew[cs->count - 1].foreign = true;
                added++;
                here++;
            }
        if (here && breach && !strcmp(breach, "true")) life_breach(s, r);
    }
    return added;
}

static void add_crew(RunCtx* ctx, const char* race, const char* name) {
    if (!race || !strcmp(race, "random") || !strcmp(race, "traitor")) {
        static const char* const races[] = {"human", "engi", "mantis", "rock", "energy", "slug"};
        race = races[rand() % 6];
    }
    if (!strcmp(race, "zoltan")) race = "energy";
    if (!strcmp(race, "lanius")) race = "anaerobic";
    bool female = !strcmp(race, "human") && (rand() & 1);
    int ri = crew_race_look(ctx->crew, race, female ? "female" : race);
    if (ri < 0) {
        female = false;
        ri = crew_race(ctx->crew, race);
    }
    if (ri < 0 || crew_native_count(ctx->crew) >= CREW_LIMIT) return;
    // Any free tile.
    for (int r = 0; r < ctx->ship->roomCount; r++) {
        const Room* rm = &ctx->ship->rooms[r];
        for (int y = rm->y; y < rm->y + rm->h; y++)
            for (int x = rm->x; x < rm->x + rm->w; x++) {
                bool used = false;
                for (int k = 0; k < ctx->crew->count && !used; k++) used = !ctx->crew->crew[k].foreign && ctx->crew->crew[k].destX == x && ctx->crew->crew[k].destY == y;
                if (used) continue;
                const char* nm = name;
                // A name to match (human names come as male and female lists).
                for (int tries = 0; !nm && ctx->names && ctx->names->count && tries < 30; tries++) {
                    int pick = rand() % ctx->names->count;
                    if (strcmp(race, "human") || ctx->names->female[pick] == female || tries == 29) nm = ctx->names->names[pick];
                }
                crew_add(ctx->crew, ri, nm ? nm : race, female, x, y);
                return;
            }
    }
}

static int jumps_between(const Run* run, int from, int to);

// xftl Sector.addQuest: a random unvisited beacon that isn't the exit, a store or another quest,
// is reachable, and (unless ignoreDistance) the fleet won't overrun before we can get there.
static bool place_quest(Run* run, const XNode* ev, bool ignoreDistance) {
    int ok[MAX_BEACONS], n = 0;
    for (int i = 0; i < run->beaconCount; i++) {
        const Beacon* b = &run->beacons[i];
        if (b->visited || i == run->current || b->exit || b->quest || !strcmp(b->eventName, "STORE")) continue;
        if (run_beacon_overtaken(run, i)) continue;
        int path = jumps_between(run, run->current, i);
        if (path < 0) continue;
        if (!ignoreDistance) {
            float dx = b->x - run->fleetX, dy = b->y - run->fleetY;
            float turns = (sqrtf(dx * dx + dy * dy) - 767.0f) / 64.0f;
            if (turns <= path) continue;
        }
        ok[n++] = i;
    }
    if (!n) return false;
    Beacon* b = &run->beacons[ok[rand() % n]];
    b->quest = true;
    b->event = resolve_event(ev);
    snprintf(b->eventName, sizeof(b->eventName), "QUEST");
    return true;
}

static const char* const ITEM_TYPES[4] = {"scrap", "fuel", "missiles", "drones"};

// Rolls an event's <item_modify> amounts (scrap, fuel, missiles, drone parts). False if it has none.
static bool roll_items(const XNode* ev, int out[4]) {
    const XNode* im = ev ? xchild(ev, "item_modify") : NULL;
    memset(out, 0, sizeof(int) * 4);
    if (!im) return false;
    for (const XNode* it = im->child; it; it = it->next) {
        if (strcmp(it->name, "item")) continue;
        const char* type = xattr(it, "type");
        int mn = xattr_int(it, "min", 0), mx = xattr_int(it, "max", mn);
        if (mx < mn) {
            int t = mn;
            mn = mx;
            mx = t;
        }
        int amount = mn + (mx > mn ? rand() % (mx - mn + 1) : 0);
        for (int k = 0; type && k < 4; k++)
            if (!strcmp(type, ITEM_TYPES[k])) out[k] += amount;
    }
    return true;
}

static void show_event_with(Run* run, RunCtx* ctx, const XNode* ev, const int* fixedItems);
static void show_event(Run* run, RunCtx* ctx, const XNode* ev) { show_event_with(run, ctx, ev, NULL); }

// Applies everything an event does, then fills in the view (text, rewards, choices). fixedItems:
// the <item_modify> amounts already rolled for a choice's preview, or NULL to roll them now.
static void show_event_with(Run* run, RunCtx* ctx, const XNode* ev, const int* fixedItems) {
    EventView* v = &run->view;
    memset(v, 0, sizeof(*v));
    ev = resolve_event(ev);
    if (!ev) return;
    v->active = true;
    snprintf(v->text, sizeof(v->text), "%s", resolve_text(xchild(ev, "text")));
    bool noText = !v->text[0];

    Gains g;
    memset(&g, 0, sizeof(g));
    Ship* s = ctx->ship;
    sCtx = ctx;
    for (const XNode* n = ev->child; n; n = n->next) {
        if (!strcmp(n->name, "autoReward")) {
            auto_reward(run, &g, n->text, tier_of(xattr(n, "level")));
        } else if (!strcmp(n->name, "item_modify")) {
            int items[4];
            if (fixedItems)
                memcpy(items, fixedItems, sizeof(items));
            else
                roll_items(ev, items);
            g.scrap += items[0];
            g.fuel += items[1];
            g.missiles += items[2];
            g.parts += items[3];
        } else if (!strcmp(n->name, "damage")) {
            // xftl InGameState: the hull always takes `amount`; with system="X" / "random" / "room"
            // that room's system takes it too, and effect="fire" (two fires) / "breach" hit there.
            int amount = xattr_int(n, "amount", 0);
            const char* sys = xattr(n, "system");
            g.hull -= amount;
            if (sys && s->roomCount > 0) {
                int room = -1;
                if (!strcmp(sys, "random") && s->systemCount > 0) room = s->systems[rand() % s->systemCount].room;
                else if (strcmp(sys, "room") && strcmp(sys, "random")) {
                    int si = ship_find_system(s, system_from_name(sys));
                    if (si >= 0) room = s->systems[si].room;
                }
                if (room < 0) room = rand() % s->roomCount;
                int si = ship_system_in_room(s, room);
                if (si >= 0 && amount > 0) {
                    s->systems[si].damage += amount;
                    if (s->systems[si].damage > s->systems[si].def->level) s->systems[si].damage = s->systems[si].def->level;
                    s->systems[si].repair = 0.0f;
                }
                // effect="fire" (two fires), "breach", "all" (both: xftl) or "random" (one or the
                // other: our reading).
                const char* effect = xattr(n, "effect");
                bool fire = effect && (!strcmp(effect, "fire") || !strcmp(effect, "all"));
                bool breach = effect && (!strcmp(effect, "breach") || !strcmp(effect, "all"));
                if (effect && !strcmp(effect, "random")) {
                    if (rand() & 1) fire = true;
                    else breach = true;
                }
                if (fire) {
                    life_ignite(s, room);
                    life_ignite(s, room);
                }
                if (breach) life_breach(s, room);
            }
        } else if (!strcmp(n->name, "crewMember")) {
            int amount = xattr_int(n, "amount", 1);
            for (int k = 0; k < amount; k++) {
                const char* id = xattr(n, "id");
                int before = crew_native_count(ctx->crew);
                add_crew(ctx, xattr(n, "class"), id ? text_get(id, NULL) : n->text[0] ? n->text : NULL);
                g.crew += crew_native_count(ctx->crew) - before;  // nothing if the crew is full
                if (crew_native_count(ctx->crew) > before) {
                    // Trained recruits: all_skills="1", pilot="2", ... are skill levels.
                    static const char* const attrs[SKILL_COUNT] = {"pilot", "engines", "shields", "weapons", "repair", "combat"};
                    Crew* m = &ctx->crew->crew[ctx->crew->count - 1];
                    int all = xattr_int(n, "all_skills", 0);
                    for (int sk = 0; sk < SKILL_COUNT; sk++) {
                        int lvl = xattr_int(n, attrs[sk], all);
                        if (lvl > 0) crew_set_skill_level(ctx->crew, m, sk, lvl);
                    }
                }
            }
        } else if (!strcmp(n->name, "removeCrew")) {
            // A random crew member is lost (never the last), or one of a race (class="engi"). With
            // <clone>true</clone> a clone bay brings them back, and the event's own text says so.
            CrewSet* cs = ctx->crew;
            const char* cls = xattr(n, "class");
            if (cls && !strcmp(cls, "zoltan")) cls = "energy";
            if (cls && !strcmp(cls, "lanius")) cls = "anaerobic";
            int native[MAX_CREW], cnt = 0, total = 0;
            for (int i = 0; i < cs->count; i++) {
                if (cs->crew[i].foreign || cs->crew[i].drone) continue;
                total++;
                if (!cls || !strcmp(cs->races[cs->crew[i].race].name, cls)) native[cnt++] = i;
            }
            if (cnt > 0 && total > 1) {
                int i = native[rand() % cnt];
                const XNode* cl = xchild(n, "clone");
                bool clone = cl && !strcmp(cl->text, "true") && ship_find_system(s, SYS_CLONEBAY) >= 0 && cs->cloneCount < MAX_CREW;
                if (clone) {
                    const Crew* m = &cs->crew[i];
                    cs->cloneQueue[cs->cloneCount].race = m->race;
                    cs->cloneQueue[cs->cloneCount].female = m->female;
                    memcpy(cs->cloneQueue[cs->cloneCount].name, m->name, sizeof(cs->cloneQueue[0].name));
                    memcpy(cs->cloneQueue[cs->cloneCount].xp, m->xp, sizeof(m->xp));
                    cs->cloneCount++;
                    const char* ct = resolve_text(xchild(n, "text"));
                    if (ct[0]) snprintf(g.note, sizeof(g.note), "%s", ct);
                } else {
                    g.crew--;
                }
                crew_remove(cs, i);
            }
        } else if (!strcmp(n->name, "ship")) {
            // A ship appears (load=...) and may turn hostile later in the event.
            if (xattr(n, "load")) sShipDef = find_named(sShips, sShipCount, xattr(n, "load"));
            const char* hostile = xattr(n, "hostile");
            if (hostile && !strcasecmp(hostile, "true")) {
                v->ship = n;
                v->shipDef = sShipDef;
            }
            if (hostile && !strcasecmp(hostile, "false") && !xattr(n, "load")) v->peace = true;
        } else if (!strcmp(n->name, "boarders")) {
            int got = add_boarders(ctx, n);
            if (got) snprintf(g.note, sizeof(g.note), "%d intruder%s on board!", got, got == 1 ? "" : "s");
        } else if (!strcmp(n->name, "status")) {
            // System limits at this beacon (xftl EventStatus): ours now, the enemy's when it's here.
            static const char* const ops[] = {"limit", "divide", "loss", "clear"};
            const char* type = xattr(n, "type");
            const char* sysName = xattr(n, "system");
            const char* target = xattr(n, "target");
            int op = -1;
            for (int k = 0; type && k < 4; k++)
                if (!strcmp(type, ops[k])) op = k;
            int sysType = sysName ? system_from_name(!strcmp(sysName, "engine") ? "engines" : sysName) : -1;
            int amount = xattr_int(n, "amount", 0);
            if (op >= 0 && sysType >= 0) {
                if (target && !strcmp(target, "enemy")) {
                    if (sEnemyStatusCount < 8) sEnemyStatus[sEnemyStatusCount++] = (EnemyStatus){sysType, op, amount};
                } else {
                    ship_event_status(s, sysType, op, amount);
                }
            }
        } else if (!strcmp(n->name, "img")) {
            run_backdrop(run, xattr(n, "back"), xattr(n, "planet"));
        } else if (!strcmp(n->name, "store")) {
            v->store = true;
        } else if (!strcmp(n->name, "environment")) {
            static const char* const types[] = {"", "nebula", "storm", "asteroid", "sun", "pulsar", "PDS"};
            const char* type = xattr(n, "type");
            for (int k = 1; type && k < (int)(sizeof(types) / sizeof(types[0])); k++)
                if (!strcmp(type, types[k])) run->env = k;
            const char* target = xattr(n, "target");
            run->envTarget = !target || !strcmp(target, "player") ? ENV_TARGET_PLAYER
                             : !strcmp(target, "enemy")             ? ENV_TARGET_ENEMY
                                                                    : ENV_TARGET_PLAYER | ENV_TARGET_ENEMY;
            run->beacons[run->current].env = run->env;
            run->beacons[run->current].envTarget = run->envTarget;
            // FTL's scenery for the hazard (xftl SunEnvironment / PulsarEnvironment): a plain
            // starfield with the sun filling the left, or the pulsar up and to the left; asteroid
            // fields add drifting rock layers (drawn by the game screen).
            if (run->env == ENV_SUN || run->env == ENV_PULSAR || run->env == ENV_ASTEROID) {
                snprintf(run->bg, sizeof(run->bg), "img/stars/bg_dullstars.png");
                run->planet[0] = '\0';
                run->planetSize = 0;
            }
            if (run->env == ENV_SUN) {
                snprintf(run->planet, sizeof(run->planet), "img/stars/planet_sun1.png");
                run->planetSize = 804;
                run->planetX = 502.0f / 1280.0f;
                run->planetY = 479.0f / 720.0f;
            } else if (run->env == ENV_PULSAR) {
                snprintf(run->planet, sizeof(run->planet), "img/effects/pulsar_white.png");
                run->planetSize = 226;
                run->planetX = 463.0f / 1280.0f;
                run->planetY = 203.0f / 720.0f;
            }
        } else if (!strcmp(n->name, "modifyPursuit")) {
            // Positive: the fleet gains a jump on us (a warning ship got away); negative: delayed.
            run->fleetX += xattr_int(n, "amount", 0) * 64.0f;
        } else if (!strcmp(n->name, "reveal_map")) {
            run->revealed = true;
        } else if (!strcmp(n->name, "weapon") || !strcmp(n->name, "drone") || !strcmp(n->name, "augment")) {
            int kind = n->name[0] == 'w' ? ITEM_WEAPON : n->name[0] == 'd' ? ITEM_DRONE : ITEM_AUGMENT;
            if (!run_give(run, ctx, kind, xattr(n, "name"), g.item, sizeof(g.item)))
                snprintf(g.item, sizeof(g.item), "nothing (no room in the hold)");
        } else if (!strcmp(n->name, "remove")) {
            // <remove name="X"/>: the item goes, from wherever we keep it.
            const char* name = xattr(n, "name");
            bool gone = false;
            for (int i = 0; name && !gone && i < run->augCount; i++)
                if (!strcmp(run->augs[i], name)) {
                    for (int k = i; k < run->augCount - 1; k++) memcpy(run->augs[k], run->augs[k + 1], sizeof(run->augs[k]));
                    run->augCount--;
                    gone = true;
                }
            for (int i = 0; name && !gone && i < CARGO_SLOTS; i++)
                if (!strcmp(run->cargo[i], name)) {
                    run->cargo[i][0] = '\0';
                    gone = true;
                }
            for (int i = 0; name && !gone && i < s->droneCount; i++)
                if (!strcmp(s->drones[i].def->name, name) && !s->drones[i].deployed) {
                    if (ctx->waitGpu) ctx->waitGpu();
                    combat_remove_drone(s, i);
                    gone = true;
                }
            for (int i = 0; name && !gone && i < s->weaponCount; i++)
                if (!strcmp(s->weapons[i].def->name, name)) {
                    if (ctx->waitGpu) ctx->waitGpu();
                    combat_remove_weapon(s, i);
                    gone = true;
                }
        } else if (!strcmp(n->name, "upgrade")) {
            upgrade(ctx, xattr(n, "system"), xattr_int(n, "amount", 1));
        } else if (!strcmp(n->name, "unlockShip")) {
            if (!run->sandbox && profile_unlock(xattr_int(n, "id", -1), 0)) snprintf(g.note, sizeof(g.note), "A new ship is available in the hangar!");
        } else if (!strcmp(n->name, "quest")) {
            // A quest marker in this sector, else the next one (xftl InGameState.addQuest:
            // dropped from sector 7 on, so none land in the Last Stand).
            const XNode* q = find_named(sEvents, sEventCount, xattr(n, "event"));
            const char* note = NULL;
            if (q && place_quest(run, q, false))
                note = text_get("added_quest", "Added a quest marker to your map!");
            else if (q && run->sector < 6 && run->delayedCount < 4) {
                run->delayedQuests[run->delayedCount++] = q;
                note = text_get("added_quest_sector", "Added a quest marker to the next sector!");
            }
            if (note) snprintf(g.note, sizeof(g.note), "%s", note);
        }
    }

    apply_scrap_arms(run, s, &g);
    // Apply and summarise.
    run->scrap += g.scrap;
    if (run->scrap < 0) run->scrap = 0;
    run->fuel += g.fuel;
    if (run->fuel < 0) run->fuel = 0;
    s->missiles += g.missiles;
    if (s->missiles < 0) s->missiles = 0;
    s->droneParts += g.parts;
    if (s->droneParts < 0) s->droneParts = 0;
    s->hullHp += g.hull;
    if (s->hullHp > s->maxHull) s->hullHp = s->maxHull;
    char line[128];
    const struct { int v; const char* n; } parts[] = {{g.scrap, "scrap"}, {g.fuel, "fuel"}, {g.missiles, "missiles"},
                                                      {g.parts, "drone parts"}, {g.hull, "hull"}, {g.crew, "crew"}};
    bool any = false;
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if (!parts[i].v) continue;
        snprintf(line, sizeof(line), "%s%s%+d %s", any ? ", " : "", any ? "" : "\n\n", parts[i].v, parts[i].n);
        append(v->text, sizeof(v->text), line);
        any = true;
    }
    if (g.item[0]) {
        snprintf(line, sizeof(line), "\n\nReceived: %s", g.item);
        append(v->text, sizeof(v->text), line);
    }
    if (g.note[0]) {
        append(v->text, sizeof(v->text), "\n\n");
        append(v->text, sizeof(v->text), g.note);
    }
    if (v->store) run_store_enter(run, ctx);

    // Choices, skipping blue options we don't qualify for.
    // FTL (xftl Choice): a choice whose requirement we don't meet is hidden if it's hidden="true",
    // else shown greyed out; hidden="true" also keeps its rewards off the choice's text.
    int groupsShown[8], groupCount = 0;  // max_group: one choice per group
    bool reqLocked[MAX_CHOICES] = {false};
    for (const XNode* c = ev->child; c && v->choiceCount < MAX_CHOICES; c = c->next) {
        if (strcmp(c->name, "choice")) continue;
        const char* req = xattr(c, "req");
        const char* hiddenAttr = xattr(c, "hidden");
        bool hidden = hiddenAttr && !strcasecmp(hiddenAttr, "true");
        bool met = requirement_met(run, ctx, c);
        if (!met && hidden) continue;
        if (xattr(c, "max_group")) {
            int grp = xattr_int(c, "max_group", 0);
            bool seen = false;
            for (int k = 0; k < groupCount; k++) seen |= groupsShown[k] == grp;
            if (seen) continue;
            if (groupCount < 8) groupsShown[groupCount++] = grp;
        }
        int i = v->choiceCount++;
        snprintf(v->choiceText[i], sizeof(v->choiceText[i]), "%s", resolve_text(xchild(c, "text")));
        if (!v->choiceText[i][0]) snprintf(v->choiceText[i], sizeof(v->choiceText[i]), "Continue...");
        const char* blue = xattr(c, "blue");
        v->choiceBlue[i] = met && req != NULL && !(blue && !strcmp(blue, "false"));
        if (!met) {
            v->choiceDisabled[i] = true;
            reqLocked[i] = true;
        }
        static const char* const races[] = {"human", "engi", "mantis", "rock", "zoltan", "energy", "slug", "crystal", "lanius", "anaerobic"};
        for (size_t k = 0; req && k < sizeof(races) / sizeof(races[0]); k++) v->choiceCrew[i] |= !strcmp(req, races[k]);
        // Like FTL, the outcome is picked now (a random event from a list, its resource amounts)
        // so the choice can say what it costs, and can't be taken if we can't pay.
        const XNode* ce = xchild(c, "event");
        const XNode* rv = ce ? resolve_event(ce) : NULL;
        v->choiceEvent[i] = rv ? rv : ce;
        v->choiceHasItems[i] = roll_items(rv, v->choiceItems[i]);
        if (v->choiceHasItems[i]) {
            static const char* const labels[4] = {"Scrap", "Fuel", "Missiles", "Drone parts"};
            const int have[4] = {run->scrap, run->fuel, s->missiles, s->droneParts};
            char preview[96] = "";
            for (int k = 0; k < 4; k++) {
                int a = v->choiceItems[i][k];
                if (!a) continue;
                char part[32];
                snprintf(part, sizeof(part), "%s%s %+d", preview[0] ? ", " : "", labels[k], a);
                append(preview, sizeof(preview), part);
                if (a < 0 && have[k] < -a) v->choiceDisabled[i] = true;
            }
            if (preview[0] && !hidden) {
                char line[120];
                snprintf(line, sizeof(line), " [%s]", preview);
                append(v->choiceText[i], sizeof(v->choiceText[i]), line);
            }
        }
    }
    // Never leave a dialog with nothing to pick: if every choice costs too much, the last one we
    // qualify for opens up.
    bool anyOpen = false;
    for (int i = 0; i < v->choiceCount; i++) anyOpen |= !v->choiceDisabled[i];
    for (int i = v->choiceCount - 1; i >= 0 && !anyOpen; i--)
        if (!reqLocked[i]) {
            v->choiceDisabled[i] = false;
            anyOpen = true;
        }
    if (v->choiceCount > 0 && !anyOpen) v->choiceDisabled[v->choiceCount - 1] = false;
    if (v->choiceCount == 0) {
        // FTL opens no window for an event with nothing to say (e.g. one that only brings a
        // ship): its effects apply silently; with rewards, the window shows just those.
        if (noText) {
            const char* p = v->text;
            while (*p == '\n') p++;
            if (!*p) {
                v->active = false;
                return;
            }
            memmove(v->text, p, strlen(p) + 1);
        }
        v->choiceCount = 1;
        snprintf(v->choiceText[0], sizeof(v->choiceText[0]), "Continue...");
        v->choiceEvent[0] = NULL;
    }
}

void run_show_named(Run* run, RunCtx* ctx, const char* name) {
    const XNode* ev = find_named(sEvents, sEventCount, name);
    if (ev) show_event(run, ctx, ev);
}

void run_choose(Run* run, RunCtx* ctx, int choice) {
    EventView* v = &run->view;
    if (!v->active || choice < 0 || choice >= v->choiceCount) return;
    if (v->choiceDisabled[choice]) return;  // can't afford it
    if (v->choiceBlue[choice] && v->choiceCrew[choice]) run->feats.blueCrewUses++;
    const XNode* next = v->choiceEvent[choice];
    // A fight or store asked for earlier in the dialog still happens once it closes.
    const XNode* ship = v->ship;
    const XNode* def = v->shipDef;
    bool store = v->store, peace = v->peace;
    int items[4];
    bool fixed = v->choiceHasItems[choice];
    memcpy(items, v->choiceItems[choice], sizeof(items));
    if (next)
        show_event_with(run, ctx, next, fixed ? items : NULL);
    else
        memset(v, 0, sizeof(*v));
    if (!v->ship && ship) {
        v->ship = ship;
        v->shipDef = def;
    }
    v->store |= store;
    v->peace |= peace;
    if (!v->shipDef) v->shipDef = def;  // still fighting the same ship after a mid-fight dialog
}

static int roll_threshold(const XNode* n) {
    if (!n || !xattr(n, "min")) return 0;
    const char* ch = xattr(n, "chance");
    if (ch && rand() / (float)RAND_MAX < atof(ch)) return 0;
    int mn = xattr_int(n, "min", 0), mx = xattr_int(n, "max", mn);
    return mn + (mx > mn ? rand() % (mx - mn + 1) : 0);
}

void run_ship_thresholds(const Run* run, int* surrender, int* escape, float* escapeTimer) {
    const XNode* def = run->view.shipDef;
    const XNode* e = def ? xchild(def, "escape") : NULL;
    *surrender = roll_threshold(def ? xchild(def, "surrender") : NULL);
    *escape = roll_threshold(e);
    if (*escape && *escape == *surrender) *escape = *surrender - 1;  // xftl: never both at once
    *escapeTimer = e && xattr(e, "timer") ? (float)atof(xattr(e, "timer")) : 15.0f;
}

bool run_ship_event(Run* run, RunCtx* ctx, const char* which) {
    const XNode* def = run->view.shipDef;
    const XNode* ev = def ? xchild(def, which) : NULL;
    if (!ev) return false;
    // The event replaces the view, but the fight's ship must stay known.
    show_event(run, ctx, ev);
    run->view.shipDef = def;
    return run->view.active;
}

void run_fight_won(Run* run, RunCtx* ctx, bool destroyed) {
    const XNode* def = run->view.shipDef;
    memset(&run->view, 0, sizeof(run->view));
    const XNode* ev = def ? xchild(def, destroyed ? "destroyed" : "deadCrew") : NULL;
    if (!ev && def) ev = xchild(def, "destroyed");
    if (ev) {
        show_event(run, ctx, ev);
    } else {
        // Ships without scripted outcomes give the standard reward.
        Gains g;
        memset(&g, 0, sizeof(g));
        sCtx = ctx;
        auto_reward(run, &g, "standard", 1);
        apply_scrap_arms(run, ctx->ship, &g);
        run->scrap += g.scrap;
        run->fuel += g.fuel;
        ctx->ship->missiles += g.missiles;
        ctx->ship->droneParts += g.parts;
        ctx->ship->hullHp += g.hull;
        if (ctx->ship->hullHp > ctx->ship->maxHull) ctx->ship->hullHp = ctx->ship->maxHull;
        run->view.active = true;
        char got[96] = "";
        if (g.fuel) snprintf(got + strlen(got), sizeof(got) - strlen(got), ", %+d fuel", g.fuel);
        if (g.missiles) snprintf(got + strlen(got), sizeof(got) - strlen(got), ", %+d missiles", g.missiles);
        if (g.parts) snprintf(got + strlen(got), sizeof(got) - strlen(got), ", %+d drone parts", g.parts);
        if (g.hull) snprintf(got + strlen(got), sizeof(got) - strlen(got), ", %+d hull", g.hull);
        snprintf(run->view.text, sizeof(run->view.text), "The enemy ship is no more.\n\n%+d scrap%s%s%s", g.scrap, got,
                 g.item[0] ? "\n\nReceived: " : "", g.item);
        run->view.choiceCount = 1;
        snprintf(run->view.choiceText[0], sizeof(run->view.choiceText[0]), "Continue...");
    }
}

bool run_arrival_ship(const Run* run, const XNode** ship, const XNode** def) {
    *ship = run->view.ship;
    *def = run->view.ship ? run->view.shipDef : sShipDef;
    return *ship || *def;
}

const char* run_ship_blueprint(const XNode* ship, const XNode* def) {
    const char* bp = def ? xattr(def, "auto_blueprint") : NULL;
    if (!bp && ship) bp = xattr(ship, "auto_blueprint");
    return bp;
}

// ---- Sectors and the map ------------------------------------------------------------------

static const XNode* sector_root(void) {
    return sSectorDoc.root ? (xchild(sSectorDoc.root, "FTL") ? xchild(sSectorDoc.root, "FTL") : sSectorDoc.root) : NULL;
}

static const XNode* sector_description(const char* name) {
    const XNode* r = sector_root();
    for (const XNode* n = r ? r->child : NULL; n; n = n->next)
        if (!strcmp(n->name, "sectorDescription") && xattr(n, "name") && !strcmp(xattr(n, "name"), name)) return n;
    return NULL;
}

// A sector of a class for column `col`: one of that class's sectorDescriptions allowed this far in
// (minSector), not a unique one already used.
static void roll_sector(SectorNode* n, int col, int cls, char used[][40], int* usedCount) {
    static const char* const classes[3] = {"CIVILIAN", "HOSTILE", "NEBULA"};
    n->cls = cls;
    const XNode* r = sector_root();
    const XNode* types = NULL;
    for (const XNode* t = r ? r->child : NULL; t; t = t->next)
        if (!strcmp(t->name, "sectorType") && xattr(t, "name") && !strcmp(xattr(t, "name"), classes[cls])) types = t;
    const char* options[16];
    int count = 0;
    for (const XNode* s = types ? types->child : NULL; s && count < 16; s = s->next) {
        const XNode* desc = sector_description(s->text);
        if (!desc || xattr_int(desc, "minSector", 0) > col) continue;
        const char* uniq = xattr(desc, "unique");
        bool taken = false;
        for (int k = 0; uniq && !strcmp(uniq, "true") && k < *usedCount; k++) taken |= !strcmp(used[k], s->text);
        if (!taken) options[count++] = s->text;
    }
    snprintf(n->type, sizeof(n->type), "%s", count ? options[rand() % count] : "STANDARD_SPACE");
    if (*usedCount < 32) snprintf(used[(*usedCount)++], 40, "%s", n->type);
}

static void sector_names(SectorNode* n) {
    const XNode* desc = sector_description(n->type);
    const XNode* nl = desc ? xchild(desc, "nameList") : NULL;
    const XNode* nm = nl ? xchild(nl, "name") : NULL;
    snprintf(n->name, sizeof(n->name), "%s", nm ? text_get(xattr(nm, "id"), n->type) : n->type);
}

// FTL's StarMap::GenerateSectorMap (xftl doc/sector-map): the first column is the civilian start,
// the last the final sector; between them 2-4 sectors per column, never the same count twice
// running (the first middle column always 2). Types: 20% nebula, else 60/40 civilian/hostile.
// Links never cross: each sector links to the matching stretch of the previous column.
static void generate_sector_map(Run* run) {
    char used[32][40];
    int usedCount = 0;
    memset(run->sectors, 0, sizeof(run->sectors));
    run->sectorCount[0] = 1;
    snprintf(run->sectors[0][0].type, sizeof(run->sectors[0][0].type), "CIVILIAN_SECTOR");
    run->sectors[0][0].cls = SECTOR_CIVILIAN;
    for (int col = 1; col < RUN_SECTORS; col++) {
        int n;
        if (col == RUN_SECTORS - 1)
            n = 1;
        else if (col == 1)
            n = 2;
        else
            do n = 2 + rand() % 3; while (n == run->sectorCount[col - 1]);
        run->sectorCount[col] = n;
        for (int row = 0; row < n; row++) {
            SectorNode* node = &run->sectors[col][row];
            if (col == RUN_SECTORS - 1) {
                snprintf(node->type, sizeof(node->type), "FINAL");
                node->cls = SECTOR_FINAL;
            } else {
                int cls = rand() % 5 == 0 ? SECTOR_NEBULA : rand() % 5 < 3 ? SECTOR_CIVILIAN : SECTOR_HOSTILE;
                roll_sector(node, col, cls, used, &usedCount);
            }
        }
        // Link: sector j of this column is reached from previous sectors [j*p/n, ((j+1)*p-1)/n].
        int p = run->sectorCount[col - 1];
        for (int row = 0; row < n; row++) {
            int lo = row * p / n, hi = ((row + 1) * p - 1) / n;
            for (int k = lo; k <= hi && k < p; k++) {
                SectorNode* from = &run->sectors[col - 1][k];
                if (from->nextCount < SECTOR_ROWS) from->next[from->nextCount++] = row;
            }
        }
    }
    for (int col = 0; col < RUN_SECTORS; col++)
        for (int row = 0; row < run->sectorCount[col]; row++) sector_names(&run->sectors[col][row]);
}

bool run_sector_reachable(const Run* run, int row) {
    if (run->sector + 1 >= RUN_SECTORS) return false;
    const SectorNode* cur = &run->sectors[run->sector][run->sectorRow];
    for (int k = 0; k < cur->nextCount; k++)
        if (cur->next[k] == row) return true;
    return false;
}

static bool connected(const Run* run, int from, int to) {
    int queue[MAX_BEACONS], head = 0, tail = 0;
    bool seen[MAX_BEACONS] = {false};
    queue[tail++] = from;
    seen[from] = true;
    while (head < tail) {
        int b = queue[head++];
        if (b == to) return true;
        for (int k = 0; k < run->beacons[b].nbCount; k++) {
            int n = run->beacons[b].nb[k];
            if (!seen[n]) {
                seen[n] = true;
                queue[tail++] = n;
            }
        }
    }
    return false;
}

static int jumps_between(const Run* run, int from, int to) {
    int dist[MAX_BEACONS], queue[MAX_BEACONS], head = 0, tail = 0;
    for (int i = 0; i < MAX_BEACONS; i++) dist[i] = -1;
    dist[from] = 0;
    queue[tail++] = from;
    while (head < tail) {
        int b = queue[head++];
        for (int k = 0; k < run->beacons[b].nbCount; k++) {
            int n = run->beacons[b].nb[k];
            if (dist[n] < 0) {
                dist[n] = dist[b] + 1;
                queue[tail++] = n;
            }
        }
    }
    return dist[to];
}

// Whether linking beacons a and b would cross an existing link or pass within 12px of another
// beacon (not part of FTL's rule; keeps the small map readable).
static bool link_is_cluttered(const Run* run, int a, int b) {
    const Beacon* A = &run->beacons[a];
    const Beacon* B = &run->beacons[b];
    float ux = B->x - A->x, uy = B->y - A->y, len2 = ux * ux + uy * uy;
    for (int i = 0; i < run->beaconCount; i++) {
        if (i == a || i == b) continue;
        const Beacon* C = &run->beacons[i];
        float t = len2 > 0 ? ((C->x - A->x) * ux + (C->y - A->y) * uy) / len2 : 0;
        if (t <= 0 || t >= 1) continue;
        float px = A->x + ux * t - C->x, py = A->y + uy * t - C->y;
        if (px * px + py * py < 12.0f * 12.0f) return true;
    }
    for (int i = 0; i < run->beaconCount; i++)
        for (int k = 0; k < run->beacons[i].nbCount; k++) {
            int j = run->beacons[i].nb[k];
            if (j < i || i == a || i == b || j == a || j == b) continue;
            const Beacon* C = &run->beacons[i];
            const Beacon* D = &run->beacons[j];
            float d1 = (D->x - C->x) * (A->y - C->y) - (D->y - C->y) * (A->x - C->x);
            float d2 = (D->x - C->x) * (B->y - C->y) - (D->y - C->y) * (B->x - C->x);
            float d3 = ux * (C->y - A->y) - uy * (C->x - A->x);
            float d4 = ux * (D->y - A->y) - uy * (D->x - A->x);
            if (((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))) return true;
        }
    return false;
}

static void generate_map(Run* run) {
    const XNode* desc = sector_description(run->sectorType);
    // The sector's event pool: each named event min..max times, shuffled.
    const XNode* pool[64];
    const char* poolNames[64];
    int poolCount = 0;
    for (const XNode* e = desc ? desc->child : NULL; e; e = e->next) {
        if (strcmp(e->name, "event")) continue;
        int mn = xattr_int(e, "min", 0), mx = xattr_int(e, "max", mn);
        int n = mn + (mx > mn ? rand() % (mx - mn + 1) : 0);
        const XNode* ev = find_named(sEvents, sEventCount, xattr(e, "name"));
        for (int k = 0; k < n && ev && poolCount < 64; k++) {
            pool[poolCount] = ev;
            poolNames[poolCount++] = xattr(e, "name");
        }
    }
    for (int i = poolCount - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        const XNode* t = pool[i];
        pool[i] = pool[j];
        pool[j] = t;
        const char* tn = poolNames[i];
        poolNames[i] = poolNames[j];
        poolNames[j] = tn;
    }

    // FTL's StarMap::GenerateMap / PopulateGrid (xftl doc/sector-map): a 6x4 grid of 110px cells,
    // filled column by column. The exit is a random cell of the two right-hand columns, the start a
    // random cell of the first; any other cell is empty 20% of the time (but no more than one in
    // five). Each beacon sits 10px inside its cell and links to the beacons already placed in the
    // neighbouring cells within 165px. On top of FTL's rule, a link that would cross another or run
    // right past a third beacon is left out: at the 3DS's scale those blur into one. Up to 16 tries
    // for an exit at least 5 jumps from the start.
    for (int attempt = 0; attempt < 16; attempt++) {
        memset(run->beacons, 0, sizeof(run->beacons));
        run->beaconCount = 0;
        run->storeSavedCount = 0;
        int cellOf[MAX_BEACONS][2];
        bool finalSector = !strcmp(run->sectorType, "FINAL");
        // The Last Stand puts the Federation base in column 2-3 instead of an exit (xftl doc/sector-map).
        // (on hard, column 3-4 and 5-6 jumps away)
        int baseCol = run->difficulty == DIFF_HARD ? 3 : 2;
        int exitCx = finalSector ? baseCol + rand() % 2 : 4 + rand() % 2, exitCy = rand() % 4, startCy = rand() % 4;
        int start = -1, exit = -1, skipped = 0, used = 0;
        for (int cx = 0; cx < 6; cx++)
            for (int cy = 0; cy < 4; cy++) {
                bool isExit = cx == exitCx && cy == exitCy, isStart = cx == 0 && cy == startCy;
                if (!isExit && !isStart && rand() % 5 == 0) {
                    int sofar = run->beaconCount + skipped;
                    if (!(skipped >= 1 && skipped * 5 >= sofar)) {
                        skipped++;
                        continue;
                    }
                }
                int bi = run->beaconCount;
                Beacon* b = &run->beacons[bi];
                b->x = cx * 110 + 10 + rand() % 91;
                b->y = cy * 110 + 10 + rand() % 91;
                if (b->y > 415) b->y = 415;
                if (cy == 0 && cx >= 4 && b->y < 30) b->y = 30;
                cellOf[bi][0] = cx;
                cellOf[bi][1] = cy;
                if (isExit && finalSector) {
                    exit = bi;
                    b->base = true;
                    b->event = find_named(sEvents, sEventCount, "FEDERATION_BASE");
                    snprintf(b->eventName, sizeof(b->eventName), "BASE");
                } else if (isExit) {
                    exit = bi;
                    b->exit = true;
                    b->event = find_named(sEvents, sEventCount, "FINISH_BEACON");
                    snprintf(b->eventName, sizeof(b->eventName), "EXIT");
                } else if (isStart) {
                    start = bi;
                    b->start = true;
                    const XNode* se = desc ? xchild(desc, "startEvent") : NULL;
                    const char* sn = run->sector == 0 ? "START_GAME" : se ? se->text : "START_BEACON";
                    if (finalSector) sn = "LAST_STAND_START";  // +10 fuel and repairs on arriving (wiki)
                    b->event = find_named(sEvents, sEventCount, sn);
                } else if (used < poolCount) {
                    b->event = pool[used];
                    snprintf(b->eventName, sizeof(b->eventName), "%s", poolNames[used++]);
                } else {
                    b->event = find_named(sEvents, sEventCount, "NEUTRAL");
                }
                run->beaconCount++;
                for (int o = 0; o < bi; o++) {
                    if (abs(cellOf[o][0] - cx) > 1 || abs(cellOf[o][1] - cy) > 1) continue;
                    float dx = run->beacons[o].x - b->x, dy = run->beacons[o].y - b->y;
                    if (dx * dx + dy * dy > 165.0f * 165.0f) continue;
                    if (link_is_cluttered(run, o, bi)) continue;
                    Beacon* A = &run->beacons[o];
                    if (A->nbCount < 8 && b->nbCount < 8) {
                        A->nb[A->nbCount++] = bi;
                        b->nb[b->nbCount++] = o;
                    }
                }
            }
        run->current = start;
        if (start >= 0 && exit >= 0 && connected(run, start, exit)) {
            int j = jumps_between(run, start, exit);
            // The base is 4 jumps from the start on normal (xftl doc/sector-map); exits at least 5.
            bool ok = !finalSector ? j >= 5 : run->difficulty == DIFF_HARD ? (j >= 5 && j <= 6) : (j >= 3 && j <= 5);
            if (ok) break;
        }
    }
    // Never leave the sector without a start or a way out: the leftmost beacon starts, the
    // rightmost is the exit (the layout above almost always provides both).
    if (run->current < 0 || run->current >= run->beaconCount) {
        int best = 0;
        for (int i = 1; i < run->beaconCount; i++)
            if (run->beacons[i].x < run->beacons[best].x) best = i;
        run->current = best;
        run->beacons[best].start = true;
    }
    bool hasExit = false;
    for (int i = 0; i < run->beaconCount; i++) hasExit |= run->beacons[i].exit || run->beacons[i].base;
    if (!hasExit && run->beaconCount > 1) {
        int best = run->current == 0 ? 1 : 0;
        for (int i = 0; i < run->beaconCount; i++)
            if (i != run->current && run->beacons[i].x > run->beacons[best].x) best = i;
        run->beacons[best].exit = true;
        run->beacons[best].event = find_named(sEvents, sEventCount, "FINISH_BEACON");
    }
    run->fleetX = -959.0f;
    if (run_has_aug(run, "FLEET_DISTRACTION")) run->fleetX -= 64.0f;  // Distraction Buoys: a jump behind
    run->fleetY = 50 + rand() % 251;
    // Each beacon's event is picked now, so Long-Ranged Scanners can tell what's there.
    for (int i = 0; i < run->beaconCount; i++) run->beacons[i].event = resolve_event(run->beacons[i].event);
    run->revealed = false;
    // The sector's music: one of its trackList, like FTL.
    const XNode* tl = desc ? xchild(desc, "trackList") : NULL;
    run->trackCount = 0;
    for (const XNode* t = tl ? tl->child : NULL; t && run->trackCount < 8; t = t->next)
        if (!strcmp(t->name, "track")) snprintf(run->tracks[run->trackCount++], sizeof(run->tracks[0]), "%s", t->text);
    if (!run->trackCount) snprintf(run->tracks[run->trackCount++], sizeof(run->tracks[0]), "%s", sound_random_track());
    run->trackIndex = rand() % run->trackCount;
    snprintf(run->track, sizeof(run->track), "%s", run->tracks[run->trackIndex]);
    const XNode* nl = desc ? xchild(desc, "nameList") : NULL;
    const XNode* nm = nl ? xchild(nl, "name") : NULL;
    snprintf(run->sectorName, sizeof(run->sectorName), "%s", nm ? text_get(xattr(nm, "id"), run->sectorType) : run->sectorType);
}

// ---- The Last Stand ----------------------------------------------------------------------

// The flagship's next step towards the base (shortest path), or -1 at the base.
static int flagship_step(const Run* run) {
    if (run->flagBeacon == run->baseBeacon) return -1;
    int best = -1, bestDist = 1 << 30;
    const Beacon* f = &run->beacons[run->flagBeacon];
    for (int k = 0; k < f->nbCount; k++) {
        int d = jumps_between(run, f->nb[k], run->baseBeacon);
        if (d >= 0 && d < bestDist) {
            bestDist = d;
            best = f->nb[k];
        }
    }
    return best;
}

static void flagship_update_next(Run* run) {
    if (run->flagRunning) return;  // it already knows where it's fleeing to
    run->flagNext = flagship_step(run);
}

// Sets up the Last Stand: the base, and the flagship on one of the two right-hand columns, 2-4
// jumps from the base (xftl GameMap). Also no rebel fleet circle: the flagship replaces it.
static void last_stand_setup(Run* run) {
    run->lastStand = true;
    run->baseBeacon = -1;
    for (int i = 0; i < run->beaconCount; i++)
        if (run->beacons[i].base) run->baseBeacon = i;
    if (run->baseBeacon < 0) {  // never without a base: the beacon furthest from the start
        int best = 0, bestD = -1;
        for (int i = 0; i < run->beaconCount; i++) {
            int d = jumps_between(run, run->current, i);
            if (d > bestD) {
                bestD = d;
                best = i;
            }
        }
        run->baseBeacon = best;
        run->beacons[best].base = true;
        run->beacons[best].exit = false;
        snprintf(run->beacons[best].eventName, sizeof(run->beacons[best].eventName), "BASE");
    }
    int pick = -1;
    for (int attempt = 0; attempt < 32 && pick < 0; attempt++) {
        int cand[MAX_BEACONS], n = 0;
        for (int i = 0; i < run->beaconCount; i++)
            if (run->beacons[i].x >= 440 && i != run->baseBeacon && i != run->current) cand[n++] = i;
        if (!n) break;
        int b = cand[rand() % n], d = jumps_between(run, b, run->baseBeacon);
        if (d >= 2 && (d <= 4 || attempt > 10)) pick = b;
    }
    if (pick < 0) {  // anywhere that can reach the base
        for (int i = run->beaconCount - 1; i >= 0 && pick < 0; i--)
            if (i != run->baseBeacon && i != run->current && jumps_between(run, i, run->baseBeacon) > 0) pick = i;
    }
    run->flagBeacon = pick >= 0 ? pick : run->baseBeacon;
    run->flagStage = 1;
    run->flagCrew = 11 - 4;
    for (int k = 0; k < 4; k++) run->flagArtCrew[k] = true;
    run->flagJumping = run->flagRunning = false;
    run->baseTurns = 0;
    flagship_update_next(run);
    run->fleetX = -100000.0f;  // no fleet circle
}

// A turn passes (we jumped or waited): the flagship jumps every second turn, and three turns
// in a row at the base destroy it (xftl FlagshipBoss.advanceFleet).
static void flagship_turn(Run* run) {
    if (!run->lastStand || run->flagStage > 3) return;
    if (!run->flagJumping) {
        flagship_update_next(run);
        run->flagJumping = run->flagNext >= 0;
    } else if (run->flagNext >= 0) {
        run->flagBeacon = run->flagNext;
        run->flagRunning = false;
        run->flagJumping = false;
        flagship_update_next(run);
    }
    // The rebel fleet takes single beacons here (wiki: The Last Stand): the warned ones fall, and
    // one more is marked for next turn (one per turn is our choice; no source gives the rate).
    // The flagship holds wherever it is. The base is never taken this way.
    for (int i = 0; i < run->beaconCount; i++)
        if (run->beacons[i].fleetSoon) {
            run->beacons[i].fleetSoon = false;
            run->beacons[i].fleetHeld = true;
        }
    if (run->flagBeacon != run->baseBeacon) run->beacons[run->flagBeacon].fleetHeld = true;
    int cand[MAX_BEACONS], n = 0;
    for (int i = 0; i < run->beaconCount; i++)
        if (!run->beacons[i].fleetHeld && !run->beacons[i].base && i != run->current) cand[n++] = i;
    if (n) run->beacons[cand[rand() % n]].fleetSoon = true;
    if (run->flagBeacon == run->baseBeacon) {
        if (++run->baseTurns >= 3) {
            run->baseLost = true;
            run->over = true;
            run->won = false;
        }
    } else {
        run->baseTurns = 0;
    }
}

void run_flagship_defeated(Run* run, RunCtx* ctx) {
    if (!run->lastStand || run->flagStage > 3) return;
    if (run->flagStage == 3) {
        run->flagStage = 4;
        run->over = run->won = true;
        return;
    }
    run->flagStage++;
    // It jumps on towards the base, or, if it was at the base, flees to a neighbour first.
    run->flagRunning = false;
    flagship_update_next(run);
    if (run->flagNext < 0) {
        const Beacon* f = &run->beacons[run->flagBeacon];
        if (f->nbCount) {
            run->flagNext = f->nb[rand() % f->nbCount];
            run->flagRunning = true;
        }
    }
    run->flagJumping = run->flagNext >= 0;
    const XNode* ev = find_named(sEvents, sEventCount, "BOSS_ESCAPED");
    if (ev) show_event(run, ctx, ev);
}

static void arrive(Run* run, RunCtx* ctx) {
    Beacon* b = &run->beacons[run->current];
    const XNode* ev = b->event;
    if (run_beacon_overtaken(run, run->current)) {
        ev = find_named(sEvents, sEventCount, b->exit ? "FLEET_EASY_BEACON_DLC" : "FLEET_EASY_DLC");
        if (!ev) ev = find_named(sEvents, sEventCount, "FLEET_HARD");
    } else if (b->visited && !b->exit) {
        ev = NULL;  // nothing new at a beacon we've already been to
    }
    // Hazards don't come back on a second visit; nebulas and storms do (wiki: Environmental Hazards).
    run->env = ENV_NONE;
    run->envTarget = 0;
    if (b->visited && (b->env == ENV_NEBULA || b->env == ENV_STORM)) {
        run->env = b->env;
        run->envTarget = b->envTarget;
    }
    b->visited = true;
    sShipDef = NULL;
    sEnemyStatusCount = 0;
    run_backdrop(run, "BACKGROUND", "PLANET");
    memset(&run->view, 0, sizeof(run->view));
    if (run->lastStand && run->flagStage <= 3 && run->current == run->flagBeacon) {
        // The flagship is here: its stage's text, then the fight (started by the game).
        char name[24];
        snprintf(name, sizeof(name), "BOSS_TEXT_%d", run->flagStage);
        ev = find_named(sEvents, sEventCount, name);
        run->bossFight = true;
    }
    if (ev) show_event(run, ctx, ev);
}

void run_start(Run* run, RunCtx* ctx) {
    int difficulty = run->difficulty;
    bool sandbox = run->sandbox;
    memset(run, 0, sizeof(*run));
    run->difficulty = difficulty;
    run->sandbox = sandbox;
    run->fuel = 16;  // wiki: all ships start with 16 fuel
    // Starting scrap 30 / 10 / 0 on easy / normal / hard (wiki: Score).
    run->scrap = difficulty == DIFF_EASY ? 30 : difficulty == DIFF_HARD ? 0 : 10;
    // The ship's own augments take up slots, as in FTL.
    const ShipBlueprint* bp = ctx->ship ? ctx->ship->bp : NULL;
    for (int i = 0; bp && i < bp->augCount && run->augCount < MAX_AUGS; i++)
        snprintf(run->augs[run->augCount++], sizeof(run->augs[0]), "%s", bp->augs[i]);
    generate_sector_map(run);
    snprintf(run->sectorType, sizeof(run->sectorType), "%s", run->sectors[0][0].type);
    generate_map(run);
    arrive(run, ctx);
}

void run_next_sector(Run* run, RunCtx* ctx, int row) {
    if (run->sector + 1 >= RUN_SECTORS) {
        run->over = run->won = true;
        return;
    }
    if (!run_sector_reachable(run, row)) row = run->sectors[run->sector][run->sectorRow].next[0];
    run->sector++;
    run->sectorRow = row;
    snprintf(run->sectorType, sizeof(run->sectorType), "%s", run->sectors[run->sector][row].type);
    generate_map(run);
    if (!strcmp(run->sectorType, "FINAL")) {
        last_stand_setup(run);
        run->delayedCount = 0;  // no quests in the last sector (wiki)
    }
    for (int i = 0; i < run->delayedCount; i++) place_quest(run, run->delayedQuests[i], true);
    run->delayedCount = 0;
    arrive(run, ctx);
}

// How far the rebel fleet advances per jump or wait: half as far while we're in a nebula, 80% in a
// nebula sector (wiki: Environmental Hazards, Nebula).
static float fleet_step(const Run* run) {
    if (run->env != ENV_NEBULA && run->env != ENV_STORM) return 64.0f;
    return run->sectors[run->sector][run->sectorRow].cls == SECTOR_NEBULA ? 64.0f * 0.8f : 64.0f * 0.5f;
}

void run_wait(Run* run, RunCtx* ctx) {
    run->fleetX += fleet_step(run);
    flagship_turn(run);
    if (run->lastStand && run->flagStage <= 3 && run->flagBeacon == run->current) {
        // It came to us.
        sShipDef = NULL;
        memset(&run->view, 0, sizeof(run->view));
        char name[24];
        snprintf(name, sizeof(name), "BOSS_TEXT_%d", run->flagStage);
        const XNode* bev = find_named(sEvents, sEventCount, name);
        run->bossFight = true;
        if (bev) show_event(run, ctx, bev);
        return;
    }
    sShipDef = NULL;
    memset(&run->view, 0, sizeof(run->view));
    const XNode* ev = NULL;
    if (run_beacon_overtaken(run, run->current)) {
        ev = find_named(sEvents, sEventCount, "NO_FUEL_FLEET_DLC");
        if (!ev) ev = find_named(sEvents, sEventCount, "NO_FUEL_FLEET");
    }
    if (!ev && run->lastStand && run->fuel > 0) return;  // the Last Stand: waiting is allowed
    if (!ev) ev = find_named(sEvents, sEventCount, "NO_FUEL");
    if (ev) show_event(run, ctx, ev);
}

bool run_beacon_overtaken(const Run* run, int beacon) {
    if (run->lastStand) return run->beacons[beacon].fleetHeld;
    float dx = run->beacons[beacon].x - run->fleetX, dy = run->beacons[beacon].y - run->fleetY;
    return dx * dx + dy * dy < 767.0f * 767.0f;
}

bool run_can_jump(const Run* run, int beacon) {
    if (run->fuel <= 0 || beacon == run->current) return false;
    const Beacon* c = &run->beacons[run->current];
    for (int k = 0; k < c->nbCount; k++)
        if (c->nb[k] == beacon) return true;
    // Adv. FTL Navigation: any beacon already visited.
    return run->beacons[beacon].visited && run_has_aug(run, "FTL_JUMPER");
}

void run_jump(Run* run, RunCtx* ctx, int beacon) {
    if (!run_can_jump(run, beacon)) return;
    run->fuel--;
    run->fleetX += fleet_step(run);  // from the beacon we're leaving
    flagship_turn(run);
    run->current = beacon;
    sound_play("jumpArrive");
    arrive(run, ctx);
}

int run_price(const Run* run, int item) {
    switch (item) {
        case STORE_FUEL: return 3;
        case STORE_MISSILES: return 6;  // wiki: Stores and resources
        case STORE_PARTS: return 8;
        default: return run->sector < 3 ? 2 : run->sector < 6 ? 3 : 4;
    }
}

bool run_buy(Run* run, RunCtx* ctx, int item) {
    int price = run_price(run, item);
    if (run->scrap < price) return false;
    Ship* s = ctx->ship;
    switch (item) {
        case STORE_FUEL:
            if (run->storeFuel <= 0) return false;
            run->storeFuel--;
            run->fuel++;
            break;
        case STORE_MISSILES:
            if (run->storeMissiles <= 0) return false;
            run->storeMissiles--;
            s->missiles++;
            break;
        case STORE_PARTS:
            if (run->storeParts <= 0) return false;
            run->storeParts--;
            s->droneParts++;
            break;
        default:
            if (s->hullHp >= s->maxHull) return false;
            s->hullHp++;
            break;
    }
    run->scrap -= price;
    store_commit(run);
    sound_play("buy");
    return true;
}
