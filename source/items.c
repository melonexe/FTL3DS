#include "items.h"

#include <stdlib.h>
#include <string.h>

#include "system.h"
#include "text.h"
#include "xmldom.h"

#define MAX_ITEMS 400

static XDoc sDocs[3];
static int sDocCount;
static ItemDef sItems[MAX_ITEMS];
static int sItemCount;
static SysBlueprint sSystems[SYS_COUNT];
static bool sHaveSystem[SYS_COUNT];

static const char* child_text(const XNode* n, const char* tag) {
    const XNode* c = xchild(n, tag);
    return c ? c->text : NULL;
}

static int child_int(const XNode* n, const char* tag, int fallback) {
    const char* t = child_text(n, tag);
    return t && *t ? atoi(t) : fallback;
}

// <title id="..."/> or <title>inline</title>.
static const char* text_of(const XNode* n, const char* tag, const char* fallback) {
    const XNode* c = xchild(n, tag);
    if (!c) return fallback;
    const char* id = xattr(c, "id");
    if (id) return text_get(id, fallback);
    return c->text[0] ? c->text : fallback;
}

static void add_item(int kind, const XNode* n) {
    const char* name = xattr(n, "name");
    if (!name) return;
    ItemDef* it = NULL;
    for (int i = 0; i < sItemCount && !it; i++)  // later files override earlier ones
        if (!strcmp(sItems[i].name, name)) it = &sItems[i];
    if (!it) {
        if (sItemCount >= MAX_ITEMS) return;
        it = &sItems[sItemCount++];
    }
    it->kind = kind;
    it->name = name;
    it->title = text_of(n, "title", name);
    it->desc = text_of(n, "desc", "");
    it->cost = child_int(n, "cost", 0);
    it->rarity = child_int(n, "rarity", 0);
    const char* v = child_text(n, "value");
    it->value = v ? (float)atof(v) : 0.0f;
    const char* st = child_text(n, "stackable");
    it->stackable = st && !strcmp(st, "true");
}

bool items_load(FtlDat* dat) {
    static const char* const files[3] = {"data/blueprints.xml", "data/dlcBlueprints.xml", "data/dlcBlueprintsOverwrite.xml"};
    for (int f = 0; f < 3; f++) {
        char* buf = (char*)ftldat_read(dat, files[f], NULL);
        if (!buf || !xdoc_parse(&sDocs[sDocCount], buf)) continue;
        const XNode* root = sDocs[sDocCount++].root;
        const XNode* top = xchild(root, "FTL") ? xchild(root, "FTL") : root;
        for (const XNode* n = top->child; n; n = n->next) {
            if (!strcmp(n->name, "weaponBlueprint")) add_item(ITEM_WEAPON, n);
            else if (!strcmp(n->name, "droneBlueprint")) add_item(ITEM_DRONE, n);
            else if (!strcmp(n->name, "augBlueprint")) add_item(ITEM_AUGMENT, n);
            else if (!strcmp(n->name, "crewBlueprint")) add_item(ITEM_CREW, n);
            else if (!strcmp(n->name, "systemBlueprint")) {
                int type = system_from_name(child_text(n, "type") ? child_text(n, "type") : xattr(n, "name"));
                if (type < 0) continue;
                SysBlueprint* s = &sSystems[type];
                memset(s, 0, sizeof(*s));
                s->startPower = child_int(n, "startPower", 1);
                s->maxPower = child_int(n, "maxPower", 8);
                s->cost = child_int(n, "cost", 0);
                s->title = text_of(n, "title", system_name(type));
                const XNode* up = xchild(n, "upgradeCost");
                for (const XNode* l = up ? up->child : NULL; l && s->upgradeCount < 8; l = l->next)
                    if (!strcmp(l->name, "level")) s->upgrade[s->upgradeCount++] = atoi(l->text);
                sHaveSystem[type] = true;
            }
        }
    }
    return sItemCount > 0;
}

void items_free(void) {
    for (int i = 0; i < sDocCount; i++) xdoc_free(&sDocs[i]);
    sDocCount = sItemCount = 0;
    memset(sHaveSystem, 0, sizeof(sHaveSystem));
}

const ItemDef* item_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < sItemCount; i++)
        if (!strcmp(sItems[i].name, name)) return &sItems[i];
    return NULL;
}

const SysBlueprint* item_system(int type) { return type >= 0 && type < SYS_COUNT && sHaveSystem[type] ? &sSystems[type] : NULL; }

int items_of_kind(int kind, const ItemDef** out, int max) {
    int n = 0;
    for (int i = 0; i < sItemCount && n < max; i++)
        if (sItems[i].kind == kind && sItems[i].rarity > 0) out[n++] = &sItems[i];
    return n;
}

int items_all_of_kind(int kind, const ItemDef** out, int max) {
    int n = 0;
    for (int i = 0; i < sItemCount && n < max; i++)
        if (sItems[i].kind == kind) out[n++] = &sItems[i];
    return n;
}

int item_reactor_cost(int bars) { return bars < 5 ? 30 : (bars / 5) * 5 + 15; }
