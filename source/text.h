#pragma once
// English text strings from data/text_*.xml, looked up by id (e.g. "weapon_LASER_BURST_3_short").

#include <stdbool.h>

#include "ftldat.h"

bool text_load(FtlDat* dat);
void text_free(void);

// The string for an id, or `fallback` if it isn't defined.
const char* text_get(const char* id, const char* fallback);
