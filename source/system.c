#include "system.h"

#include <string.h>

#include "crew.h"

static const char* const NAMES[SYS_COUNT] = {
    "shields",  "engines", "oxygen",  "weapons", "drones",  "medbay", "clonebay", "teleporter",
    "cloaking", "hacking", "mind",    "artillery", "pilot", "sensors", "doors",   "battery",
};

const char* system_name(int type) { return type >= 0 && type < SYS_COUNT ? NAMES[type] : "?"; }

int system_from_name(const char* name) {
    for (int i = 0; i < SYS_COUNT; i++)
        if (!strcmp(NAMES[i], name)) return i;
    return -1;
}

bool system_is_subsystem(int type) { return type >= SYS_PILOT; }

bool system_is_mannable(int type) {
    return type == SYS_SHIELDS || type == SYS_ENGINES || type == SYS_WEAPONS || type == SYS_PILOT ||
           type == SYS_SENSORS || type == SYS_DOORS;
}

void system_default_slot(int type, int* slot, int* dir) {
    // Blueprints that use the default room art either give these explicitly
    // (pilot, weapons, sensors, doors) or rely on them (shields, engines, which we read off the art).
    switch (type) {
        case SYS_PILOT: *slot = 0; *dir = DIR_RIGHT; return;
        case SYS_SHIELDS: *slot = 0; *dir = DIR_LEFT; return;
        case SYS_WEAPONS: *slot = 1; *dir = DIR_UP; return;
        case SYS_ENGINES: *slot = 1; *dir = DIR_RIGHT; return;
        case SYS_SENSORS: *slot = 1; *dir = DIR_DOWN; return;
        case SYS_DOORS: *slot = 1; *dir = DIR_UP; return;
        default: *slot = -1; *dir = DIR_DOWN; return;
    }
}
