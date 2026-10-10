#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"
#include "sound.h"
#include "xmldom.h"

#ifdef FTL_TEST
#define PROFILE_FILE "sdmc:/3ds/ftl3ds/profile_test.txt"  // tests keep their own (everything unlocked)
#else
#define PROFILE_FILE "sdmc:/3ds/ftl3ds/profile.txt"
#endif

#define MAX_ACH 64
static AchDef sAch[MAX_ACH];
static int sAchCount;
static unsigned char sAchLevel[MAX_ACH];         // 0, or 1 + difficulty
static unsigned char sUnlocked[SHIP_FAMILIES];   // bit per layout
static unsigned char sWins[SHIP_FAMILIES];       // bit per layout: beaten the flagship with it
static long sScrap, sKills;

#define MAX_POPUPS 8
static ProfilePopup sPopups[MAX_POPUPS];
static int sPopupCount;

static const char* const FAMILY_BP[SHIP_FAMILIES] = {
    "PLAYER_SHIP_HARD", "PLAYER_SHIP_STEALTH", "PLAYER_SHIP_MANTIS", "PLAYER_SHIP_CIRCLE", "PLAYER_SHIP_FED",
    "PLAYER_SHIP_JELLY", "PLAYER_SHIP_ROCK", "PLAYER_SHIP_ENERGY", "PLAYER_SHIP_CRYSTAL", "PLAYER_SHIP_ANAEROBIC"};

// Beating the flagship with ship i unlocks ship VICTORY_UNLOCKS[i] (wiki: each ship's Unlock).
static const signed char VICTORY_UNLOCKS[SHIP_FAMILIES] = {-1, -1, FAM_SLUG, FAM_FED, FAM_ZOLTAN, FAM_ROCK, FAM_STEALTH,
                                                           FAM_MANTIS, -1, -1};

int profile_family_of(const char* name, int* layout) {
    char base[48];
    snprintf(base, sizeof(base), "%.47s", name);
    size_t n = strlen(base);
    int l = 0;
    if (n > 2 && base[n - 2] == '_' && (base[n - 1] == '2' || base[n - 1] == '3')) {
        l = base[n - 1] - '1';
        base[n - 2] = '\0';
    }
    if (layout) *layout = l;
    for (int f = 0; f < SHIP_FAMILIES; f++)
        if (!strcmp(base, FAMILY_BP[f])) return f;
    return -1;
}

static void popup(int ach, int family, int layout) {
    if (sPopupCount >= MAX_POPUPS) return;
    sPopups[sPopupCount++] = (ProfilePopup){ach, family, layout};
}

bool profile_next_popup(ProfilePopup* out) {
    if (!sPopupCount) return false;
    *out = sPopups[0];
    memmove(sPopups, sPopups + 1, sizeof(sPopups[0]) * --sPopupCount);
    return true;
}

bool profile_layout_unlocked(int f, int layout) {
    return f >= 0 && f < SHIP_FAMILIES && layout >= 0 && layout < 3 && (sUnlocked[f] >> layout & 1);
}

bool profile_victory(int f, int layout) { return f >= 0 && f < SHIP_FAMILIES && (sWins[f] >> layout & 1); }

static void check_unlocks(void);

bool profile_unlock(int f, int layout) {
    if (f < 0 || f >= SHIP_FAMILIES || layout < 0 || layout > 2 || profile_layout_unlocked(f, layout)) return false;
    sUnlocked[f] |= 1 << layout;
    popup(-1, f, layout);
    sound_play("achievement");
    check_unlocks();
    profile_save();
    return true;
}

int profile_ach_count(void) { return sAchCount; }
const AchDef* profile_ach(int i) { return i >= 0 && i < sAchCount ? &sAch[i] : NULL; }
int profile_ach_level(int i) { return i >= 0 && i < sAchCount ? sAchLevel[i] : 0; }

int profile_ach_find(const char* id) {
    for (int i = 0; i < sAchCount; i++)
        if (!strcmp(sAch[i].id, id)) return i;
    return -1;
}

bool profile_award(const char* id, int difficulty) {
    int i = profile_ach_find(id);
    if (i < 0) return false;
    int level = sAch[i].multi ? 1 + difficulty : 1;
    if (sAchLevel[i] >= level) return false;
    bool fresh = sAchLevel[i] == 0;
    sAchLevel[i] = (unsigned char)level;
    if (fresh) {
        popup(i, -1, -1);
        sound_play("achievement");
    }
    check_unlocks();
    profile_save();
    return true;
}

// Layouts B (two of the ship's three achievements), the Lanius (four ships besides the Kestrel),
// the Crystal (A and B victories with every other ship but the Lanius), and Your Own Fleet.
static void check_unlocks(void) {
    for (int f = 0; f < SHIP_FAMILIES; f++) {
        int got = 0;
        for (int i = 0; i < sAchCount; i++) got += sAch[i].family == f && sAchLevel[i] > 0;
        if (got >= 2 && profile_layout_unlocked(f, 0) && !profile_layout_unlocked(f, 1)) {
            sUnlocked[f] |= 2;
            popup(-1, f, 1);
        }
    }
    int others = 0;
    for (int f = 0; f < SHIP_FAMILIES; f++) others += f != FAM_KESTREL && f != FAM_LANIUS && profile_layout_unlocked(f, 0);
    if (others >= 4 && !profile_layout_unlocked(FAM_LANIUS, 0)) {
        sUnlocked[FAM_LANIUS] |= 1;
        popup(-1, FAM_LANIUS, 0);
    }
    bool allWins = true;
    for (int f = 0; f < SHIP_FAMILIES && allWins; f++)
        if (f != FAM_LANIUS && f != FAM_CRYSTAL) allWins = profile_victory(f, 0) && profile_victory(f, 1);
    if (allWins && !profile_layout_unlocked(FAM_CRYSTAL, 0)) {
        sUnlocked[FAM_CRYSTAL] |= 1;
        popup(-1, FAM_CRYSTAL, 0);
    }
    bool fleet = true;
    for (int f = 0; f < SHIP_FAMILIES; f++) fleet &= profile_layout_unlocked(f, 0);
    int ua = profile_ach_find("ACH_UNLOCK_ALL");
    if (fleet && ua >= 0 && !sAchLevel[ua]) {
        sAchLevel[ua] = 1;
        popup(ua, -1, -1);
    }
}

void profile_record_victory(int f, int layout, int difficulty) {
    if (f < 0 || f >= SHIP_FAMILIES) return;
    sWins[f] |= 1 << layout;
    profile_award("ACH_WIN_EASY", difficulty);
    if (difficulty >= DIFF_NORMAL) profile_award("ACH_WIN_NORMAL", difficulty);
    if (VICTORY_UNLOCKS[f] >= 0) profile_unlock(VICTORY_UNLOCKS[f], 0);
    check_unlocks();
    profile_save();
}

void profile_add_scrap(int n) {
    if (n <= 0) return;
    long before = sScrap;
    sScrap += n;
    if (before < 10000 && sScrap >= 10000) profile_award("ACH_SCRAP", DIFF_NORMAL);
}

void profile_add_ship_kill(void) {
    sKills++;
    if (sKills >= 1000) profile_award("ACH_SHIPS", DIFF_NORMAL);
}

const char* profile_unlock_hint(int f, int layout) {
    static char buf[96];
    static const char* const names[SHIP_FAMILIES] = {"Kestrel", "Stealth", "Mantis", "Engi", "Federation",
                                                      "Slug", "Rock", "Zoltan", "Crystal", "Lanius"};
    if (layout == 1) return "Earn 2 of this ship's 3 achievements";
    if (layout == 2) return "Reach sector 8 with Type B";
    switch (f) {
        case FAM_ENGI: return "Reach sector 5 with the Kestrel";
        case FAM_LANIUS: return "Unlock 4 ships besides the Kestrel";
        case FAM_CRYSTAL: return "Win with Types A and B of every ship";
        default: break;
    }
    for (int k = 0; k < SHIP_FAMILIES; k++)
        if (VICTORY_UNLOCKS[k] == f) {
            snprintf(buf, sizeof(buf), "Find its quest, or win with the %s", names[k]);
            return buf;
        }
    return "";
}

// ---- Loading and saving -------------------------------------------------------------

static void load_defs(FtlDat* dat) {
    sAchCount = 0;
    char* buf = (char*)ftldat_read(dat, "data/achievements.xml", NULL);
    if (!buf) return;
    XDoc doc;
    if (!xdoc_parse(&doc, buf)) {  // xdoc owns buf from here
        xdoc_free(&doc);
        return;
    }
    const XNode* top = xchild(doc.root, "FTL") ? xchild(doc.root, "FTL") : doc.root;
    for (const XNode* n = top->child; n && sAchCount < MAX_ACH; n = n->next) {
        if (strcmp(n->name, "achievement") || !xattr(n, "id")) continue;
        AchDef* a = &sAch[sAchCount++];
        memset(a, 0, sizeof(*a));
        snprintf(a->id, sizeof(a->id), "%s", xattr(n, "id"));
        const XNode* img = xchild(n, "img");
        snprintf(a->img, sizeof(a->img), "img/%s", img ? img->text : "");
        const XNode* ship = xchild(n, "ship");
        a->family = ship ? profile_family_of(ship->text, NULL) : -1;
        a->multi = xchild(n, "multiDifficulty") != NULL;
    }
    xdoc_free(&doc);
}

void profile_init(FtlDat* dat) {
    load_defs(dat);
    memset(sAchLevel, 0, sizeof(sAchLevel));
    memset(sUnlocked, 0, sizeof(sUnlocked));
    memset(sWins, 0, sizeof(sWins));
    sScrap = sKills = 0;
    sUnlocked[FAM_KESTREL] = 1;  // the only ship to begin with
#ifdef FTL_TEST
    memset(sUnlocked, 7, sizeof(sUnlocked));  // scenarios can fly anything, from a clean slate
    return;
#endif
    FILE* f = fopen(PROFILE_FILE, "r");
    if (!f) return;
    char key[24], a[40];
    int x, y;
    while (fscanf(f, "%23s", key) == 1) {
        if (!strcmp(key, "ship") && fscanf(f, "%d %d", &x, &y) == 2 && x >= 0 && x < SHIP_FAMILIES)
            sUnlocked[x] |= (unsigned char)y;
        else if (!strcmp(key, "win") && fscanf(f, "%d %d", &x, &y) == 2 && x >= 0 && x < SHIP_FAMILIES)
            sWins[x] |= (unsigned char)y;
        else if (!strcmp(key, "ach") && fscanf(f, "%39s %d", a, &x) == 2) {
            int i = profile_ach_find(a);
            if (i >= 0) sAchLevel[i] = (unsigned char)x;
        } else if (!strcmp(key, "scrap") && fscanf(f, "%ld", &sScrap) == 1) {
        } else if (!strcmp(key, "kills") && fscanf(f, "%ld", &sKills) == 1) {
        } else {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
            }
        }
    }
    fclose(f);
    sUnlocked[FAM_KESTREL] |= 1;
    sPopupCount = 0;
}

void profile_save(void) {
    FILE* f = fopen(PROFILE_FILE, "w");
    if (!f) return;
    for (int i = 0; i < SHIP_FAMILIES; i++) {
        if (sUnlocked[i]) fprintf(f, "ship %d %d\n", i, sUnlocked[i]);
        if (sWins[i]) fprintf(f, "win %d %d\n", i, sWins[i]);
    }
    for (int i = 0; i < sAchCount; i++)
        if (sAchLevel[i]) fprintf(f, "ach %s %d\n", sAch[i].id, sAchLevel[i]);
    fprintf(f, "scrap %ld\nkills %ld\n", sScrap, sKills);
    fclose(f);
}
