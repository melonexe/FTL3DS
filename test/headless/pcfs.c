// Headless PC build: maps 3DS paths to PC folders (see pcfs.h).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* pc_path(const char* path) {
    static char bufs[4][1024];
    static int next;
    const char* env = NULL;
    const char* rest = NULL;
    const char* fallback = NULL;
    if (!strncmp(path, "sdmc:/", 6)) {
        env = getenv("FTL3DS_SDMC");
        fallback = "sdmc";
        rest = path + 6;
    } else if (!strncmp(path, "romfs:/", 7)) {
        env = getenv("FTL3DS_ROMFS");
        fallback = "romfs";
        rest = path + 7;
    } else {
        return path;
    }
    char* out = bufs[next++ & 3];
    snprintf(out, sizeof(bufs[0]), "%s/%s", env && env[0] ? env : fallback, rest);
    return out;
}
