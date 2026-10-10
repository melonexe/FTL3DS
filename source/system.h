#pragma once
// Ship systems: types, stations, and reactor power.

#include <stdbool.h>

enum {
    SYS_SHIELDS, SYS_ENGINES, SYS_OXYGEN, SYS_WEAPONS, SYS_DRONES, SYS_MEDBAY, SYS_CLONEBAY,
    SYS_TELEPORTER, SYS_CLOAKING, SYS_HACKING, SYS_MIND, SYS_ARTILLERY,
    // Subsystems: powered by themselves, not the reactor.
    SYS_PILOT, SYS_SENSORS, SYS_DOORS, SYS_BATTERY,
    SYS_COUNT
};

#define MAX_SYSTEMS 16

typedef struct {
    int type;       // SYS_*
    int level;      // capacity (the blueprint's "power" attribute)
    int roomId;     // room id from the blueprint
    char img[32];   // interior image name, e.g. room_pilot_2; empty if none
    int slot;       // station tile, row-major within the room; -1 if not mannable
    int slotDir;    // DIR_* the crew member faces at the station
    char weapon[32]; // artillery: the weapon blueprint it fires
    int max;         // enemies: the most power the generator may give it (0: the system's maximum)
} SystemDef;

const char* system_name(int type);  // XML / icon name, e.g. "shields"
int system_from_name(const char* name);
bool system_is_subsystem(int type);
bool system_is_mannable(int type);

// Station used when a blueprint gives no <slot>. Matches the default room_<system>.png art.
void system_default_slot(int type, int* slot, int* dir);
