#pragma once
// Sprite sheets and animations from data/animations.xml and data/dlcAnimations.xml.

#include <stdbool.h>

#include "ftldat.h"
#include "texture.h"

typedef struct {
    char* xml[2];  // animations.xml, dlcAnimations.xml
} AnimDb;

typedef struct {
    char file[64];  // path inside ftl.dat, e.g. img/people/human_base.png
    int w, h, fw, fh;
} SheetInfo;

typedef struct {
    char sheet[24];
    int x, y;    // first frame; y counts rows from the BOTTOM of the sheet, as in FTL's XML
    int length;  // frames
    float time;  // seconds for the whole animation
} AnimInfo;

typedef struct {
    char sheet[24];
    int x, y, length;          // frames, as in AnimInfo
    int chargedFrame, fireFrame;
    int mountX, mountY;        // point in the frame that sits on the hull's mount
} WeaponAnimInfo;

bool animdb_open(AnimDb* db, FtlDat* dat);
void animdb_close(AnimDb* db);
bool animdb_sheet(const AnimDb* db, const char* name, SheetInfo* out);
bool animdb_anim(const AnimDb* db, const char* name, AnimInfo* out);
bool animdb_weapon(const AnimDb* db, const char* name, WeaponAnimInfo* out);

// Draws one fw x fh frame of a sheet texture. col/rowFromTop are frame coordinates,
// (x, y) is the top-left corner on screen.
void sheet_draw_frame(const Texture* t, int fw, int fh, int col, int rowFromTop, float x, float y, float scale);

// Same, but centred on (cx, cy) and rotated by `angle` radians.
void sheet_draw_frame_rotated(const Texture* t, int fw, int fh, int col, int rowFromTop, float cx, float cy, float scale,
                              float angle);
