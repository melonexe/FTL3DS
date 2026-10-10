#pragma once
// The player's profile across runs (sdmc:/3ds/ftl3ds/profile.txt): which ships and layouts are
// unlocked, achievements earned (and on which difficulty), flagship victories per layout, and
// running totals. Rules from the FTL wiki (Ships, Ship Achievements, Achievements):
//   - only the Kestrel A to begin with; reaching sector 5 with the Kestrel unlocks the Engi;
//   - layout A: the ship's quest (events' <unlockShip id>) or beating the flagship with the ship
//     before it (Engi -> Federation -> Zoltan -> Mantis -> Slug -> Rock -> Stealth);
//   - layout B: two of the ship's three achievements; layout C: sector 8 with layout B;
//   - the Lanius after four other ships; the Crystal after A and B victories with every other ship.
// Achievement names, descriptions and pictures come from data/achievements.xml.

#include <stdbool.h>

#include "ftldat.h"

enum { FAM_KESTREL, FAM_STEALTH, FAM_MANTIS, FAM_ENGI, FAM_FED, FAM_SLUG, FAM_ROCK, FAM_ZOLTAN, FAM_CRYSTAL, FAM_LANIUS,
       SHIP_FAMILIES };  // FTL's own ship ids (as in <unlockShip id>)

typedef struct {
    char id[32];      // ACH_SECTOR_5, ...
    char img[48];     // img/achievements/...
    int family;       // FAM_* for ship achievements, -1 for general ones
    bool multi;       // records the difficulty it was earned on
} AchDef;

void profile_init(FtlDat* dat);
void profile_save(void);

// The ship family of a player blueprint (and its layout 0-2 from the _2 / _3 suffix), or -1.
int profile_family_of(const char* bpName, int* layout);
bool profile_layout_unlocked(int family, int layout);
// How to unlock it, for the hangar.
const char* profile_unlock_hint(int family, int layout);
// Unlocks a layout; true if it wasn't already (queues a popup).
bool profile_unlock(int family, int layout);

int profile_ach_count(void);
const AchDef* profile_ach(int i);
int profile_ach_find(const char* id);
int profile_ach_level(int i);  // 0 not earned, else 1 + the best difficulty it was earned on
// Earns an achievement (difficulty: DIFF_*); true if new or on a harder difficulty. Queues a
// popup, and may unlock layouts.
bool profile_award(const char* id, int difficulty);

void profile_record_victory(int family, int layout, int difficulty);
bool profile_victory(int family, int layout);
void profile_add_scrap(int n);
void profile_add_ship_kill(void);

// Popups waiting to be shown: an achievement (index) or a ship unlock (family/layout), one at a
// time. False when there's nothing.
typedef struct {
    int ach;               // achievement index, or -1 for a ship unlock
    int family, layout;
} ProfilePopup;
bool profile_next_popup(ProfilePopup* out);
