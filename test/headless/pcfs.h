#pragma once
// Headless PC build: forced into every file (-include). The 3DS's "sdmc:/" and "romfs:/" paths go to
// the folders in $FTL3DS_SDMC and $FTL3DS_ROMFS (default ./sdmc and ./romfs), so each test run can
// have its own SD card and share one read-only ftl.dat.

#include <stdio.h>
#include <sys/stat.h>

const char* pc_path(const char* path);

#define fopen(p, m) fopen(pc_path(p), m)
#define remove(p) remove(pc_path(p))
#define stat(p, s) stat(pc_path(p), s)
