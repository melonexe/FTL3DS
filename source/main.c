// Stage 5: combat. Loads ships from the player's own ftl.dat with their crew, systems and
// weapons, and pits them against a random enemy ship. The touch screen has a crew tab (send
// crew between rooms, doors), an arms tab (weapons and targeting) and a power tab.

#include <3ds.h>
#include <citro2d.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "combat.h"
#include "crew.h"
#include "life.h"
#include "ftldat.h"
#include "ship.h"
#include "sound.h"
#include "feats.h"
#include "font.h"
#include "items.h"
#include "profile.h"
#include "run.h"
#include "save.h"
#include "testhooks.h"
#include "text.h"

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240
#define TAB_H 18  // tab strip at the top of the touch screen
#define BAR_H 24  // crew bar at the bottom of the touch screen
#define STEREO_FAR 6.0f    // 3D: pixels of eye separation (at full slider) for the starfield, behind
#define STEREO_NEAR -2.0f  // ... and for the readouts, in front of the screen
#define MAP_H (BOT_H - BAR_H)
#define DOORBTN_X 2  // "open all" / "close all" door buttons on the crew tab
#define DOORBTN_Y (TAB_H + 2)
#define DOORBTN_W 24  // buttons with FTL's door icons, floating over the map's top-left corner
#define DOORBTN_H 24
#define MAP_TOP (TAB_H + 1)  // ship map viewport on the crew tab
#define STATIONBTN_X (DOORBTN_X + 2 * (DOORBTN_W + 2) + 6)  // save / return to stations
#define ALLBTN_W 52          // "select all crew" button, top right
#define ALLBTN_X (BOT_W - ALLBTN_W - 2)
#define LOCKBTN_W 76  // a Crystal's lockdown, under "select all"
#define LOCKBTN_H 18
#define LOCKBTN_X (BOT_W - LOCKBTN_W - 2)
#define LOCKBTN_Y (DOORBTN_Y + 18)

enum { TAB_CREW, TAB_ARMS, TAB_SYS, TAB_POWER, TAB_COUNT };
#define TAB_W 46

// Selected crew: bright yellow, clear against FTL's green system icons, health bars and floors.
#define SELECT_COL C2D_Color32(255, 225, 40, 255)
#define SELECT_BG C2D_Color32(92, 80, 18, 255)

// Top screen: the player's ship on the left, the enemy in a panel on the right.
#define ENEMY_X 256
#define ENEMY_W (TOP_W - ENEMY_X)

// romfs: a personal test CIA can carry the player's own ftl.dat; otherwise it's read from the SD card.
static const char* const DAT_PATHS[] = {"romfs:/ftl.dat", "sdmc:/3ds/ftl3ds/ftl.dat", "sdmc:/ftl.dat"};

typedef struct {
    float x, y;  // ship-space point at the centre of the view
    float zoom;
    float fitZoom;
    bool rotated;  // turned a quarter anticlockwise: the ship's top points left
    float vx, vy, vw, vh;  // viewport on screen
} Camera;

// Fits the whole ship (hull included), or just its rooms when roomsOnly is set.
static void camera_fit(Camera* cam, const Ship* ship, float margin, bool roomsOnly) {
    float x0, y0, x1, y1;
    ship_bounds(ship, &x0, &y0, &x1, &y1);
    if (roomsOnly) {
        x0 = y0 = 1e9f;
        x1 = y1 = -1e9f;
        for (int i = 0; i < ship->roomCount; i++) {
            const Room* r = &ship->rooms[i];
            float rx = (float)((ship->xOffset + r->x) * TILE), ry = (float)((ship->yOffset + r->y) * TILE);
            if (rx < x0) x0 = rx;
            if (ry < y0) y0 = ry;
            if (rx + r->w * TILE > x1) x1 = rx + r->w * TILE;
            if (ry + r->h * TILE > y1) y1 = ry + r->h * TILE;
        }
    }
    cam->x = (x0 + x1) * 0.5f;
    cam->y = (y0 + y1) * 0.5f;
    float spanX = cam->rotated ? y1 - y0 : x1 - x0, spanY = cam->rotated ? x1 - x0 : y1 - y0;
    float zx = (cam->vw - margin) / spanX, zy = (cam->vh - margin) / spanY;
    cam->fitZoom = cam->zoom = zx < zy ? zx : zy;
}

// Ship space to screen and back.
static inline void to_screen(const Camera* c, float x, float y, float* X, float* Y) {
    float dx = (x - c->x) * c->zoom, dy = (y - c->y) * c->zoom;
    float cx = c->vx + c->vw * 0.5f, cy = c->vy + c->vh * 0.5f;
    *X = c->rotated ? cx + dy : cx + dx;
    *Y = c->rotated ? cy - dx : cy + dy;
}

static inline void to_ship(const Camera* c, float X, float Y, float* x, float* y) {
    float u = (X - c->vx - c->vw * 0.5f) / c->zoom, v = (Y - c->vy - c->vh * 0.5f) / c->zoom;
    *x = c->x + (c->rotated ? -v : u);
    *y = c->y + (c->rotated ? u : v);
}

// How much sprites drawn in ship space must turn.
static inline float cam_angle(const Camera* c) { return c->rotated ? -(float)M_PI_2 : 0.0f; }

// Seconds since the game started, for animations. (osGetTime() counts milliseconds since 1900,
// far too big to keep sub-second precision in a float.)
static float sClock;

// An animation from animations.xml with its sheet, for effects drawn on ships (fire, breaches).
typedef struct {
    Texture tex;
    int fw, fh, col, row, frames;
    float time;
} SheetAnim;

static SheetAnim sFireAnim, sBreachAnim;

static bool load_sheet_anim(SheetAnim* a, FtlDat* dat, const AnimDb* db, const char* name) {
    AnimInfo info;
    SheetInfo sh;
    if (!animdb_anim(db, name, &info) || !animdb_sheet(db, info.sheet, &sh)) return false;
    uint32_t size;
    uint8_t* png = ftldat_read(dat, sh.file, &size);
    bool ok = png && texture_load_png(&a->tex, png, size);
    free(png);
    if (!ok) return false;
    a->fw = sh.fw;
    a->fh = sh.fh;
    a->col = info.x;
    a->row = sh.h / sh.fh - 1 - info.y;
    int inSheet = sh.w / sh.fw - info.x;
    a->frames = info.length < inSheet ? info.length : inSheet;
    a->time = info.time > 0 ? info.time : 1.0f;
    return true;
}

// Frame of a looping animation at time t (seconds), offset per tile so neighbours don't sync up.
static void draw_sheet_anim(const SheetAnim* a, float x, float y, float scale, float t) {
    if (!a->tex.loaded) return;
    if (a->frames <= 0) return;
    int frame = (int)(t / (a->time > 0.0f ? a->time : 1.0f) * a->frames) % a->frames;
    sheet_draw_frame_rotated(&a->tex, a->fw, a->fh, a->col + frame, a->row, x, y, scale, 0.0f);
}

// Restricts drawing on the top screen to the columns [x0, x1). The framebuffer is stored rotated
// (240 wide, 400 tall, screen x running down it), hence the swapped axes.
static void clip_top(int x0, int x1) {
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, TOP_W - x1, TOP_H, TOP_W - x0);
}

static void clip_off(void) {
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// Image whose top-left corner is at ship-space (x, y).
static void draw_image(const Camera* cam, const Texture* t, float x, float y) {
    if (!t->loaded) return;
    float X, Y;
    if (!cam->rotated) {
        to_screen(cam, x, y, &X, &Y);
        C2D_DrawImageAt(t->image, X, Y, 0.0f, NULL, cam->zoom, cam->zoom);
    } else {
        to_screen(cam, x + t->width * 0.5f, y + t->height * 0.5f, &X, &Y);
        C2D_DrawImageAtRotated(t->image, X, Y, 0.0f, cam_angle(cam), NULL, cam->zoom, cam->zoom);
    }
}

// Rectangle given in ship space.
static void draw_rect(const Camera* cam, float x, float y, float w, float h, u32 color) {
    float x0, y0, x1, y1;
    to_screen(cam, x, y, &x0, &y0);
    to_screen(cam, x + w, y + h, &x1, &y1);
    C2D_DrawRectSolid(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, 0.0f, fabsf(x1 - x0), fabsf(y1 - y0), color);
}

static void draw_outline(const Camera* cam, float x, float y, float w, float h, float t, u32 color) {
    draw_rect(cam, x, y, w, t, color);
    draw_rect(cam, x, y + h - t, w, t, color);
    draw_rect(cam, x, y, t, h, color);
    draw_rect(cam, x + w - t, y, t, h, color);
}

// ---- Sensors ----------------------------------------------------------------------
// FTL wiki (Sensors): level 1 shows your whole ship (otherwise only rooms your crew are in), level 2
// the enemy's interior, level 3 the enemy's weapon charge; manning adds one level.

static struct {
    const Ship* ship[2];
    bool hidden[2][MAX_ROOMS];
} sVis;

static bool sDebugSensors;  // debug: see everything

static int sEnvNow;   // the beacon's environment (ENV_*), for the sensors and the HUD
static int sTestEnv;  // test.txt "env": the environment of one-off fights

// Hazards that put the ship "in danger": the drive must charge and the ship menu stays shut.
static bool env_dangerous(int env) { return env == ENV_ASTEROID || env == ENV_SUN || env == ENV_PULSAR || env == ENV_PDS; }

static int sBossStage;  // fighting the rebel flagship's stage 1-3, or 0
static bool sLifeSense;  // Lifeform Scanner or a Slug aboard: crew show in rooms the sensors miss
static char sExtraAugs[3][32];  // test scenarios: augments for one-off fights (outside a run)
static int sExtraAugCount;

static int sensor_level_raw(const Ship* s);

static int sensor_level(const Ship* s) {
    int lvl = sensor_level_raw(s);
    return sBossStage && lvl > 2 && !sDebugSensors ? 2 : lvl;  // the flagship jams sensors past 2 (wiki)
}

static int sensor_level_raw(const Ship* s) {
    if (sDebugSensors) return 4;
    if (sEnvNow == ENV_NEBULA || sEnvNow == ENV_STORM) return 0;  // a nebula blinds sensors (wiki)
    int si = ship_find_system(s, SYS_SENSORS);
    if (si < 0) return 0;
    int cap = ship_system_capacity(s, si);
    if (cap <= 0) return 0;
    return cap + (s->systems[si].manned && s->systems[si].ionTimer <= 0.0f ? 1 : 0);
}

static void update_visibility(const Ship* player, const CrewSet* crew, const Ship* enemy, const CrewSet* enemyCrew) {
    int level = sensor_level(player);
    sVis.ship[0] = player;
    sVis.ship[1] = enemy;
    for (int r = 0; r < player->roomCount; r++) sVis.hidden[0][r] = level < 1;
    for (int i = 0; i < crew->count; i++) {
        int r = ship_room_at(player, crew->crew[i].destX, crew->crew[i].destY);
        int r2 = ship_room_at(player, (int)floorf(crew->crew[i].x / TILE), (int)floorf(crew->crew[i].y / TILE));
        if (r >= 0) sVis.hidden[0][r] = false;
        if (r2 >= 0) sVis.hidden[0][r2] = false;
    }
    if (enemy) {
        for (int r = 0; r < enemy->roomCount; r++) sVis.hidden[1][r] = level < 2;
        for (int i = 0; enemyCrew && i < enemyCrew->count; i++) {
            if (!enemyCrew->crew[i].foreign) continue;
            int r = ship_room_at(enemy, (int)floorf(enemyCrew->crew[i].x / TILE), (int)floorf(enemyCrew->crew[i].y / TILE));
            if (r >= 0) sVis.hidden[1][r] = false;
        }
    }
}

static bool room_hidden(const Ship* s, int room) {
    for (int k = 0; k < 2; k++)
        if (sVis.ship[k] == s && room >= 0 && room < s->roomCount) return sVis.hidden[k][room];
    return false;
}

// Gibs fly apart for 2 s (xftl ShipGib.kt: distance = speed x time x 25, spin = turns x progress),
// then drift on and fade out.
#define GIB_TIME 2.0f
#define GIB_FADE 1.5f
#define GIB_SPIN 0.3f    // our tuning: a third of the game's spin reads better on the small screen
#define GIB_DRIFT 40.0f  // pixels per second per unit of gib speed (the game uses 25)

static bool draw_gibs(const Ship* s, const Camera* cam) {
    if (s->deathTime < 0.0f) return false;
    const float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    float t = s->deathTime, k = t / GIB_TIME;
    float alpha = t < GIB_TIME ? 1.0f : 1.0f - (t - GIB_TIME) / GIB_FADE;
    if (alpha <= 0.0f) return true;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    for (int i = s->gibCount - 1; i >= 0; i--) {
        const Gib* g = &s->gibs[i];
        if (!g->tex.loaded) continue;
        float dist = g->v * t * GIB_DRIFT, ang = -g->dir * (float)M_PI / 180.0f;
        float cx = ox + s->hullX + g->x + g->tex.width * 0.5f + cosf(ang) * dist;
        float cy = oy + s->hullY + g->y + g->tex.height * 0.5f + sinf(ang) * dist;
        float X, Y;
        to_screen(cam, cx, cy, &X, &Y);
        C2D_DrawImageAtRotated(g->tex.image, X, Y, 0, k * g->spin * GIB_SPIN * 2.0f * (float)M_PI + cam_angle(cam), &tint, cam->zoom,
                               cam->zoom);
    }
    return true;
}

// ---- Crisp small images ----------------------------------------------------------------
// FTL's art is drawn for 1280x720; shrunk by the GPU at odd ratios it blurs. These helpers cut
// out what's needed and draw it pixel for pixel.

static FtlDat* sDat;           // for UI art loaded on demand
static const AnimDb* sAnimDb;  // for item art

// Copies the (x, y, w, h) part of a PNG from ftl.dat into a texture (nearest filtering).
// With `icon`, white symbol pixels are kept, halved (any-of-2x2) and given a fresh 1px black
// outline, so a 32px FTL icon becomes a sharp 16px one.
static bool load_crop(Texture* out, const char* path, int x, int y, int w, int h, bool icon) {
    uint32_t size;
    uint8_t* png = ftldat_read(sDat, path, &size);
    uint8_t* rgba = NULL;
    int pw, ph;
    bool ok = false;
    if (png && texture_decode_png(png, size, 0, 0, &rgba, &pw, &ph)) {
        if (w <= 0) w = pw - x;
        if (h <= 0) h = ph - y;
        int ow = icon ? w / 2 + 2 : w, oh = icon ? h / 2 + 2 : h;
        uint8_t* buf = calloc((size_t)ow * oh, 4);
        if (buf) {
            if (!icon) {
                for (int j = 0; j < h; j++)
                    for (int i = 0; i < w; i++)
                        if (x + i < pw && y + j < ph && x + i >= 0 && y + j >= 0)
                            memcpy(buf + ((size_t)j * w + i) * 4, rgba + ((size_t)(y + j) * pw + x + i) * 4, 4);
            } else {
                // Symbol mask at half size, one pixel of margin for the outline.
                uint8_t* mask = calloc((size_t)ow * oh, 1);
                for (int j = 0; mask && j < h / 2; j++)
                    for (int i = 0; i < w / 2; i++)
                        for (int k = 0; k < 4; k++) {
                            int sx = x + i * 2 + (k & 1), sy = y + j * 2 + (k >> 1);
                            if (sx >= pw || sy >= ph) continue;
                            const uint8_t* p = rgba + ((size_t)sy * pw + sx) * 4;
                            if (p[3] > 128 && p[0] + p[1] + p[2] > 3 * 170) mask[(j + 1) * ow + i + 1] = 1;
                        }
                for (int j = 0; mask && j < oh; j++)
                    for (int i = 0; i < ow; i++) {
                        uint8_t* o = buf + ((size_t)j * ow + i) * 4;
                        if (mask[j * ow + i]) {
                            o[0] = o[1] = o[2] = o[3] = 255;
                            continue;
                        }
                        bool edge = false;
                        for (int dy = -1; dy <= 1 && !edge; dy++)
                            for (int dx = -1; dx <= 1 && !edge; dx++) {
                                int nx = i + dx, ny = j + dy;
                                edge = nx >= 0 && ny >= 0 && nx < ow && ny < oh && mask[ny * ow + nx];
                            }
                        if (edge) o[3] = 255;  // black outline
                    }
                free(mask);
            }
            ok = texture_from_rgba(out, buf, ow, oh);
            if (ok) C3D_TexSetFilter(out->image.tex, GPU_NEAREST, GPU_NEAREST);
            free(buf);
        }
    }
    free(rgba);
    free(png);
    return ok;
}

// 16px room icons, made once per system type.
static Texture sSmallIcon[SYS_COUNT];
static const Texture* small_icon(int type) {
    static bool tried[SYS_COUNT];
    if (type < 0 || type >= SYS_COUNT) return NULL;
    if (!tried[type]) {
        tried[type] = true;
        char path[64];
        snprintf(path, sizeof(path), "img/icons/s_%s_overlay.png", system_name(type));
        load_crop(&sSmallIcon[type], path, 0, 0, 0, 0, true);
    }
    return sSmallIcon[type].loaded ? &sSmallIcon[type] : NULL;
}

// The icon to draw in a room at this zoom: FTL's own at 1:1 when the room has space for it,
// otherwise the sharp small one. Always drawn unscaled.
static const Texture* room_icon(const ShipSystem* y, const Room* r, const Camera* cam) {
    float room = (r->w < r->h ? r->w : r->h) * TILE * cam->zoom;
    if (y->icon.loaded && room >= y->icon.width + 2) return &y->icon;
    const Texture* t = small_icon(y->def->type);
    return t ? t : (y->icon.loaded ? &y->icon : NULL);
}

static void draw_ship(const Ship* s, const Camera* cam) {
    if (draw_gibs(s, cam)) return;
    const float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    const u32 roomFill = C2D_Color32(230, 226, 219, 255);
    const u32 gridLine = C2D_Color32(180, 176, 170, 255);
    const u32 wall = C2D_Color32(40, 40, 40, 255);
    const u32 doorCol = C2D_Color32(230, 150, 40, 255);
    const float wallT = 1.5f / cam->zoom > 2.0f ? 1.5f / cam->zoom : 2.0f;  // ship pixels, at least 1.5 on screen

    if (s->cloakTime > 0.0f) {
        // Cloaked: the hull fades and the cloak glow outlines it.
        C2D_ImageTint fade;
        C2D_AlphaImageTint(&fade, 0.35f);
        float X, Y;
        to_screen(cam, ox + s->hullX, oy + s->hullY, &X, &Y);
        if (s->hull.loaded && !cam->rotated) C2D_DrawImageAt(s->hull.image, X, Y, 0, &fade, cam->zoom, cam->zoom);
        else draw_image(cam, &s->hull, ox + s->hullX, oy + s->hullY);
        draw_image(cam, &s->cloakImage, ox + s->hullX + s->cloakX, oy + s->hullY + s->cloakY);
    } else {
        draw_image(cam, &s->hull, ox + s->hullX, oy + s->hullY);
    }
    draw_image(cam, &s->floor, ox + s->hullX + s->floorX, oy + s->hullY + s->floorY);

    for (int i = 0; i < s->roomCount; i++) {
        const Room* r = &s->rooms[i];
        float x = ox + r->x * TILE, y = oy + r->y * TILE, w = r->w * TILE, h = r->h * TILE;
        if (room_hidden(s, i)) {
            draw_rect(cam, x, y, w, h, C2D_Color32(52, 58, 70, 255));
            draw_rect(cam, x - wallT / 2, y - wallT / 2, w + wallT, wallT, wall);
            draw_rect(cam, x - wallT / 2, y + h - wallT / 2, w + wallT, wallT, wall);
            draw_rect(cam, x - wallT / 2, y - wallT / 2, wallT, h + wallT, wall);
            draw_rect(cam, x + w - wallT / 2, y - wallT / 2, wallT, h + wallT, wall);
            continue;
        }
        draw_rect(cam, x, y, w, h, roomFill);
        for (int gx = 1; gx < r->w; gx++) draw_rect(cam, x + gx * TILE - 0.5f, y, 1.0f, h, gridLine);
        for (int gy = 1; gy < r->h; gy++) draw_rect(cam, x, y + gy * TILE - 0.5f, w, 1.0f, gridLine);
        int si = ship_system_in_room(s, i);
        if (si >= 0) draw_image(cam, &s->systems[si].interior, x, y);
        if (s->oxygen[i] < LIFE_SUFFOCATE_BELOW) {
            float pulse = 0.5f + 0.5f * sinf(sClock * 6.0f);
            draw_rect(cam, x, y, w, h, C2D_Color32(230, 20, 20, (u8)(150 + 60 * pulse)));
        } else if (s->oxygen[i] < 99.0f) {
            draw_rect(cam, x, y, w, h, C2D_Color32(255, 90, 90, (u8)((100.0f - s->oxygen[i]) * 0.9f)));
        }
        // Breaches and fires on this room's tiles; kept at least half size so they stay visible.
        float t = sClock, fxScale = cam->zoom < 0.5f ? 0.5f : cam->zoom;
        for (int ty = r->y; ty < r->y + r->h; ty++)
            for (int tx = r->x; tx < r->x + r->w; tx++) {
                if (s->breach[ty][tx] <= 0.0f && s->fire[ty][tx] <= 0.0f) continue;
                float X, Y;
                to_screen(cam, ox + (tx + 0.5f) * TILE, oy + (ty + 0.5f) * TILE, &X, &Y);
                if (s->breach[ty][tx] > 0.0f) draw_sheet_anim(&sBreachAnim, X, Y, fxScale, sBreachAnim.time * 0.99f);
                if (s->fire[ty][tx] > 0.0f) draw_sheet_anim(&sFireAnim, X, Y, fxScale, t + tx * 0.37f + ty * 0.61f);
            }
        draw_rect(cam, x - wallT / 2, y - wallT / 2, w + wallT, wallT, wall);
        draw_rect(cam, x - wallT / 2, y + h - wallT / 2, w + wallT, wallT, wall);
        draw_rect(cam, x - wallT / 2, y - wallT / 2, wallT, h + wallT, wall);
        draw_rect(cam, x + w - wallT / 2, y - wallT / 2, wallT, h + wallT, wall);
    }

    // System icons: white-on-black art, multiplied so the fill turns green (powered) or grey
    // (unpowered) and the outline stays black. Drawn unscaled (see room_icon) so they stay sharp.
    C2D_SetTintMode(C2D_TintMult);
    for (int i = 0; i < s->systemCount; i++) {
        const ShipSystem* y = &s->systems[i];
        const Room* r = &s->rooms[y->room];
        const Texture* icon = room_icon(y, r, cam);
        if (!icon) continue;
        C2D_ImageTint tint;
        u32 col = y->ionTimer > 0.0f          ? C2D_Color32(120, 200, 255, 255)
                  : y->damage >= y->def->level ? C2D_Color32(235, 70, 60, 255)
                  : y->damage > 0            ? C2D_Color32(240, 160, 50, 255)
                  : y->power > 0             ? C2D_Color32(120, 235, 120, 255)
                                             : C2D_Color32(165, 165, 165, 255);
        C2D_PlainImageTint(&tint, col, 1.0f);
        float cx, cy;
        to_screen(cam, ox + (r->x + r->w * 0.5f) * TILE, oy + (r->y + r->h * 0.5f) * TILE, &cx, &cy);
        C2D_DrawImageAt(icon->image, floorf(cx - icon->width * 0.5f), floorf(cy - icon->height * 0.5f), 0.0f, &tint, 1.0f, 1.0f);
    }
    C2D_SetTintMode(C2D_TintSolid);

    // A hacking drone on one of this ship's systems.
    if (s->hackedSys >= 0 && s->hackedSys < s->systemCount) {
        const Room* r = &s->rooms[s->systems[s->hackedSys].room];
        if (s->hackPulse > 0.0f)
            draw_rect(cam, ox + r->x * TILE, oy + r->y * TILE, r->w * TILE, r->h * TILE,
                      C2D_Color32(170, 80, 230, (u8)(70 + 50 * sinf(sClock * 8.0f))));
        float X, Y;
        to_screen(cam, ox + (r->x + r->w) * TILE - 8, oy + r->y * TILE + 8, &X, &Y);
        C2D_DrawCircleSolid(X, Y, 0, 5, C2D_Color32(20, 10, 30, 255));
        C2D_DrawCircleSolid(X, Y, 0, 3.5f, C2D_Color32(200, 110, 255, 255));
    }

    // Damaged systems: one red pip per broken bar under the icon, and while someone works on it,
    // a bar filling towards the next repaired bar.
    for (int i = 0; i < s->systemCount; i++) {
        const ShipSystem* y = &s->systems[i];
        if (y->damage <= 0 || room_hidden(s, y->room)) continue;
        const Room* r = &s->rooms[y->room];
        float cx, cy;
        to_screen(cam, ox + (r->x + r->w * 0.5f) * TILE, oy + (r->y + r->h * 0.5f) * TILE, &cx, &cy);
        const Texture* icon = room_icon(y, r, cam);
        float iconH = icon ? icon->height : 16;
        float top = cy + iconH * 0.5f - 2;
        float pw = 4, gap = 1, total = y->damage * (pw + gap) - gap;
        C2D_DrawRectSolid(cx - total * 0.5f - 1, top - 1, 0, total + 2, 5, C2D_Color32(10, 10, 10, 220));
        for (int b = 0; b < y->damage; b++)
            C2D_DrawRectSolid(cx - total * 0.5f + b * (pw + gap), top, 0, pw, 3, C2D_Color32(230, 60, 50, 255));
        if (y->repair > 0.0f) {
            float bw = 24;
            C2D_DrawRectSolid(cx - bw * 0.5f - 1, top + 5, 0, bw + 2, 5, C2D_Color32(10, 10, 10, 220));
            C2D_DrawRectSolid(cx - bw * 0.5f, top + 6, 0, bw * y->repair, 3, C2D_Color32(240, 200, 60, 255));
        }
    }

    // Doors: door_sheet.png has 5 frames from closed to open, one row per door level.
    int doorsSys = ship_find_system(s, SYS_DOORS);
    int row = doorsSys >= 0 ? s->systems[doorsSys].def->level - 1 : 0;
    if (row < 0) row = 0;
    if (row > 3) row = 3;
    const float doorLen = TILE - 10.0f, doorT = 6.0f;
    for (int i = 0; i < s->doorCount; i++) {
        const Door* d = &s->doors[i];
        float x = ox + d->x * TILE, y = oy + d->y * TILE;
        if (s->doorSheet.loaded) {
            int frame = (int)(s->doorState[i].anim * 4.0f + 0.5f);
            float cx = d->vertical ? x : x + TILE * 0.5f, cy = d->vertical ? y + TILE * 0.5f : y;
            float X, Y;
            to_screen(cam, cx, cy, &X, &Y);
            sheet_draw_frame_rotated(&s->doorSheet, TILE, TILE, frame, row, X, Y, cam->zoom,
                                     (d->vertical ? 0.0f : (float)M_PI_2) + cam_angle(cam));
        } else if (d->vertical) {
            draw_rect(cam, x - doorT / 2, y + (TILE - doorLen) / 2, doorT, doorLen, doorCol);
        } else {
            draw_rect(cam, x + (TILE - doorLen) / 2, y - doorT / 2, doorLen, doorT, doorCol);
        }
    }
}

// Weapons sit on the hull's mounts: charging frames up to the charged frame, then the firing
// frames after each shot. Player mounts are rotated to face the enemy.
static void draw_mounted(const Ship* s, const Camera* cam, const WeaponSlot* w, int mount, float charge01);

static void draw_weapons(const Ship* s, const Camera* cam) {
    if (s->deathTime >= 0.0f) return;
    for (int i = 0; i < s->weaponCount && i < s->mountCount; i++) {
        const WeaponSlot* w = &s->weapons[i];
        draw_mounted(s, cam, w, i, w->powered && w->fullCharge > 0 ? w->charge / w->fullCharge : 0.0f);
    }
    // Artillery (the Federation cruiser's, the flagship's guns) sits on the last mounts.
    for (int k = 0; k < s->artilleryCount; k++) {
        int mount = s->mountCount - s->artilleryCount + k;
        float full = ship_artillery_time(s, k);
        if (mount >= s->weaponCount && mount >= 0) draw_mounted(s, cam, &s->artillery[k], mount, full > 0 ? s->artillery[k].charge / full : 0.0f);
    }
}

// One weapon's art on a mount: charging frames up to the charged frame, then the firing frames.
static void draw_mounted(const Ship* s, const Camera* cam, const WeaponSlot* w, int i, float charge01) {
        const WeaponMount* m = &s->mounts[i];
        if (!w->art.loaded) return;
        int frame = 0;
        if (w->burstLeft > 0 || w->fireAnim < 0.35f) {
            int n = w->artFrames - w->fireFrame;
            frame = w->fireFrame + (int)(w->fireAnim / 0.35f * n);
            if (frame >= w->artFrames) frame = w->artFrames - 1;
        } else {
            frame = (int)((charge01 > 1.0f ? 1.0f : charge01) * w->chargedFrame);
        }
        float mx, my;
        combat_mount_point(s, i, &mx, &my);
        // Offset from the mount point to the frame centre, mirrored and rotated like the sprite.
        float dx = w->artFw * 0.5f - w->mountPx, dy = w->artFh * 0.5f - w->mountPy;
        if (m->mirror) dx = -dx;
        float angle = m->rotate ? (float)M_PI_2 : 0.0f;
        float rx = m->rotate ? -dy : dx, ry = m->rotate ? dx : dy;
        float cx, cy;
        to_screen(cam, mx, my, &cx, &cy);
        cx += rx * cam->zoom;
        cy += ry * cam->zoom;
        Tex3DS_SubTexture sub = {
            .width = (u16)w->artFw,
            .height = (u16)w->artFh,
            .left = (float)((w->artX + frame) * w->artFw) / w->art.image.tex->width,
            .right = (float)((w->artX + frame + 1) * w->artFw) / w->art.image.tex->width,
            .top = 1.0f - (float)(w->artY * w->artFh) / w->art.image.tex->height,
            .bottom = 1.0f - (float)((w->artY + 1) * w->artFh) / w->art.image.tex->height,
        };
        C2D_Image img = {w->art.image.tex, &sub};
        C2D_DrawImageAtRotated(img, cx, cy, 0.0f, angle, NULL, m->mirror ? -cam->zoom : cam->zoom, cam->zoom);
}

// Shield bubble, brighter for each layer and flashing when hit.
static void draw_shields(const Ship* s, const Camera* cam, float flash) {
    if (s->deathTime >= 0.0f) return;
    if (s->superShield > 0) {
        float zx, zy, zw, zh, X, Y;
        combat_shield_ellipse(s, &zx, &zy, &zw, &zh);
        to_screen(cam, zx, zy, &X, &Y);
        int full = s->superShieldMax > 0 ? s->superShieldMax : 5;
        u8 a = (u8)(40 + 80 * (s->superShield > full ? full : s->superShield) / full);
        float w = zw * 1.08f * cam->zoom, h = zh * 1.08f * cam->zoom;
        if (cam->rotated) {
            float t = w;
            w = h;
            h = t;
        }
        C2D_DrawEllipseSolid(X - w, Y - h, 0, 2 * w, 2 * h, C2D_Color32(90, 230, 120, a));
    }
    if (s->shieldLayers <= 0 && flash <= 0.0f) return;
    float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    float cx = ox + s->hullX + s->hull.width * 0.5f, cy = oy + s->hullY + s->hull.height * 0.5f;
    if (!s->bp->enemy && s->roomCount > 0) {
        // Player ships: the rooms' centre plus the ELLIPSE offset (the shield image is 2w x 2h).
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < s->roomCount; i++) {
            const Room* r = &s->rooms[i];
            if (r->x * TILE < x0) x0 = r->x * TILE;
            if (r->y * TILE < y0) y0 = r->y * TILE;
            if ((r->x + r->w) * TILE > x1) x1 = (r->x + r->w) * TILE;
            if ((r->y + r->h) * TILE > y1) y1 = (r->y + r->h) * TILE;
        }
        cx = ox + (x0 + x1) * 0.5f + s->ellipse[2];
        cy = oy + (y0 + y1) * 0.5f + s->ellipse[3];
    }
    float rw = s->ellipse[0] > 0 ? s->ellipse[0] : s->hull.width * 0.55f;
    float rh = s->ellipse[1] > 0 ? s->ellipse[1] : s->hull.height * 0.55f;
    float a = 0.25f + 0.12f * s->shieldLayers + (flash > 0.0f ? flash : 0.0f);
    if (a > 0.9f) a = 0.9f;
    if (s->shieldImage.loaded) {
        C2D_ImageTint tint;
        C2D_AlphaImageTint(&tint, a);
        float scx = 2.0f * rw / s->shieldImage.width * cam->zoom, scy = 2.0f * rh / s->shieldImage.height * cam->zoom;
        float X, Y;
        to_screen(cam, cx - rw, cy - rh, &X, &Y);
        C2D_DrawImageAt(s->shieldImage.image, X, Y, 0.0f, &tint, scx, scy);
    } else {
        float X, Y;
        to_screen(cam, cx - rw, cy - rh, &X, &Y);
        C2D_DrawEllipseSolid(X, Y, 0.0f, 2 * rw * cam->zoom, 2 * rh * cam->zoom,
                             C2D_Color32(80, 160, 255, (u8)(a * 120)));
    }
}

// Ship-space centre of a door (room-grid origin, no X/Y_OFFSET).
static void door_centre(const Ship* s, int i, float* x, float* y) {
    const Door* d = &s->doors[i];
    *x = d->x * TILE + (d->vertical ? 0.0f : TILE * 0.5f);
    *y = d->y * TILE + (d->vertical ? TILE * 0.5f : 0.0f);
}

static int door_pick(const Ship* s, float px, float py, float radius) {
    int best = -1;
    float bestD = radius * radius;
    for (int i = 0; i < s->doorCount; i++) {
        float x, y;
        door_centre(s, i, &x, &y);
        float d = (x - px) * (x - px) + (y - py) * (y - py);
        if (d <= bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// Below this zoom a 35-pixel tile is under ~16 screen pixels and crew sprites turn to mush.
#define CREW_MARKER_ZOOM 0.45f

// FTL's skill icons (img/people/skill_*_white.png, 26x26), tinted by level: white, green, gold.
static Texture sSkillIcon[SKILL_COUNT];

static const Texture* skill_icon(int skill) {
    static bool tried;
    if (!tried) {
        tried = true;
        static const char* const names[SKILL_COUNT] = {"pilot", "engines", "shields", "weapons", "repair", "combat"};
        for (int k = 0; k < SKILL_COUNT; k++) {
            char path[48];
            snprintf(path, sizeof(path), "img/people/skill_%s_white.png", names[k]);
            load_crop(&sSkillIcon[k], path, 0, 0, 0, 0, false);
        }
    }
    return &sSkillIcon[skill];
}

static u32 skill_colour(int level) {
    return level >= 2 ? C2D_Color32(255, 210, 50, 255) : level == 1 ? C2D_Color32(110, 235, 110, 255)
                                                                 : C2D_Color32(235, 235, 235, 255);
}

// A skill icon centred on (cx, cy), tinted.
static void draw_skill_icon(int skill, float cx, float cy, u32 col) {
    const Texture* t = skill_icon(skill);
    if (!t->loaded) return;
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, col, 1.0f);
    C2D_DrawImageAt(t->image, floorf(cx - 13), floorf(cy - 13), 0.0f, &tint, 1.0f, 1.0f);
}

static void draw_crew(const Ship* s, const CrewSet* cs, const Camera* cam) {
    if (s->deathTime >= 0.0f) return;
    const float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    const u32 select = SELECT_COL;

    // Lockdown: a crystal coating over the room, fading in its last two seconds.
    for (int r = 0; r < s->roomCount; r++) {
        if (s->lockdown[r] <= 0.0f) continue;
        const Room* rm = &s->rooms[r];
        float k = s->lockdown[r] < 2.0f ? s->lockdown[r] / 2.0f : 1.0f;
        float x = ox + rm->x * TILE, y = oy + rm->y * TILE, w = rm->w * TILE, h = rm->h * TILE, t = 3.0f;
        draw_rect(cam, x, y, w, h, C2D_Color32(120, 220, 255, (u8)(70 * k)));
        u32 edge = C2D_Color32(170, 240, 255, (u8)(230 * k));
        draw_rect(cam, x, y, w, t, edge);
        draw_rect(cam, x, y + h - t, w, t, edge);
        draw_rect(cam, x, y, t, h, edge);
        draw_rect(cam, x + w - t, y, t, h, edge);
    }

    // Destination markers for anyone still walking.
    for (int i = 0; i < cs->count; i++) {
        const Crew* c = &cs->crew[i];
        if (c->pathPos < c->pathLen && !room_hidden(s, ship_room_at(s, c->destX, c->destY)))
            draw_image(cam, &cs->destMarker, ox + c->destX * TILE, oy + c->destY * TILE);
    }
    // When tiles are too small for sprites, crew are drawn as markers coloured by health.
    bool compact = cam->zoom < CREW_MARKER_ZOOM;
    for (int i = 0; i < cs->count; i++) {
        const Crew* c = &cs->crew[i];
        if (room_hidden(s, ship_room_at(s, (int)floorf(c->x / TILE), (int)floorf(c->y / TILE)))) {
            if (sLifeSense && !c->drone) {  // a life sign: where, not who
                float X, Y;
                to_screen(cam, ox + c->x, oy + c->y, &X, &Y);
                float r = 3.5f + 1.2f * sinf(sClock * 4.0f + i);
                C2D_DrawCircleSolid(X, Y, 0, r + 1.5f, C2D_Color32(20, 20, 20, 200));
                C2D_DrawCircleSolid(X, Y, 0, r, C2D_Color32(230, 70, 60, 230));
            }
            continue;
        }
        float X, Y;
        to_screen(cam, ox + c->x, oy + c->y, &X, &Y);
        float hp = c->health / cs->races[c->race].maxHealth;
        bool hostile = s->bp->enemy != c->foreign;  // the player's enemies, wherever they stand
        u32 hpCol = hp > 0.5f ? C2D_Color32(100, 220, 100, 255) : hp > 0.25f ? C2D_Color32(230, 200, 60, 255)
                                                                 : C2D_Color32(230, 60, 60, 255);
        if (compact || !cs->races[c->race].tex.loaded) {  // markers, or no sprite loaded
            if (crew_is_selected(cs, i)) C2D_DrawCircleSolid(X, Y, 0, 6.5f, select);
            // Enemy crew get a red rim so they can't be mistaken for ours.
            C2D_DrawCircleSolid(X, Y, 0, 4.5f, hostile ? C2D_Color32(220, 40, 40, 255) : C2D_Color32(10, 10, 10, 255));
            C2D_DrawCircleSolid(X, Y, 0, 3.2f, hpCol);
            continue;
        }
        if (crew_is_selected(cs, i)) {
            float t = 1.5f / cam->zoom > 1.5f ? 1.5f / cam->zoom : 1.5f;
            draw_outline(cam, ox + c->x - 12, oy + c->y - 14, 24, 28, t, select);
        }
        crew_draw(cs, i, X, Y, cam->zoom);
        if (c->fighting) {
            // Extinguisher spray: puffs drifting out the way they face, growing and fading.
            static const float DX[4] = {0, 1, 0, -1}, DY[4] = {1, 0, -1, 0};
            float fx = DX[c->dir & 3], fy = DY[c->dir & 3];
            if (cam->rotated) {
                float a = cam_angle(cam), rx = fx * cosf(a) - fy * sinf(a), ry = fx * sinf(a) + fy * cosf(a);
                fx = rx;
                fy = ry;
            }
            for (int k = 0; k < 6; k++) {
                float ph = fmodf(sClock * 1.8f + k / 6.0f + i * 0.37f, 1.0f);
                float d = (5.0f + ph * 15.0f) * cam->zoom, side = sinf(k * 2.1f + sClock * 3.0f) * ph * 5.0f * cam->zoom;
                float px = X + fx * d - fy * side, py = Y - 4 * cam->zoom + fy * d + fx * side;
                C2D_DrawCircleSolid(px, py, 0, (1.2f + ph * 3.0f) * cam->zoom, C2D_Color32(225, 238, 255, (u8)((1.0f - ph) * 190)));
            }
        }
        if (c->mind > 0.0f) {
            float r = 9.0f * (cam->zoom < 0.6f ? 0.6f : cam->zoom);
            for (int k = 0; k < 8; k++) {
                float a = sClock * 2.0f + k * 0.785f;
                C2D_DrawCircleSolid(X + cosf(a) * r, Y + sinf(a) * r, 0, 1.4f, C2D_Color32(200, 110, 255, 255));
            }
        }
        if (c->stun > 0.0f) {
            float a = sClock * 6.0f, r = 7.0f * (cam->zoom < 0.6f ? 0.6f : cam->zoom);
            for (int k = 0; k < 2; k++)
                C2D_DrawCircleSolid(X + cosf(a + k * 3.14f) * r, Y - 12 * cam->zoom + sinf(a + k * 3.14f) * r * 0.4f, 0, 2,
                                    C2D_Color32(255, 230, 80, 255));
        }
        if (c->levelUp > 0.0f && c->levelUpSkill >= 0) {
            // A new skill level: its icon rises over their head in the level's colour.
            float t = 2.5f - c->levelUp;
            if (t < 0.0f) t = 0.0f;
            draw_skill_icon(c->levelUpSkill, X, Y - 26 * (cam->zoom < 0.6f ? 0.6f : cam->zoom) - t * 6,
                            skill_colour(crew_skill_level(cs, c, c->levelUpSkill)));
        }
        if (hp < 1.0f || hostile) {
            float bh = 3.0f / cam->zoom > 4.0f ? 3.0f / cam->zoom : 4.0f;
            float bw = 12.0f / cam->zoom > 24.0f ? 12.0f / cam->zoom : 24.0f;
            float bx = ox + c->x - bw * 0.5f, by = oy + c->y - 16.0f - bh;
            draw_rect(cam, bx - 1 / cam->zoom, by - 1 / cam->zoom, bw + 2 / cam->zoom, bh + 2 / cam->zoom,
                      hostile ? C2D_Color32(220, 40, 40, 255) : C2D_Color32(10, 10, 10, 255));
            draw_rect(cam, bx, by, bw * hp, bh, hpCol);
        }
    }
}

// ---- Text --------------------------------------------------------------------

static C2D_TextBuf textBuf;

// The run (sector map, events, stores). Outside a run, fights are one-offs from the debug menu.
static Run sRun;
static bool sInRun;

// The augments as counts on the ship (the run's list; outside a run, the blueprint's own), and
// what they do to walking speeds: Mantis Pheromones for our crew (here or boarding), the Drone
// Reactor Booster for our crew drones.
static void sync_augs(Ship* ps, CrewSet* pc, CrewSet* ec) {
    memset(ps->aug, 0, sizeof(ps->aug));
    for (int i = 0; i < (sInRun ? sRun.augCount : ps->bp->augCount); i++)
        for (int k = 0; k < AUG_COUNT; k++) ps->aug[k] += !strcmp(sInRun ? sRun.augs[i] : ps->bp->augs[i], AUG_NAMES[k]);
    for (int i = 0; !sInRun && i < sExtraAugCount; i++)
        for (int k = 0; k < AUG_COUNT; k++) ps->aug[k] += !strcmp(sExtraAugs[i], AUG_NAMES[k]);
    float crew = ps->aug[AUG_CREW_STIMS] ? 1.25f : 1.0f, drones = ps->aug[AUG_DRONE_SPEED] ? 1.25f : 1.0f;
    pc->boost[0][0] = crew;
    pc->boost[0][1] = drones;
    pc->boost[1][0] = pc->boost[1][1] = 1.0f;
    if (ec) {
        ec->boost[1][0] = crew;
        ec->boost[1][1] = drones;
        ec->boost[0][0] = ec->boost[0][1] = 1.0f;
    }
    sLifeSense = ps->aug[AUG_LIFE_SCANNER] > 0;
    for (int i = 0; i < pc->count && !sLifeSense; i++)
        sLifeSense = !pc->crew[i].foreign && !strcmp(pc->races[pc->crew[i].race].name, "slug");
}
static ShipBlueprint sEnemyBp;  // the current enemy, generated from its blueprint for the sector (FTL's generator)
static bool sCanJump;   // the JUMP button works: no fight, or the FTL drive is charged mid-fight
static bool sFighting;  // an enemy is here and the fight is on
static bool sMapOpen, sStoreOpen;
static bool sSectorMapOpen;  // choosing the next sector
static int sSectorPick = -1, sNextRow;

// The FTL jump: the ship streaks off to the right, a white flash, then it flies in from the left
// at the new beacon. Timings are our choice (no source gives FTL's).
#define JUMP_OUT 0.7f
#define JUMP_IN 0.5f
static float sJumpTime = -1.0f;  // seconds into the jump, or < 0
// The FTL drive's charge (0..1) during a fight; outside fights it's ready at once. Seconds to
// charge by engine power: unmanned, then manned by engines skill 0/1/2 (wiki: Engines, "FTL Drive
// charge time").
static float sFtlCharge = 1.0f;
// The enemy's surrender / escape thresholds (hull points, 0: none) and FTL escape countdown.
static int sSurrenderHp, sEscapeHp;
static float sEscapeTime, sEscapeLeft = -1.0f;
static const float FTL_TIME[4][8] = {{67.9f, 53.1f, 43.6f, 36.9f, 32.1f, 28.3f, 25.4f, 23.0f},
                                     {61.8f, 48.3f, 39.6f, 33.6f, 29.0f, 25.7f, 23.1f, 20.9f},
                                     {58.0f, 45.4f, 37.2f, 31.6f, 27.5f, 24.2f, 21.7f, 19.6f},
                                     {54.3f, 42.6f, 34.9f, 29.5f, 25.7f, 22.6f, 20.3f, 18.4f}};
static int sJumpTarget;          // beacon, or -1 for the next sector

// How far right of its place the ship is drawn, in screen pixels.
static float jump_offset(void) {
    if (sJumpTime < 0.0f) return 0.0f;
    if (sJumpTime < JUMP_OUT) {
        float t = sJumpTime / JUMP_OUT;
        return t * t * t * (TOP_W + 200);
    }
    float k = 1.0f - (sJumpTime - JUMP_OUT) / JUMP_IN;
    return -k * k * k * (TOP_W + 200);
}

// Text uses FTL's bitmap fonts at 1:1 pixels (font.c); sizes are the old system-font scales. The
// fonts are shorter than the system font was, so text is nudged down to keep layouts centred.
static float text_nudge(float scale) { return floorf(scale * 6.0f); }

static void draw_text(float x, float y, float scale, u32 color, const char* str) {
    font_draw(x, y + text_nudge(scale), scale, color, str, 1.0f);
}

// Text that must fit in maxW: a smaller font first, then squeezed.
static void draw_text_fit_left(float x, float y, float scale, float maxW, u32 color, const char* str) {
    float w = font_width(scale, str);
    if (w > maxW && scale >= 0.42f && font_width(0.4f, str) <= maxW) {
        font_draw(x, y + text_nudge(scale) + 1, 0.4f, color, str, 1.0f);
        return;
    }
    font_draw(x, y + text_nudge(scale), scale, color, str, w > maxW ? maxW / w : 1.0f);
}

// Centred in a box of width w.
static void draw_text_centred(float x, float y, float w, float scale, u32 color, const char* str) {
    float tw = font_width(scale, str);
    if (tw > w) {
        draw_text_fit_left(x, y, scale, w, color, str);
        return;
    }
    draw_text(floorf(x + (w - tw) * 0.5f), y, scale, color, str);
}

// A crew name at a readable size. Long names fall back to the first name, and only then shrink.
static void draw_name(float x, float y, float maxW, u32 color, const char* name) {
    const float scale = 0.46f;
    const char* space = strchr(name, ' ');
    if (font_width(scale, name) > maxW && space) {
        char first[24];
        snprintf(first, sizeof(first), "%.*s", (int)(space - name), name);
        draw_text_fit_left(x, y, scale, maxW, color, first);
        return;
    }
    draw_text_fit_left(x, y, scale, maxW, color, name);
}

// ---- Bottom screen -----------------------------------------------------------

static int bar_slot_width(const CrewSet* cs) {
    int n = crew_native_count(cs);
    return n > 0 ? BOT_W / (n > 4 ? n : 4) : BOT_W;
}

// Crew index of the n-th own (non-boarder) crew member, or -1.
static int bar_crew(const CrewSet* cs, int n) {
    for (int i = 0; i < cs->count; i++)
        if (!cs->crew[i].foreign && !cs->crew[i].drone && n-- == 0) return i;
    return -1;
}

// Pause button, just left of the tabs.
#define PAUSE_W 48
#define PAUSE_X (BOT_W - TAB_COUNT * TAB_W - PAUSE_W - 2)

static void round_rect(float x, float y, float w, float h, float r, u32 col);
static void ftl_button(float x, float y, float w, float h, bool lit, const char* label);
#define SHIPBTN_W 40
#define JUMPBTN_X 43
#define JUMPBTN_W (PAUSE_X - JUMPBTN_X - 2)

static void draw_tabs(const Ship* s, int tab, int index, int count, bool paused) {
    static const char* const labels[TAB_COUNT] = {"CREW", "FIGHT", "SYS", "POWER"};
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(150, 160, 170, 255);
    C2D_DrawRectSolid(0, 0, 0, BOT_W, TAB_H, C2D_Color32(20, 26, 36, 255));
    for (int i = 0; i < TAB_COUNT; i++) {
        float x = BOT_W - TAB_COUNT * TAB_W + i * TAB_W;
        // FTL-style tabs: rounded, light edge on the selected one.
        round_rect(x + 1, 1, TAB_W - 2, TAB_H - 1, 3, i == tab ? C2D_Color32(235, 235, 235, 255) : C2D_Color32(70, 80, 98, 255));
        round_rect(x + 2.5f, 2.5f, TAB_W - 5, TAB_H - 4, 2, i == tab ? C2D_Color32(40, 90, 50, 255) : C2D_Color32(30, 37, 47, 255));
        draw_text_centred(x, 3, TAB_W, 0.4f, i == tab ? white : dim, labels[i]);
    }
    C2D_DrawRectSolid(PAUSE_X + 1, 1, 0, PAUSE_W - 2, TAB_H - 2, paused ? C2D_Color32(170, 130, 30, 255) : C2D_Color32(40, 46, 60, 255));
    draw_text_fit_left(PAUSE_X + 4, 2, 0.42f, PAUSE_W - 8, paused ? white : dim, paused ? "RESUME" : "PAUSE");
    if (sInRun) {
        ftl_button(1, 1, SHIPBTN_W, TAB_H - 1, false, "SHIP");
        if (sCanJump) {
            ftl_button(JUMPBTN_X, 1, JUMPBTN_W, TAB_H - 1, true, "JUMP");
        } else {
            // Charging: the FTL drive's progress under the label.
            ftl_button(JUMPBTN_X, 1, JUMPBTN_W, TAB_H - 1, false, "FTL");
            C2D_DrawRectSolid(JUMPBTN_X + 4, TAB_H - 4, 0, (JUMPBTN_W - 8) * sFtlCharge, 2, C2D_Color32(90, 200, 255, 255));
        }
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d/%d %s", index + 1, count, s->bp->name + 12);  // skip "PLAYER_SHIP_"
        draw_text_fit_left(4, 2, 0.45f, PAUSE_X - 6, white, buf);
    }
}

// The one crew member selected, if they're a Crystal (who can lock down a room), or -1.
static int selected_crystal(const CrewSet* cs) {
    int sel = -1;
    for (int i = 0; i < cs->count; i++)
        if (crew_is_selected(cs, i)) {
            if (sel >= 0) return -1;
            sel = i;
        }
    return sel >= 0 && !cs->crew[sel].drone && !strcmp(cs->races[cs->crew[sel].race].name, "crystal") ? sel : -1;
}

static void draw_crew_tab(const Ship* s, const CrewSet* cs, const Camera* map) {
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(150, 160, 170, 255);
    draw_ship(s, map);
    draw_crew(s, cs, map);

    // Crew bar: one slot per crew member.
    C2D_DrawRectSolid(0, MAP_H, 0, BOT_W, BAR_H, C2D_Color32(20, 26, 36, 255));
    C2D_DrawRectSolid(0, MAP_H, 0, BOT_W, 1, C2D_Color32(80, 90, 110, 255));
    int w = bar_slot_width(cs);
    for (int slot = 0; bar_crew(cs, slot) >= 0; slot++) {
        int i = bar_crew(cs, slot);
        float x = (float)(slot * w);
        const Crew* c = &cs->crew[i];
        const Race* r = &cs->races[c->race];
        C2D_DrawRectSolid(x + 1, MAP_H + 2, 0, w - 2, BAR_H - 3,
                          crew_is_selected(cs, i) ? SELECT_BG : C2D_Color32(32, 40, 54, 255));

        // Wide slots: portrait on the left, name and health beside it. Narrow slots (more than
        // four crew): portrait over a health bar.
        bool wide = w >= 56;
        const int* b = r->portrait;
        float fh = (float)(b[3] - b[1] + 1), scale = fh > 18.0f ? 18.0f / fh : 1.0f;
        float pcx = wide ? x + 13 : x + w * 0.5f, pcy = MAP_H + (wide ? 12.5f : 10.5f);  // figure centre
        sheet_draw_frame(&r->tex, r->fw, r->fh, r->walk[DIR_DOWN].x, r->rows - 1 - r->walk[DIR_DOWN].y,
                         floorf(pcx - (b[0] + b[2] + 1) * 0.5f * scale), floorf(pcy - (b[1] + b[3] + 1) * 0.5f * scale),
                         scale);

        float hp = c->health / r->maxHealth;
        float hx = wide ? x + 26 : x + 4, hy = wide ? MAP_H + 17 : MAP_H + 19, hw = wide ? w - 31 : w - 8;
        u32 hpCol = hp > 0.5f ? C2D_Color32(100, 220, 100, 255) : C2D_Color32(230, 60, 60, 255);
        C2D_DrawRectSolid(hx, hy, 0, hw, 3, C2D_Color32(55, 64, 80, 255));
        C2D_DrawRectSolid(hx, hy, 0, hw * hp, 3, hpCol);
        if (wide) draw_name(x + 26, MAP_H + 3, w - 30, crew_is_selected(cs, i) ? white : C2D_Color32(205, 212, 220, 255), c->name);
    }
    if (crew_native_count(cs) == 0) draw_text(4, MAP_H + 5, 0.45f, dim, "No crew aboard.");
    // Narrow slots (5+ crew) have no room for names: the selection gets a label above the bar.
    if (w < 56 && cs->selected) {
        int n = 0, first = -1;
        for (int i = 0; i < cs->count; i++)
            if (crew_is_selected(cs, i)) {
                n++;
                if (first < 0) first = i;
            }
        char label[48];
        if (n == 1) {
            const Crew* c = &cs->crew[first];
            snprintf(label, sizeof(label), "%s  %d/%d", c->name, (int)ceilf(c->health), (int)cs->races[c->race].maxHealth);
        } else {
            snprintf(label, sizeof(label), "%d crew selected", n);
        }
        float lw = font_width(0.46f, label) + 12;
        round_rect(2, MAP_H - 18, lw, 16, 4, SELECT_COL);
        round_rect(3, MAP_H - 17, lw - 2, 14, 3, C2D_Color32(30, 28, 14, 255));
        draw_text(8, MAP_H - 18, 0.46f, white, label);
    }

    // Open all / close all doors, with FTL's icons (img/systemUI/button_door_*_on.png).
    static Texture doorIcon[2];
    static bool doorTried;
    if (!doorTried) {
        doorTried = true;
        // FTL's bottom door button opens (arrows apart), the top one closes (arrows together).
        load_crop(&doorIcon[0], "img/systemUI/button_door_bottom_on.png", 10, 33, 20, 20, false);
        load_crop(&doorIcon[1], "img/systemUI/button_door_top_on.png", 10, 10, 20, 20, false);
    }
    for (int i = 0; i < 2; i++) {
        float bx = DOORBTN_X + i * (DOORBTN_W + 2);
        ftl_button(bx, DOORBTN_Y, DOORBTN_W, DOORBTN_H, false, "");
        if (doorIcon[i].loaded) C2D_DrawImageAt(doorIcon[i].image, bx + 2, DOORBTN_Y + 2, 0.0f, NULL, 1.0f, 1.0f);
    }
    // Save stations / return to stations (img/statusUI/button_station_*_on.png).
    static Texture stationIcon[2];
    static bool stationTried;
    if (!stationTried) {
        stationTried = true;
        load_crop(&stationIcon[0], "img/statusUI/button_station_assign_on.png", 0, 0, 20, 20, false);
        load_crop(&stationIcon[1], "img/statusUI/button_station_return_on.png", 0, 0, 20, 20, false);
    }
    for (int i = 0; i < 2; i++) {
        float bx = STATIONBTN_X + i * (DOORBTN_W + 2);
        ftl_button(bx, DOORBTN_Y, DOORBTN_W, DOORBTN_H, false, "");
        if (stationIcon[i].loaded) C2D_DrawImageAt(stationIcon[i].image, bx + 2, DOORBTN_Y + 2, 0.0f, NULL, 1.0f, 1.0f);
    }
    ftl_button(ALLBTN_X, DOORBTN_Y, ALLBTN_W, 16, false, "ALL CREW");
    int cry = selected_crystal(cs);
    if (cry >= 0) {
        const Crew* c = &cs->crew[cry];
        bool ready = c->ability <= 0.0f;
        ftl_button(LOCKBTN_X, LOCKBTN_Y, LOCKBTN_W, LOCKBTN_H, ready, "LOCKDOWN");
        if (!ready) {  // recharging: a bar filling along the bottom
            float k = 1.0f - c->ability / 50.0f;
            C2D_DrawRectSolid(LOCKBTN_X + 3, LOCKBTN_Y + LOCKBTN_H - 4, 0, (LOCKBTN_W - 6) * k, 2, C2D_Color32(120, 220, 255, 255));
        }
    }
    (void)dim;
}

// Power tab layout: the reactor as a strip across the top, then one card per system (bars above
// a large icon), reactor systems first and subsystems after a gap.
#define REACTOR_Y (TAB_H + 5)
#define REACTOR_H 11
#define PANEL_TOP (TAB_H + 48)            // highest a card can reach
#define CARD_BOTTOM (BOT_H - 3)
#define ICON_H 30                         // icon area at the bottom of each card
#define BARS_BOTTOM (CARD_BOTTOM - ICON_H - 2)
#define MAX_PITCH 26
#define CARD_GAP 3
#define GROUP_GAP 10
#define PANEL_X 4

typedef struct {
    int col[MAX_SYSTEMS];  // system index for each card
    int count;
    int firstSub;  // first subsystem card
    float cardW, pitch;
    float cardTop;  // cards are as tall as the largest system needs
} PanelLayout;

static void panel_layout(const Ship* s, PanelLayout* L) {
    L->count = 0;
    L->firstSub = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) L->firstSub = L->count;
        for (int i = 0; i < s->systemCount; i++)
            if (system_is_subsystem(s->systems[i].def->type) == (pass == 1)) L->col[L->count++] = i;
    }
    bool twoGroups = L->firstSub > 0 && L->firstSub < L->count;
    float avail = BOT_W - 2 * PANEL_X - (L->count - 1) * CARD_GAP - (twoGroups ? GROUP_GAP : 0);
    L->cardW = L->count ? avail / L->count : avail;
    if (L->cardW > 40) L->cardW = 40;
    int maxBars = 1;
    for (int i = 0; i < s->systemCount; i++)
        if (s->systems[i].def->level > maxBars) maxBars = s->systems[i].def->level;
    L->pitch = (BARS_BOTTOM - PANEL_TOP - 5) / (float)maxBars;
    if (L->pitch > MAX_PITCH) L->pitch = MAX_PITCH;
    L->cardTop = BARS_BOTTOM - maxBars * L->pitch - 17;  // room for the power readout
}

static float panel_card_x(const PanelLayout* L, int c) {
    return PANEL_X + c * (L->cardW + CARD_GAP) + (c >= L->firstSub && L->firstSub > 0 ? GROUP_GAP : 0);
}

static void draw_power_tab(const Ship* s) {
    const u32 dim = C2D_Color32(140, 150, 165, 255);
    const u32 green = C2D_Color32(100, 220, 100, 255), greenHi = C2D_Color32(170, 245, 170, 255);
    const u32 blue = C2D_Color32(110, 170, 230, 255), blueHi = C2D_Color32(175, 210, 245, 255);
    const u32 empty = C2D_Color32(44, 52, 66, 255), cardBg = C2D_Color32(22, 28, 40, 255);
    const u32 border = C2D_Color32(52, 62, 80, 255), manned = C2D_Color32(230, 200, 60, 255);
    PanelLayout L;
    panel_layout(s, &L);

    // Reactor strip: lit segments are free power.
    int free = ship_reactor_free(s);
    char buf[16];
    draw_text(PANEL_X + 2, REACTOR_Y - 1, 0.4f, dim, "REACTOR");
    snprintf(buf, sizeof(buf), "%d / %d", free, s->reactor);
    draw_text(BOT_W - 40, REACTOR_Y - 1, 0.42f, free > 0 ? greenHi : dim, buf);
    float segX = 62, segAvail = BOT_W - 46 - segX;
    float segW = s->reactor > 0 ? (segAvail - (s->reactor - 1) * 2) / s->reactor : segAvail;
    if (segW > 16) segW = 16;
    for (int i = 0; i < s->reactor; i++) {
        float x = segX + i * (segW + 2);
        bool stormCut = i >= s->reactor - s->reactorCut;  // bars an ion storm has taken (dark red)
        C2D_DrawRectSolid(x, REACTOR_Y, 0, segW, REACTOR_H, stormCut ? C2D_Color32(90, 30, 30, 255) : i < free ? green : empty);
        if (i < free) C2D_DrawRectSolid(x, REACTOR_Y, 0, segW, 2, greenHi);
    }

    // Group labels.
    draw_text(PANEL_X + 2, REACTOR_Y + 16, 0.36f, dim, "Tap a bar to set power. Tap an icon for on/off.");

    float labelY = L.cardTop - 12;
    if (L.firstSub > 0) draw_text(panel_card_x(&L, 0), labelY, 0.36f, green, "SYSTEMS");
    if (L.firstSub < L.count) draw_text(panel_card_x(&L, L.firstSub), labelY, 0.36f, blue, "SUBSYSTEMS");

    for (int c = 0; c < L.count; c++) {
        const ShipSystem* y = &s->systems[L.col[c]];
        float x = panel_card_x(&L, c), w = L.cardW;
        bool sub = c >= L.firstSub;

        // Card with a 1px border, yellow when someone is manning the system.
        u32 edge = y->manned ? manned : border;
        C2D_DrawRectSolid(x, L.cardTop, 0, w, CARD_BOTTOM - L.cardTop, edge);
        C2D_DrawRectSolid(x + 1, L.cardTop + 1, 0, w - 2, CARD_BOTTOM - L.cardTop - 2, cardBg);

        // Power readout at the top of the card.
        char pw[8];
        snprintf(pw, sizeof(pw), "%d/%d", y->power, y->def->level);
        draw_text_centred(x, L.cardTop + 2, w, 0.4f, y->power > 0 ? (sub ? blueHi : greenHi) : dim, pw);

        // Bars from the bottom up, with a highlight on lit ones.
        int cap = y->def->level - y->damage, ionCap = cap - ship_system_ion(s, L.col[c]);
        for (int b = 0; b < y->def->level; b++) {
            float by = BARS_BOTTOM - (b + 1) * L.pitch + 1, bh = L.pitch - 2;
            bool lit = b < y->power;
            bool zolt = !lit && b < y->power + y->zoltan && b < ionCap;
            u32 col = b >= cap      ? C2D_Color32(200, 60, 50, 255)    // damaged
                      : b >= ionCap ? C2D_Color32(120, 200, 255, 255)  // locked by ion
                      : lit         ? (sub ? blue : green)
                      : zolt        ? C2D_Color32(190, 120, 240, 255)  // powered by a Zoltan
                                    : empty;
            C2D_DrawRectSolid(x + 4, by, 0, w - 8, bh, col);
            if (lit) C2D_DrawRectSolid(x + 4, by, 0, w - 8, 1, sub ? blueHi : greenHi);
        }

        // Icon, scaled by the symbol itself rather than its padded 64x64 image.
        const Texture* icon = y->power > 0 ? &y->panelOn : &y->panelOff;
        if (icon->loaded) {
            const int* ib = y->iconBounds;
            float gw = (float)(ib[2] - ib[0] + 1), gh = (float)(ib[3] - ib[1] + 1);
            float g = gw > gh ? gw : gh;
            float target = w - 8 < 26 ? w - 8 : 26;
            float scale = target / g;
            float cx = x + w * 0.5f, cy = CARD_BOTTOM - ICON_H * 0.5f - 1;
            C2D_DrawImageAt(icon->image, floorf(cx - (ib[0] + ib[2] + 1) * 0.5f * scale),
                            floorf(cy - (ib[1] + ib[3] + 1) * 0.5f * scale), 0.0f, NULL, scale, scale);
        }
    }
}

// Tap on the power tab: set a system's power to the tapped bar (tapping the top lit bar
// turns it off), or tap its icon to switch it fully on or off.
static void power_tab_touch(Ship* s, Combat* combat, int px, int py) {
    PanelLayout L;
    panel_layout(s, &L);
    if (py < L.cardTop) return;
    for (int c = 0; c < L.firstSub; c++) {
        float x = panel_card_x(&L, c);
        if (px < x || px >= x + L.cardW) continue;
        int si = L.col[c];
        ShipSystem* y = &s->systems[si];
        int before = y->power, want;
        if (py >= BARS_BOTTOM) {
            want = y->power > 0 ? 0 : y->def->level;
        } else {
            int k = (int)((BARS_BOTTOM - py) / L.pitch) + 1;
            if (k > y->def->level) k = y->def->level;
            want = k == y->power ? k - 1 : k;
        }
        if (y->def->type == SYS_WEAPONS)
            combat_set_weapons_power(s, want);
        else if (y->def->type == SYS_DRONES)
            combat_set_drones_power(combat, SIDE_PLAYER, want);
        else
            ship_set_power(s, si, want);
        if (y->power > before)
            sound_play("powerUpSystem");
        else if (y->power < before)
            sound_play("powerDownSystem");
        else if (want > before)
            sound_play("powerUpFail");
        return;
    }
}

// ---- Combat drawing --------------------------------------------------------------

static struct {
    Texture tex;
    int fw, fh, col, row, frames;
} sExplosion;

static void load_explosion(FtlDat* dat, const AnimDb* db) {
    AnimInfo a;
    SheetInfo sh;
    if (!animdb_anim(db, "explosion_big1", &a) || !animdb_sheet(db, a.sheet, &sh)) return;
    uint32_t size;
    uint8_t* png = ftldat_read(dat, sh.file, &size);
    if (png && texture_load_png(&sExplosion.tex, png, size)) {
        sExplosion.fw = sh.fw;
        sExplosion.fh = sh.fh;
        sExplosion.col = a.x;
        sExplosion.row = sh.h / sh.fh - 1 - a.y;
        // The anim says 12 frames, but the sheet that wins has 10.
        int inSheet = sh.w / sh.fw - a.x;
        sExplosion.frames = a.length < inSheet ? a.length : inSheet;
    }
    free(png);
}

// Screen pixels per unit of weapon speed at zoom 1: a laser (speed 60) crosses the top screen in
// about half a second, close to how fast shots move in the original.
#define PROJ_PX_PER_UNIT 14.0f

// Projectiles fly from one ship's view to the other's. When only one ship is on this screen
// (the other's camera is NULL), shots enter or leave at screen x = offX, level with that ship.
// Projectile art, from a ship weapon or a drone.
typedef struct {
    const Texture* tex;
    int fw, fh, x, y, frames;
    float time;
} ProjArt;

// Asteroids (img/asteroids/asteroid0-9.png) and the anti-ship battery's shot (img/weapons/pds.png).
static const Texture* hazard_tex(int kind, int variant) {
    static Texture rocks[10], pds;
    static bool tried;
    if (!tried) {
        tried = true;
        for (int k = 0; k < 10; k++) {
            char path[40];
            snprintf(path, sizeof(path), "img/asteroids/asteroid%d.png", k);
            load_crop(&rocks[k], path, 0, 0, 0, 0, false);
        }
        load_crop(&pds, "img/weapons/pds.png", 0, 0, 0, 0, false);
    }
    return kind == SHOT_PDS ? &pds : &rocks[variant % 10];
}

static ProjArt proj_art(const Combat* c, const Projectile* p) {
    ProjArt a;
    if (p->weapon == SHOT_ASTEROID || p->weapon == SHOT_PDS) {
        const Texture* t = hazard_tex(p->weapon, (int)(p - c->shots));
        return (ProjArt){t, t->width, t->height, 0, 0, 1, 1.0f};
    }
    if (p->weapon == SHOT_SURGE) {
        const Ship* s = c->ship[p->from];
        if (s->artilleryCount > 0) {
            const WeaponSlot* w = &s->artillery[0];
            return (ProjArt){&w->proj, w->projFw, w->projFh, w->projX, w->projY, w->projFrames, w->projTime};
        }
    }
    if (p->weapon == SHOT_SHARD) {
        const WeaponSlot* w = combat_shard_slot();
        if (w) return (ProjArt){&w->proj, w->projFw, w->projFh, w->projX, w->projY, w->projFrames, w->projTime};
        const Texture* t = hazard_tex(SHOT_ASTEROID, 0);
        return (ProjArt){t, t->width, t->height, 0, 0, 1, 1.0f};
    }
    if (SHOT_IS_ART(p->weapon)) {
        const WeaponSlot* w = &c->ship[p->from]->artillery[SHOT_ART_SLOT(p->weapon)];
        return (ProjArt){&w->proj, w->projFw, w->projFh, w->projX, w->projY, w->projFrames, w->projTime};
    }
    if (p->drone >= 0) {
        const DroneSlot* d = &c->ship[p->from]->drones[p->drone];
        a = (ProjArt){&d->proj, d->projFw, d->projFh, d->projX, d->projY, d->projFrames, d->projTime};
    } else {
        const WeaponSlot* w = &c->ship[p->from]->weapons[p->weapon];
        a = (ProjArt){&w->proj, w->projFw, w->projFh, w->projX, w->projY, w->projFrames, w->projTime};
    }
    return a;
}

static void draw_projectiles(const Combat* c, const Camera* cams[2], float offX) {
    for (int i = 0; i < MAX_SHOTS; i++) {
        const Projectile* p = &c->shots[i];
        if (!p->active) continue;
        const Camera* from = cams[p->from];
        const Camera* to = cams[!p->from];
        if (!from && !to) continue;
        const WeaponDef* def = combat_shot_def(c, p->from, p->weapon, p->drone);
        ProjArt art = proj_art(c, p);
        if (def->type == WEAPON_BOMB) {
            float elapsed = p->t * p->duration, appear = p->duration - 0.75f;
            if (!to || elapsed < appear || !art.tex->loaded) continue;
            float bx, by;
            to_screen(to, p->tx, p->ty, &bx, &by);
            int frame = (int)((elapsed - appear) / 0.75f * art.frames);
            if (frame >= art.frames) frame = art.frames - 1;
            sheet_draw_frame_rotated(art.tex, art.fw, art.fh, art.x + frame, art.y, bx, by, to->zoom, 0.0f);
            continue;
        }
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0, x, y, zoom;
        if (p->inTarget) {
            // Fired by a drone already in the target's space.
            if (!to) continue;
            to_screen(to, p->sx, p->sy, &x0, &y0);
            to_screen(to, p->tx, p->ty, &x1, &y1);
            x = x0 + (x1 - x0) * p->t;
            y = y0 + (y1 - y0) * p->t;
            zoom = to->zoom;
        } else {
            if (from) to_screen(from, p->sx, p->sy, &x0, &y0);
            if (to) to_screen(to, p->tx, p->ty, &x1, &y1);
            if (from && to) {
                // Both ships on screen: straight from the mount to the target.
                if (p->miss) {  // carry on past the target
                    x1 += (x1 - x0) * 0.3f;
                    y1 += (y1 - y0) * 0.3f;
                }
                x = x0 + (x1 - x0) * p->t;
                y = y0 + (y1 - y0) * p->t;
                zoom = from->zoom + (to->zoom - from->zoom) * p->t;
            } else {
                // One ship on screen: the shot moves at its real speed, leaving towards the edge just
                // after firing, or arriving from it just before it lands.
                zoom = from ? from->zoom : to->zoom;
                float v = p->speed * PROJ_PX_PER_UNIT * zoom, elapsed = p->t * p->duration;
                if (from) {
                    x1 = offX;
                    y1 = y0;
                    float dir = offX > x0 ? 1.0f : -1.0f;
                    x = x0 + dir * v * elapsed;
                    y = y0;
                    if ((x - offX) * dir > 0) continue;  // gone
                } else {
                    x0 = offX;
                    y0 = y1;
                    float dir = x1 > offX ? 1.0f : -1.0f;
                    x = x1 - dir * v * (p->duration - elapsed);
                    y = y1;
                    if ((offX - x) * dir > 0) continue;  // not here yet
                }
            }
        }
        float angle = atan2f(y1 - y0, x1 - x0);
        if (p->weapon == SHOT_ASTEROID) angle = p->t * p->duration * 2.79f + i;
        if (art.tex->loaded) {
            if (art.fh > art.fw) angle += (float)M_PI_2;  // missiles are drawn nose-up
            int frame = art.frames > 1 ? (int)(p->t * p->duration / art.time * art.frames) % art.frames : 0;
            sheet_draw_frame_rotated(art.tex, art.fw, art.fh, art.x + frame, art.y, x, y, zoom, angle);
        } else {
            C2D_DrawCircleSolid(x, y, 0, 3, C2D_Color32(255, 80, 60, 255));
        }
    }
}

// Drones flying in a ship's space (ours around our ship, theirs combat drones too).
static void draw_drones(const Combat* c, int space, const Camera* cam) {
    for (int i = 0; i < MAX_DRONE_ENTITIES; i++) {
        const DroneEntity* e = &c->droneEnts[i];
        if (!e->active || e->space != space) continue;
        const DroneSlot* d = &c->ship[e->side]->drones[e->slot];
        float X, Y;
        to_screen(cam, e->x, e->y, &X, &Y);
        float scale = cam->zoom < 0.5f ? 0.5f : cam->zoom;  // drone art is padded; keep it readable
        bool charged = (d->def->type == DRONE_COMBAT && e->pause > 0.0f) ||
                       ((d->def->type == DRONE_DEFENSE || d->def->type == DRONE_SHIELD) && e->cooldown <= 0.0f);
        const Texture* layers[2] = {&d->base, charged ? &d->charged : &d->on};
        for (int k = 0; k < 2; k++) {
            const Texture* t = layers[k];
            if (!t->loaded || (k == 1 && (!d->powered || e->stun > 0.0f))) continue;
            C2D_DrawImageAt(t->image, X - t->width * scale * 0.5f, Y - t->height * scale * 0.5f, 0, NULL, scale, scale);
        }
        if (e->shotFlash > 0.0f) {
            float tx, ty;
            to_screen(cam, e->shotX, e->shotY, &tx, &ty);
            C2D_DrawLine(X, Y, C2D_Color32(120, 255, 140, 220), tx, ty, C2D_Color32(120, 255, 140, 220), 2.0f, 0);
        }
        if (e->stun > 0.0f) C2D_DrawCircleSolid(X, Y, 0, 5 * scale / 0.7f, C2D_Color32(120, 200, 255, 120));
    }
}

static void draw_beams(const Combat* c, int side, const Camera* cam) {
    for (int i = 0; i < MAX_BEAMS; i++) {
        const Beam* b = &c->beams[i];
        if (!b->active || b->from == side || b->delay > 0.0f) continue;
        float k = b->t > 1.0f ? 1.0f : b->t;
        float x0, y0, x1, y1;
        to_screen(cam, b->sx, b->sy, &x0, &y0);
        to_screen(cam, b->sx + (b->ex - b->sx) * k, b->sy + (b->ey - b->sy) * k, &x1, &y1);
        float fade = b->t > 1.0f ? 1.0f - (b->t - 1.0f) / 0.2f : 1.0f;
        u8 a = (u8)(255 * (fade < 0 ? 0 : fade));
        float w = 6.0f * cam->zoom < 3.0f ? 3.0f : 6.0f * cam->zoom;
        C2D_DrawLine(x0, y0, C2D_Color32(255, 60, 30, a / 2), x1, y1, C2D_Color32(255, 60, 30, a / 2), w * 2, 0);
        C2D_DrawLine(x0, y0, C2D_Color32(255, 140, 60, a), x1, y1, C2D_Color32(255, 140, 60, a), w, 0);
        C2D_DrawLine(x0, y0, C2D_Color32(255, 240, 200, a), x1, y1, C2D_Color32(255, 240, 200, a), w * 0.35f, 0);
        if (b->t <= 1.0f) C2D_DrawCircleSolid(x1, y1, 0, w * 1.2f, C2D_Color32(255, 230, 160, a));
    }
}

// Explosion animations, loaded the first time each is needed this beacon (a jump empties the cache).
static struct {
    char name[32];
    SheetAnim a;
    bool tried;
} sExplosionCache[16];

static void explosion_cache_clear(void) {  // wait for the GPU first
    for (int i = 0; i < 16; i++) texture_free(&sExplosionCache[i].a.tex);
    memset(sExplosionCache, 0, sizeof(sExplosionCache));
}

static const SheetAnim* explosion_anim(const char* name) {
#define cache sExplosionCache
    if (!name) return NULL;
    for (int i = 0; i < 16; i++) {
        if (cache[i].tried && !strcmp(cache[i].name, name)) return cache[i].a.tex.loaded ? &cache[i].a : NULL;
        if (!cache[i].tried) {
            cache[i].tried = true;
            snprintf(cache[i].name, sizeof(cache[i].name), "%s", name);
            load_sheet_anim(&cache[i].a, sDat, sAnimDb, name);
            return cache[i].a.tex.loaded ? &cache[i].a : NULL;
        }
    }
    return NULL;
#undef cache
}

static void draw_effects(const Combat* c, int side, const Camera* cam) {
    for (int i = 0; i < MAX_EFFECTS; i++) {
        const Effect* e = &c->effects[i];
        if (!e->active || e->side != side) continue;
        float x, y, k = e->t / 0.75f;
        to_screen(cam, e->x, e->y, &x, &y);
        const SheetAnim* ex = e->kind == FX_EXPLOSION ? explosion_anim(e->anim) : NULL;
        if (ex && ex->tex.loaded) {
            int frame = (int)(e->t / ex->time * ex->frames);
            if (frame < ex->frames)
                sheet_draw_frame_rotated(&ex->tex, ex->fw, ex->fh, ex->col + frame, ex->row, x, y, cam->zoom * 1.2f, 0.0f);
        } else if (e->kind == FX_EXPLOSION && sExplosion.tex.loaded) {
            int frame = (int)(k * sExplosion.frames);
            if (frame >= sExplosion.frames) frame = sExplosion.frames - 1;
            sheet_draw_frame_rotated(&sExplosion.tex, sExplosion.fw, sExplosion.fh, sExplosion.col + frame, sExplosion.row,
                                     x, y, cam->zoom * 1.2f, 0.0f);
        } else if (e->kind == FX_SHIELD) {
            float r = (10 + 30 * k) * cam->zoom;
            C2D_DrawEllipseSolid(x - r, y - r, 0, 2 * r, 2 * r, C2D_Color32(120, 200, 255, (u8)(160 * (1 - k))));
        } else if (e->kind == FX_MISS) {
            draw_text(x - 14, y - 10 - 12 * k, 0.45f, C2D_Color32(255, 255, 255, (u8)(255 * (1 - k))), "MISS");
        }
    }
}

static u32 hull_colour(int hp, int max) {
    return hp * 2 > max ? C2D_Color32(110, 220, 100, 255) : hp * 4 > max ? C2D_Color32(235, 200, 60, 255)
                                                                       : C2D_Color32(235, 70, 60, 255);
}

// FTL's Zoltan shield: a solid green bar, 5 points full, with the lost part dark.
#define ZOLTAN_MAX 5
static float draw_zoltan_bar(const Ship* s, float x, float cy, float h) {
    float w = 40;
    C2D_DrawRectSolid(x, cy - h / 2, 0, w, h, C2D_Color32(30, 60, 40, 255));
    int full = s->superShieldMax > 0 ? s->superShieldMax : ZOLTAN_MAX;
    C2D_DrawRectSolid(x, cy - h / 2, 0, w * (s->superShield > full ? full : s->superShield) / full, h, C2D_Color32(110, 235, 130, 255));
    return x + w;
}

// Shield bubbles centred on row cy (radius r, spacing step), a thin charge bar under them while a
// layer is recharging, then the Zoltan bar when there is one. Returns the right-hand end.
static float draw_shield_row(const Ship* s, float x, float cy, float r, float step) {
    int maxSh = combat_max_shields(s);
    for (int i = 0; i < maxSh; i++) {
        float cx = x + r + i * step;
        C2D_DrawCircleSolid(cx, cy, 0, r, C2D_Color32(40, 60, 90, 255));
        if (i < s->shieldLayers) C2D_DrawCircleSolid(cx, cy, 0, r - 1, C2D_Color32(90, 180, 255, 255));
    }
    float k = combat_shield_progress(s), ex = x + (maxSh > 0 ? (maxSh - 1) * step + 2 * r : 0);
    if (maxSh > 0 && k >= 0.0f) {
        float bw = ex - x;
        C2D_DrawRectSolid(x, cy + r + 1, 0, bw, 2, C2D_Color32(30, 40, 60, 255));
        C2D_DrawRectSolid(x, cy + r + 1, 0, bw * k, 2, C2D_Color32(90, 180, 255, 255));
    }
    if (s->superShield > 0) ex = draw_zoltan_bar(s, maxSh > 0 ? ex + 4 : x, cy, 2 * r - 2);
    return ex;
}

// Hull segments, shield bubbles and evasion for one ship, starting at (x, y), at most w wide.
static void draw_ship_status(const Ship* s, float x, float y, float w) {
    const u32 dim = C2D_Color32(150, 160, 175, 255);
    float seg = (w - (s->maxHull - 1)) / s->maxHull;
    if (seg > 4) seg = 4;
    u32 hc = hull_colour(s->hullHp, s->maxHull);
    for (int i = 0; i < s->maxHull; i++)
        C2D_DrawRectSolid(x + i * (seg + 1), y, 0, seg, 9, i < s->hullHp ? hc : C2D_Color32(45, 52, 66, 255));
    float ex = draw_shield_row(s, x, y + 18, 5, 13) + 4;
    char buf[24];
    snprintf(buf, sizeof(buf), "EVADE %d%%", combat_evasion(s));
    draw_text(ex, y + 12, 0.38f, dim, buf);
}

// One small bar per enemy weapon showing its charge (sensors level 3+).
static void draw_enemy_weapons(const Ship* e, float x, float y, float w) {
    if (!e || e->weaponCount == 0) return;
    float bw = (w - (e->weaponCount - 1) * 3) / e->weaponCount;
    if (bw > 30) bw = 30;
    for (int i = 0; i < e->weaponCount; i++) {
        const WeaponSlot* ws = &e->weapons[i];
        float k = ws->fullCharge > 0 ? ws->charge / ws->fullCharge : 0;
        float bx = x + i * (bw + 3);
        C2D_DrawRectSolid(bx, y, 0, bw, 4, C2D_Color32(10, 10, 10, 220));
        C2D_DrawRectSolid(bx, y, 0, bw * k, 4, !ws->powered ? C2D_Color32(90, 90, 100, 255)
                                                 : k >= 1.0f   ? C2D_Color32(235, 80, 60, 255)
                                                               : C2D_Color32(235, 170, 60, 255));
    }
}

enum { RES_FUEL, RES_MISSILES, RES_DRONES, RES_SCRAP };
static float draw_resource(float x, float y, int which, int value, bool warn);

// With one of our crew selected, their skills in the bottom-left corner of the top screen, as
// FTL's crew tooltip: each skill's icon in its level's colour and a bar of progress to the next.
static void draw_skill_panel(const CrewSet* cs) {
    int sel = -1;
    for (int i = 0; i < cs->count; i++)
        if (crew_is_selected(cs, i)) {
            if (sel >= 0) return;  // more than one
            sel = i;
        }
    if (sel < 0 || cs->crew[sel].foreign || cs->crew[sel].drone) return;
    const Crew* m = &cs->crew[sel];
    const float W = 186, H = 64, x = 2, y = TOP_H - H - 2;
    C2D_DrawRectSolid(x, y, 0, W, H, C2D_Color32(10, 14, 24, 215));
    C2D_DrawRectSolid(x, y, 0, W, 1, C2D_Color32(80, 90, 110, 255));
    draw_name(x + 5, y + 2, W - 10, C2D_Color32(235, 235, 235, 255), m->name);
    for (int k = 0; k < SKILL_COUNT; k++) {
        float cx = x + 4 + (k % 3) * 61, cy = y + 16 + (k / 3) * 23;
        int lvl = crew_skill_level(cs, m, k), t = crew_skill_threshold(cs, m, k);
        u32 col = skill_colour(lvl);
        draw_skill_icon(k, cx + 10, cy + 10, col);
        float prog = lvl >= 2 ? 1.0f : (m->xp[k] - lvl * t) / (float)t;
        if (prog < 0) prog = 0;
        C2D_DrawRectSolid(cx + 22, cy + 8, 0, 34, 5, C2D_Color32(44, 52, 66, 255));
        C2D_DrawRectSolid(cx + 22, cy + 8, 0, 34 * prog, 5, col);
    }
}

// The beacon's hazard at the top of the top screen (FTL's img/warnings/danger_*.png), with a
// countdown while a flare, pulse or battery shot is coming, and the flash when it hits.
static void draw_env_hud(const Combat* c) {
    if (c->envFlash > 0.0f) {
        float k = c->envFlash;
        u8 a = (u8)(k * k * 200);
        C2D_DrawRectSolid(0, 0, 0, TOP_W, TOP_H, c->envFlashKind == ENV_SUN ? C2D_Color32(255, 225, 160, a) : C2D_Color32(170, 200, 255, a));
    }
    float surge = combat_surge_warning(c);
    if (surge >= 0.0f) {
        const float sx = 134, sy = c->env == ENV_NONE ? 2 : 36;
        char line[24];
        snprintf(line, sizeof(line), "SURGE IN %d", (int)ceilf(surge));
        C2D_DrawRectSolid(sx, sy, 0, 120, 18, C2D_Color32(70, 16, 16, 220));
        draw_text_fit_left(sx + 4, sy + 2, 0.42f, 112, C2D_Color32(255, 120, 100, 255), line);
    }
    if (c->env == ENV_NONE) return;
    static const char* const files[] = {"", "danger_nebula", "danger_storm", "danger_asteroids", "danger_star", "danger_pulsar", "danger_pds"};
    static const char* const names[] = {"", "NEBULA", "ION STORM", "ASTEROIDS", "SOLAR FLARES", "PULSAR", "BATTERY"};
    static const char* const notes[] = {"", "Sensors down", "Reactor halved", "Incoming rocks", "Fires every ~30s",
                                        "Ion every ~15s", "Targeting you"};
    static Texture icons[7];
    static bool tried[7];
    int e = c->env < 7 ? c->env : 0;
    if (!tried[e]) {
        tried[e] = true;
        char path[48];
        snprintf(path, sizeof(path), "img/warnings/%s.png", files[e]);
        load_crop(&icons[e], path, 0, 0, 0, 0, false);
    }
    const float x = 134, y = 2, w = 120, h = 32;
    float warn = combat_env_warning(c);
    C2D_DrawRectSolid(x, y, 0, w, h, warn >= 0.0f ? C2D_Color32(70, 16, 16, 220) : C2D_Color32(10, 14, 24, 200));
    if (icons[e].loaded) C2D_DrawImageAt(icons[e].image, x + 1, y + 1, 0, NULL, 30.0f / icons[e].width, 30.0f / icons[e].height);
    draw_text_fit_left(x + 33, y + 1, 0.4f, w - 35, C2D_Color32(255, 210, 90, 255), names[e]);
    char line[32];
    snprintf(line, sizeof(line), "%s", e == ENV_PDS && !(c->envTargets & ENV_TARGET_PLAYER) ? "Targeting enemy" : notes[e]);
    if (warn >= 0.0f) {
        const char* what = c->env == ENV_SUN ? "FLARE" : c->env == ENV_PULSAR ? "PULSE" : "SHOT";
        snprintf(line, sizeof(line), "%s IN %d", what, (int)ceilf(warn));
    }
    if (line[0])
        draw_text_fit_left(x + 33, y + 15, 0.4f, w - 35, warn >= 0.0f ? C2D_Color32(255, 120, 100, 255) : C2D_Color32(170, 180, 195, 255), line);
}

static void draw_top_hud(const Ship* player, const Ship* enemy, const Combat* c, bool paused) {
    // Player: hull, shields, oxygen.
    C2D_DrawRectSolid(2, 2, 0, 128, sInRun ? 56 : 44, C2D_Color32(10, 14, 24, 190));
    draw_ship_status(player, 6, 5, 120);
    float o2 = life_ship_oxygen(player);
    char buf[48];
    snprintf(buf, sizeof(buf), "O2 %d%%", (int)(o2 + 0.5f));
    draw_text(6, 33, 0.4f, o2 < 25.0f ? C2D_Color32(255, 90, 90, 255) : C2D_Color32(140, 220, 255, 255), buf);
    if (sInRun) {
        // FTL's resource icons: fuel, missiles, drone parts, scrap.
        float x = 5;
        x = draw_resource(x, 44, RES_FUEL, sRun.fuel, sRun.fuel <= 3);
        x = draw_resource(x, 44, RES_MISSILES, player->missiles, player->missiles == 0);
        x = draw_resource(x, 44, RES_DRONES, player->droneParts, false);
        draw_resource(x, 44, RES_SCRAP, sRun.scrap, false);
    }

    // Enemy: name and status at the top of its panel.
    if (enemy) {
        // Name, hull, shields (with their charge bar), then enemy weapon charge with sensors 3+.
        bool weaponsShown = sensor_level(player) >= 3 && enemy->weaponCount > 0;
        float panelH = weaponsShown ? 49 : 44;
        C2D_DrawRectSolid(ENEMY_X + 1, 0, 0, ENEMY_W - 1, panelH, C2D_Color32(12, 15, 28, 220));
        draw_text(ENEMY_X + 5, 2, 0.4f, C2D_Color32(235, 120, 110, 255), text_get(enemy->bp->classId, enemy->bp->name));
        draw_ship_status(enemy, ENEMY_X + 5, 16, ENEMY_W - 10);
        if (weaponsShown) draw_enemy_weapons(enemy, ENEMY_X + 5, 43, ENEMY_W - 10);
        if (sEscapeLeft >= 0.0f && c->result == COMBAT_FIGHTING) {
            char esc[32];
            snprintf(esc, sizeof(esc), "FTL: JUMPING IN %ds", (int)ceilf(sEscapeLeft));
            C2D_DrawRectSolid(ENEMY_X + 1, panelH + 2, 0, ENEMY_W - 1, 13, C2D_Color32(90, 20, 20, 220));
            draw_text(ENEMY_X + 5, panelH + 2, 0.4f, C2D_Color32(255, 200, 120, 255), esc);
        }
    }

    const char* banner = paused ? "PAUSED - tap RESUME or press A" : NULL;
    if (c->result == COMBAT_WON) banner = c->crewKilled ? "ENEMY CREW KILLED" : "ENEMY DESTROYED";
    if (c->result == COMBAT_LOST) {
        bool crewLost = player->hullHp > 0;  // everyone dead, ship intact
        banner = sInRun ? (crewLost ? "ALL CREW LOST - A: main menu" : "YOUR SHIP WAS DESTROYED - A: main menu")
                        : (crewLost ? "ALL CREW LOST - press A" : "YOUR SHIP WAS DESTROYED - press A");
    }
    if (sInRun && sRun.over && sRun.won) banner = "FLAGSHIP DESTROYED - VICTORY! A: main menu";
    if (sInRun && sRun.over && sRun.baseLost) banner = "THE FEDERATION BASE HAS FALLEN - A: main menu";
    if (banner) {
        C2D_DrawRectSolid(0, TOP_H / 2 - 14, 0, TOP_W, 26, C2D_Color32(10, 14, 24, 210));
        draw_text_centred(0, TOP_H / 2 - 11, TOP_W, 0.6f, C2D_Color32(255, 230, 120, 255), banner);
    }
}

// ---- Arms tab ----------------------------------------------------------------------

#define ARMS_MAP_TOP (TAB_H + 14)
#define AUTO_X (BOT_W - 62)  // autofire toggle
#define AUTO_W 58
#define WCARD_H 54                              // weapon card height on the arms tab
#define CARDS_TOP (BOT_H - WCARD_H - 3)
#define ARMS_MAP_H (CARDS_TOP - 4 - ARMS_MAP_TOP)
#define WCARD_GAP 3
#define POWER_BTN_H 22

static float wcard_w(void) { return (BOT_W - 8 - (MAX_WEAPON_SLOTS - 1) * WCARD_GAP) / (float)MAX_WEAPON_SLOTS; }
static float wcard_x(int i) { return 4 + i * (wcard_w() + WCARD_GAP); }

static void draw_reticle(float x, float y, u32 col, int number) {
    C2D_DrawRectSolid(x - 7, y - 1, 0, 5, 2, col);
    C2D_DrawRectSolid(x + 2, y - 1, 0, 5, 2, col);
    C2D_DrawRectSolid(x - 1, y - 7, 0, 2, 5, col);
    C2D_DrawRectSolid(x - 1, y + 2, 0, 2, 5, col);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", number);
    draw_text(x + 4, y + 2, 0.38f, col, buf);
}

// Tints red every room a beam path from (x, y) at `angle`, `len` long, would cross (enemy coordinates).
static void draw_beam_rooms(const Ship* e, const Camera* cam, float x, float y, float angle, float len, u8 alpha) {
    unsigned long long seen = 0;
    for (float d = 0; d <= len; d += 4.0f) {
        int r = ship_room_at(e, (int)floorf((x + cosf(angle) * d) / TILE) - e->xOffset,
                             (int)floorf((y + sinf(angle) * d) / TILE) - e->yOffset);
        if (r < 0 || (seen & (1ull << r))) continue;
        seen |= 1ull << r;
        const Room* rm = &e->rooms[r];
        draw_rect(cam, (e->xOffset + rm->x) * TILE, (e->yOffset + rm->y) * TILE, rm->w * TILE, rm->h * TILE,
                  C2D_Color32(230, 40, 30, alpha));
    }
}

// Card rectangles: one tall row of weapons, or weapons over drones when the ship has drones.
static void card_rect(const Ship* player, int row, int i, float* x, float* y, float* w, float* h) {
    (void)player;
    (void)row;
    *h = WCARD_H;
    *x = wcard_x(i);
    *w = wcard_w();
    *y = CARDS_TOP;
}

// Power button area of a card (the bottom strip on tall cards, the bottom-right pill on short ones).
static void card_button(float x, float y, float w, float h, float* bx, float* by, float* bw, float* bh) {
    if (h >= 60) {
        *bx = x + 3;
        *by = y + h - POWER_BTN_H - 3;
        *bw = w - 6;
        *bh = POWER_BTN_H;
    } else {
        *bw = 34;
        *bh = 14;
        *bx = x + w - *bw - 3;
        *by = y + h - *bh - 3;
    }
}

// One weapon or drone card. charge < 0 hides the charge bar.
static void draw_card(float x, float y, float w, float h, bool selected, const char* name, bool on, float charge,
                      bool ready, const char* status, int pips, const char* extra, bool extraLow) {
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(145, 155, 170, 255);
    bool tall = h >= 60;
    C2D_DrawRectSolid(x, y, 0, w, h, selected ? C2D_Color32(230, 200, 60, 255) : C2D_Color32(52, 62, 80, 255));
    C2D_DrawRectSolid(x + 1, y + 1, 0, w - 2, h - 2, C2D_Color32(22, 28, 40, 255));
    // Short cards: the power cost as small bars in the top-right corner (tall ones show it below).
    if (!tall && pips > 0) {
        for (int p = 0; p < pips; p++)
            C2D_DrawRectSolid(x + w - 4 - (pips - p) * 5, y + 3, 0, 3, 9, on ? C2D_Color32(110, 220, 100, 255) : C2D_Color32(120, 130, 145, 255));
    }
    draw_name(x + 4, y + (tall ? 3 : 2), w - 8 - (!tall && pips > 0 ? pips * 5 + 2 : 0), on ? white : dim, name);
    float barY = y + (tall ? 20 : 17), barH = tall ? 7 : 5;
    if (charge >= 0.0f) {
        C2D_DrawRectSolid(x + 4, barY, 0, w - 8, barH, C2D_Color32(44, 52, 66, 255));
        C2D_DrawRectSolid(x + 4, barY, 0, (w - 8) * (charge > 1 ? 1 : charge), barH,
                          ready ? C2D_Color32(110, 220, 100, 255) : C2D_Color32(90, 170, 240, 255));
    }
    draw_text_fit_left(x + 4, y + (tall ? 30 : (charge >= 0 ? 23 : 17)), tall ? 0.38f : 0.36f, w - 8,
                       ready ? C2D_Color32(140, 235, 130, 255) : dim, status);
    float bx, by, bw, bh;
    card_button(x, y, w, h, &bx, &by, &bw, &bh);
    C2D_DrawRectSolid(bx, by, 0, bw, bh, on ? C2D_Color32(40, 90, 50, 255) : C2D_Color32(44, 52, 66, 255));
    if (tall)
        for (int p = 0; p < pips; p++)
            C2D_DrawRectSolid(bx + 4 + p * 9, by + 5, 0, 6, 12, on ? C2D_Color32(110, 220, 100, 255) : C2D_Color32(80, 90, 105, 255));
    draw_text(bx + bw - (tall ? 25 : 27), by + (tall ? 4 : 1), tall ? 0.42f : 0.38f, on ? white : dim, on ? "ON" : "OFF");
    if (extra[0])
        draw_text(tall ? bx + 6 + pips * 9 : x + 4, tall ? by + 5 : by + 1, tall ? 0.38f : 0.36f,
                  extraLow ? C2D_Color32(235, 90, 70, 255) : white, extra);
}

// Our weapons' targets on the enemy ship: FTL's crosshairs on targeted rooms, aiming lines for
// beams. Markers are kept inside (x0, y0)-(x1, y1). Nothing once the fight is over.
static void draw_target_markers(const Ship* player, const Ship* enemy, const Combat* c, const Camera* emap, int selected,
                                float x0, float y0, float x1, float y1) {
    if (!enemy || c->result != COMBAT_FIGHTING || enemy->hullHp <= 0) return;
    for (int i = 0; i < player->weaponCount; i++) {
        const WeaponSlot* ws = &player->weapons[i];
        if (ws->beamAimed && ws->target >= 0) {
            draw_beam_rooms(enemy, emap, ws->beamX, ws->beamY, ws->beamAngle, ws->def->length, i == selected ? 110 : 80);
            float bx0, by0, bx1, by1;
            to_screen(emap, ws->beamX, ws->beamY, &bx0, &by0);
            to_screen(emap, ws->beamX + cosf(ws->beamAngle) * ws->def->length,
                      ws->beamY + sinf(ws->beamAngle) * ws->def->length, &bx1, &by1);
            // FTL's colours: red for a one-off target, yellow with autofire.
            u32 col = c->autofire ? C2D_Color32(255, 220, 60, i == selected ? 255 : 210)
                                  : C2D_Color32(255, 80, 60, i == selected ? 255 : 210);
            C2D_DrawLine(bx0, by0, col, bx1, by1, col, i == selected ? 2.5f : 2.0f, 0);
            C2D_DrawCircleSolid(bx0, by0, 0, 3, col);
            char num[4];
            snprintf(num, sizeof(num), "%d", i + 1);
            draw_text(bx0 + 4, by0 + 2, 0.38f, col, num);
            continue;
        }
        int r = ws->target;
        if (r < 0 || r >= enemy->roomCount) continue;
        const Room* rm = &enemy->rooms[r];
        float x, y;
        to_screen(emap, (enemy->xOffset + rm->x + rm->w * 0.5f) * TILE, (enemy->yOffset + rm->y + rm->h * 0.5f) * TILE,
                  &x, &y);
        // Weapons aimed at the same room spread out so their markers don't hide each other.
        int same = 0, shared = 0;
        for (int j = 0; j < player->weaponCount; j++) {
            const WeaponSlot* o = &player->weapons[j];
            if (o->beamAimed || o->target != r) continue;
            if (j < i) same++;
            shared++;
        }
        if (shared > 1) x += ((same & 1) - 0.5f) * 12;           // side by side
        if (shared > 2) y += ((same >> 1) - 0.5f) * 12;          // then a second row
        // Keep the whole marker inside the map, clear of the header and the weapon cards.
        if (x < x0 + 10) x = x0 + 10;
        if (x > x1 - 16) x = x1 - 16;
        if (y < y0 + 9) y = y0 + 9;
        if (y > y1 - 14) y = y1 - 14;
        // FTL's target markers (img/mouse/mouse_crosshairs2_N red, crosshairs3_N yellow for
        // autofire), drawn 1:1 with the reticle's centre on the room. The art sits in the
        // bottom of a 32x32 image: reticle at x 1-17, y 12-29, the weapon number to its right.
        static Texture cross[2][4];
        static bool crossTried;
        if (!crossTried) {
            crossTried = true;
            for (int k = 0; k < 8; k++) {
                char path[48];
                snprintf(path, sizeof(path), "img/mouse/mouse_crosshairs%d_%d.png", k / 4 + 2, k % 4 + 1);
                load_crop(&cross[k / 4][k % 4], path, 0, 12, 24, 18, false);
            }
        }
        const Texture* ct = &cross[c->autofire ? 1 : 0][i & 3];
        if (ct->loaded)
            C2D_DrawImageAt(ct->image, floorf(x - 9), floorf(y - 9), 0.0f, NULL, 1.0f, 1.0f);
        else
            draw_reticle(x, y, c->autofire ? C2D_Color32(255, 220, 60, 255) : C2D_Color32(255, 90, 70, 255), i + 1);
    }
}

static void draw_arms_tab(const Ship* player, const Ship* enemy, const CrewSet* enemyCrew, const Combat* c,
                          const Camera* emap, int selected) {
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(145, 155, 170, 255);

    if (enemy) {
        draw_ship(enemy, emap);
        draw_crew(enemy, enemyCrew, emap);
        draw_effects(c, SIDE_ENEMY, emap);
        draw_beams(c, SIDE_ENEMY, emap);
        draw_drones(c, SIDE_ENEMY, emap);
        if (sensor_level(player) >= 3) draw_enemy_weapons(enemy, 4, CARDS_TOP - 10, 120);
        const Camera* cams[2] = {NULL, emap};
        draw_projectiles(c, cams, -30);
        draw_target_markers(player, enemy, c, emap, selected, 0, ARMS_MAP_TOP, BOT_W, CARDS_TOP);
    }

    // Header over the map: enemy name, hull, shields and evasion, then the autofire toggle.
    char buf[48];
    C2D_DrawRectSolid(0, TAB_H, 0, BOT_W, ARMS_MAP_TOP - TAB_H, C2D_Color32(8, 10, 20, 255));
    if (enemy) {
        draw_name(4, TAB_H + 1, 70, C2D_Color32(235, 120, 110, 255), text_get(enemy->bp->classId, enemy->bp->name));
        float hx = 78, hw = 70;
        float seg = (hw - (enemy->maxHull - 1)) / enemy->maxHull;
        if (seg > 4) seg = 4;
        u32 hc = hull_colour(enemy->hullHp, enemy->maxHull);
        for (int i = 0; i < enemy->maxHull; i++)
            C2D_DrawRectSolid(hx + i * (seg + 1), TAB_H + 3, 0, seg, 8, i < enemy->hullHp ? hc : C2D_Color32(45, 52, 66, 255));
        float px = draw_shield_row(enemy, hx + enemy->maxHull * (seg + 1) + 6, TAB_H + 6, 4, 11);
        snprintf(buf, sizeof(buf), "%d%%", combat_evasion(enemy));
        draw_text(px + 4, TAB_H + 1, 0.38f, dim, buf);
    }
    C2D_DrawRectSolid(AUTO_X, TAB_H + 1, 0, AUTO_W, 12, c->autofire ? C2D_Color32(40, 90, 50, 255) : C2D_Color32(40, 46, 60, 255));
    draw_text(AUTO_X + 5, TAB_H + 1, 0.38f, c->autofire ? white : dim, c->autofire ? "AUTO ON" : "AUTO OFF");

    // Weapon cards, plus a row of drone cards when the ship has drones.
    C2D_DrawRectSolid(0, CARDS_TOP - 3, 0, BOT_W, BOT_H - CARDS_TOP + 3, C2D_Color32(8, 10, 20, 255));
    for (int i = 0; i < MAX_WEAPON_SLOTS; i++) {
        float x, y, w, h;
        card_rect(player, 0, i, &x, &y, &w, &h);
        if (i >= player->weaponCount) {
            C2D_DrawRectSolid(x, y, 0, w, h, C2D_Color32(18, 22, 32, 255));
            continue;
        }
        const WeaponSlot* ws = &player->weapons[i];
        const WeaponDef* d = ws->def;
        float k = ws->fullCharge > 0 ? ws->charge / ws->fullCharge : 0;
        bool ready = ws->powered && k >= 1.0f;
        const char* status = !weapon_supported(d) ? "NOT YET" : !ws->powered ? "NO POWER"
                             : (d->missiles > 0 && player->missiles < d->missiles) ? "NO AMMO"
                             : ready ? (ws->target >= 0 ? "FIRING" : "READY") : "CHARGING";
        char ammo[12] = "";
        if (d->missiles > 0) snprintf(ammo, sizeof(ammo), "x%d", player->missiles);
        draw_card(x, y, w, h, i == selected, d->shortName, ws->powered, k, ready, status, d->power, ammo,
                  d->missiles > 0 && player->missiles < d->missiles);
    }
}

// Returns the newly selected weapon.
static int arms_tab_touch(Ship* player, const Ship* enemy, Combat* c, const Camera* emap, int selected, int px, int py) {
    if (py < ARMS_MAP_TOP && px >= AUTO_X) {
        c->autofire = !c->autofire;
        sound_play(c->autofire ? "autofireOn" : "autofireOff");
        return selected;
    }
    if (py >= CARDS_TOP) {
        for (int i = 0; i < player->weaponCount; i++) {
            float x, y, w, h, bx, by, bw, bh;
            card_rect(player, 0, i, &x, &y, &w, &h);
            if (px < x || px >= x + w || py < y || py >= y + h) continue;
            card_button(x, y, w, h, &bx, &by, &bw, &bh);
            bool wasOn = player->weapons[i].powered;
            if (py >= by && px >= bx) {
                bool ok = combat_toggle_weapon(player, i);
                sound_play(!ok ? "powerUpFail" : wasOn ? "powerDownSystem" : "powerUpSystem");
                return selected == i && wasOn ? -1 : selected;
            }
            if (!wasOn) {
                bool ok = combat_toggle_weapon(player, i);
                sound_play(ok ? "powerUpSystem" : "powerUpFail");
                if (!ok) return selected;
            }
            sound_play("hoverBeep");
            return selected == i ? -1 : i;
        }
        return selected;
    }
    if (!enemy || selected < 0 || py < ARMS_MAP_TOP) return selected;
    float sxp, syp;
    to_ship(emap, px, py, &sxp, &syp);
    int room = ship_room_at(enemy, (int)floorf(sxp / TILE - enemy->xOffset), (int)floorf(syp / TILE - enemy->yOffset));
    if (room < 0) return selected;
    // Tapping the current target again clears it.
    combat_set_target(c, selected, player->weapons[selected].target == room ? -1 : room);
    sound_play("hoverBeep");
    return -1;
}

// ---- Systems tab -------------------------------------------------------------------
// Drones in the top row, special systems (cloaking, battery, clone bay, ...) below.

#define SYS_ROW1 (TAB_H + 16)
#define SYS_ROW2 (TAB_H + 102)
#define SYS_CARD_H 64

// Special-system cards in display order; returns how many, with their system indices.
static int special_systems(const Ship* s, int out[MAX_WEAPON_SLOTS]) {
    static const int order[] = {SYS_ARTILLERY, SYS_CLOAKING, SYS_BATTERY, SYS_CLONEBAY, SYS_TELEPORTER, SYS_HACKING, SYS_MIND};
    int n = 0;
    for (size_t k = 0; k < sizeof(order) / sizeof(order[0]) && n < MAX_WEAPON_SLOTS; k++) {
        int si = ship_find_system(s, order[k]);
        if (si >= 0) out[n++] = si;
    }
    return n;
}

static const char* system_title(int type) {
    switch (type) {
        case SYS_CLOAKING: return "Cloaking";
        case SYS_BATTERY: return "Backup Battery";
        case SYS_CLONEBAY: return "Clone Bay";
        case SYS_TELEPORTER: return "Teleporter";
        case SYS_HACKING: return "Hacking";
        case SYS_MIND: return "Mind Control";
        case SYS_ARTILLERY: return "Artillery";
        default: return "?";
    }
}

// The next tap on the enemy map picks a room for one of these.
enum { PICK_NONE, PICK_TELEPORT, PICK_HACK, PICK_MIND };
static int pickMode;

static void draw_sys_tab(const Ship* player, const CrewSet* crew, const Combat* c) {
    const u32 dim = C2D_Color32(145, 155, 170, 255), head = C2D_Color32(110, 200, 120, 255);
    char buf[32];
    draw_text(4, TAB_H + 2, 0.4f, head, "DRONES");
    if (ship_find_system(player, SYS_DRONES) >= 0) {
        (void)buf;
        draw_resource(BOT_W - 50, TAB_H + 2, RES_DRONES, player->droneParts, player->droneParts == 0);
    }
    for (int i = 0; i < MAX_WEAPON_SLOTS; i++) {
        float x = wcard_x(i), w = wcard_w();
        if (i >= player->droneCount) {
            C2D_DrawRectSolid(x, SYS_ROW1, 0, w, SYS_CARD_H, C2D_Color32(18, 22, 32, 255));
            continue;
        }
        const DroneSlot* ds = &player->drones[i];
        char status[16];
        if (!drone_supported(ds->def))
            snprintf(status, sizeof(status), "NOT YET");
        else if (ds->powered)
            snprintf(status, sizeof(status), "ACTIVE");
        else if (ds->redeploy > 0.0f)
            snprintf(status, sizeof(status), "WAIT %ds", (int)ceilf(ds->redeploy));
        else if (!ds->deployed && player->droneParts <= 0)
            snprintf(status, sizeof(status), "NO PARTS");
        else
            snprintf(status, sizeof(status), ds->deployed ? "IDLE" : "DOCKED");
        draw_card(x, SYS_ROW1, w, SYS_CARD_H, false, ds->def->shortName, ds->powered, -1.0f, ds->powered, status,
                  ds->def->power, "", false);
    }
    if (player->droneCount == 0) draw_text(8, SYS_ROW1 + 24, 0.4f, dim, "No drones.");

    draw_text(4, SYS_ROW2 - 14, 0.4f, head, "SYSTEMS");
    int sys[MAX_WEAPON_SLOTS];
    int n = special_systems(player, sys);
    for (int k = 0; k < MAX_WEAPON_SLOTS; k++) {
        float x = wcard_x(k), w = wcard_w();
        if (k >= n) {
            C2D_DrawRectSolid(x, SYS_ROW2, 0, w, SYS_CARD_H, C2D_Color32(18, 22, 32, 255));
            continue;
        }
        const ShipSystem* y = &player->systems[sys[k]];
        int type = y->def->type;
        char status[24] = "NOT YET";
        float bar = -1.0f;
        bool on = false, ready = false;
        if (type == SYS_CLOAKING) {
            if (player->cloakTime > 0.0f) {
                snprintf(status, sizeof(status), "CLOAKED %ds", (int)ceilf(player->cloakTime));
                on = ready = true;
                bar = player->cloakTime / (y->power > 0 ? y->power * 5.0f : 5.0f);
            } else if (y->ionTimer > 0.0f) {
                snprintf(status, sizeof(status), "RECHARGE %ds", (int)ceilf(y->ionTimer));
            } else {
                snprintf(status, sizeof(status), y->power > 0 ? "READY" : "NO POWER");
                ready = y->power > 0;
            }
        } else if (type == SYS_BATTERY) {
            if (player->batteryTime > 0.0f) {
                snprintf(status, sizeof(status), "+%d POWER %ds", player->batteryBonus, (int)ceilf(player->batteryTime));
                on = ready = true;
                bar = player->batteryTime / 30.0f;
            } else if (y->ionTimer > 0.0f) {
                snprintf(status, sizeof(status), "RECHARGE %ds", (int)ceilf(y->ionTimer));
            } else {
                snprintf(status, sizeof(status), ship_system_capacity(player, sys[k]) > 0 ? "READY" : "BROKEN");
                ready = ship_system_capacity(player, sys[k]) > 0;
            }
        } else if (type == SYS_TELEPORTER) {
            int st = combat_teleporter_state(c, SIDE_PLAYER);
            int away = c->crew[SIDE_ENEMY] ? crew_foreign_count(c->crew[SIDE_ENEMY]) : 0;  // no enemy: nobody aboard
            if (st == 1)
                snprintf(status, sizeof(status), "RECHARGE %ds", (int)ceilf(y->ionTimer));
            else if (st == 2)
                snprintf(status, sizeof(status), "NO POWER");
            else
                snprintf(status, sizeof(status), pickMode == PICK_TELEPORT ? "PICK A ROOM" : "TAP TO SEND");
            ready = st == 0;
            on = away > 0;
            if (away > 0) snprintf(buf, sizeof(buf), "%d aboard", away);
        } else if (type == SYS_ARTILLERY) {
            int ak = ship_artillery_index(player, sys[k]);
            float full = ship_artillery_time(player, ak), charge = ak >= 0 ? player->artillery[ak].charge : 0.0f;
            on = full > 0.0f;
            bar = on ? charge / full : 0.0f;
            if (on)
                snprintf(status, sizeof(status), "FIRES IN %ds", (int)ceilf(full - charge));
            else
                snprintf(status, sizeof(status), "NO POWER");
        } else if (type == SYS_HACKING) {
            static const char* const hs[] = {"", "TAP: PICK SYSTEM", "LAUNCHING", "TAP TO HACK", "HACKING", "RECHARGE", "NO POWER"};
            int st = combat_hack_state(c, SIDE_PLAYER);
            snprintf(status, sizeof(status), "%s", pickMode == PICK_HACK ? "PICK A SYSTEM" : hs[st]);
            if (st == HACK_PULSING && c->ship[SIDE_ENEMY]) {
                snprintf(status, sizeof(status), "HACKING %ds", (int)ceilf(c->ship[SIDE_ENEMY]->hackPulse));
                bar = c->ship[SIDE_ENEMY]->hackPulse / (1.0f + 3.0f * ship_system_power(player, sys[k]));
            }
            if (st == HACK_RECHARGING) snprintf(status, sizeof(status), "RECHARGE %ds", (int)ceilf(y->ionTimer));
            on = st == HACK_PULSING;
            ready = st == HACK_READY || st == HACK_NO_DRONE;
        } else if (type == SYS_MIND) {
            int st = combat_mind_state(c, SIDE_PLAYER);
            snprintf(status, sizeof(status), pickMode == PICK_MIND ? "PICK A ROOM" : st == MIND_READY ? "TAP: PICK ROOM" : st == MIND_NO_POWER ? "NO POWER" : "RECHARGE");
            if (st == MIND_RECHARGING) snprintf(status, sizeof(status), "RECHARGE %ds", (int)ceilf(y->ionTimer));
            ready = st == MIND_READY;
        } else if (type == SYS_CLONEBAY) {
            if (crew->cloneCount > 0) {
                snprintf(status, sizeof(status), y->power > 0 ? "CLONING (%d)" : "NO POWER (%d)", crew->cloneCount);
                bar = crew->cloneProgress;
                on = y->power > 0;
            } else {
                snprintf(status, sizeof(status), y->power > 0 ? "STANDING BY" : "NO POWER");
                on = y->power > 0;
            }
        }
        bool picking = (type == SYS_TELEPORTER && pickMode == PICK_TELEPORT) || (type == SYS_HACKING && pickMode == PICK_HACK) ||
                       (type == SYS_MIND && pickMode == PICK_MIND);
        draw_card(x, SYS_ROW2, w, SYS_CARD_H, picking, system_title(type), on, bar, ready,
                  status, 0, type == SYS_TELEPORTER && on ? buf : "", false);
        if (type == SYS_TELEPORTER) {
            // Its button brings boarders home instead of switching power.
            float bx, by, bw, bh;
            card_button(x, SYS_ROW2, w, SYS_CARD_H, &bx, &by, &bw, &bh);
            C2D_DrawRectSolid(bx, by, 0, bw, bh, on ? C2D_Color32(40, 70, 110, 255) : C2D_Color32(44, 52, 66, 255));
            draw_text(bx + 4, by + 4, 0.4f, on ? C2D_Color32(235, 235, 235, 255) : dim, "RETURN");
        }
    }
    if (n == 0) draw_text(8, SYS_ROW2 + 24, 0.4f, dim, "No special systems.");
    (void)c;
}

static void sys_tab_touch(Ship* player, Combat* c, int px, int py) {
    for (int i = 0; i < player->droneCount; i++) {
        float x = wcard_x(i), w = wcard_w();
        if (px < x || px >= x + w || py < SYS_ROW1 || py >= SYS_ROW1 + SYS_CARD_H) continue;
        bool wasOn = player->drones[i].powered;
        bool ok = combat_toggle_drone(c, SIDE_PLAYER, i);
        sound_play(!ok ? "powerUpFail" : wasOn ? "powerDownSystem" : "powerUpSystem");
        return;
    }
    int sys[MAX_WEAPON_SLOTS];
    int n = special_systems(player, sys);
    for (int k = 0; k < n; k++) {
        float x = wcard_x(k), w = wcard_w();
        if (px < x || px >= x + w || py < SYS_ROW2 || py >= SYS_ROW2 + SYS_CARD_H) continue;
        int type = player->systems[sys[k]].def->type;
        // Teleporting, hacking and mind control need an enemy ship to act on.
        if (!c->ship[SIDE_ENEMY] && (type == SYS_TELEPORTER || type == SYS_HACKING || type == SYS_MIND)) {
            sound_play("powerUpFail");
            return;
        }
        if (type == SYS_TELEPORTER) {
            float bx, by, bw, bh;
            card_button(x, SYS_ROW2, w, SYS_CARD_H, &bx, &by, &bw, &bh);
            if (py >= by) {
                sound_play(combat_teleport_return(c, SIDE_PLAYER) ? "powerUpSystem" : "powerUpFail");
            } else if (combat_teleporter_state(c, SIDE_PLAYER) == 0) {
                pickMode = PICK_TELEPORT;  // the main loop switches to the arms tab
                sound_play("hoverBeep");
            } else {
                sound_play("powerUpFail");
            }
            return;
        }
        if (type == SYS_HACKING) {
            int st = combat_hack_state(c, SIDE_PLAYER);
            if (st == HACK_NO_DRONE) {
                pickMode = PICK_HACK;
                sound_play("hoverBeep");
            } else {
                sound_play(combat_hack_pulse(c, SIDE_PLAYER) ? "powerUpSystem" : "powerUpFail");
            }
            return;
        }
        if (type == SYS_MIND) {
            if (combat_mind_state(c, SIDE_PLAYER) == MIND_READY) {
                pickMode = PICK_MIND;
                sound_play("hoverBeep");
            } else {
                sound_play("powerUpFail");
            }
            return;
        }
        bool ok = type == SYS_CLOAKING ? combat_cloak(c, SIDE_PLAYER) : type == SYS_BATTERY ? combat_battery(c, SIDE_PLAYER) : false;
        if (type == SYS_CLOAKING || type == SYS_BATTERY) sound_play(ok ? "powerUpSystem" : "powerUpFail");
        return;
    }
}

// ---- Test hooks ------------------------------------------------------------------------
// Scenario tests (make TEST=1; see testhooks.h). In the normal build none of this exists.

#ifdef FTL_TEST
static float sTestClock, sTestHold;  // seconds since start; screenshot hold left
static float sTestFrameTime = 1 / 60.0f;  // seconds a frame, averaged over about a second
static bool sTestQuit;
static u32 sTestKeys;                 // buttons a scenario presses next frame
static bool sTestTap, sTestTapNow;    // a scenario's touch: pending, and this frame's
static u16 sTestTapX, sTestTapY;
#else
#define sTestHold 0.0f
#endif

// The bottom screen's touch this frame: the real one, or a scenario's tap.
static void read_touch(touchPosition* t) {
#ifdef FTL_TEST
    if (sTestTapNow) {
        t->px = sTestTapX;
        t->py = sTestTapY;
        return;
    }
#endif
    hidTouchRead(t);
}

static int find_blueprint(const ShipBlueprint* bps, int count, const char* name) {
    if (!name[0]) return -1;
    int n = atoi(name);
    if (n >= 1 && n <= count) return n - 1;
    for (int i = 0; i < count; i++)
        if (!strcmp(bps[i].name, name)) return i;
    return -1;
}

// ---- Debug menu (SELECT) -------------------------------------------------------------

enum {
    DBG_DESTROY, DBG_KILL_CREW, DBG_FIRE, DBG_FIX, DBG_ENEMY_BOARDS, DBG_WE_BOARD,
    DBG_RESOURCES, DBG_HOLD_FIRE, DBG_COOLDOWNS, DBG_BREAK_ENEMY, DBG_SENSORS, DBG_NEW_RUN, DBG_COUNT,
    DBG_START = 100, DBG_SHIP_PREV, DBG_SHIP_NEXT, DBG_ENEMY_PREV, DBG_ENEMY_NEXT
};
#define DBG_ROW1 (TAB_H + 4)
#define DBG_ROW2 (TAB_H + 24)
#define DBG_START_Y (TAB_H + 45)
#define DBG_GRID_Y (TAB_H + 68)
#define DBG_BTN_H 20

static void ftl_button(float x, float y, float w, float h, bool lit, const char* label);

static void dbg_button(float x, float y, float w, float h, bool on, const char* label) { ftl_button(x, y, w, h, on, label); }

// ---- Run screens: event dialogs, the sector map, stores ---------------------------------------

static void gpu_idle(void);

static void load_tex(Texture* t, const char* path, int minW, int minH) {
    uint32_t size;
    uint8_t* png = ftldat_read(sDat, path, &size);
    if (png) texture_load_png_fit(t, png, size, minW, minH);
    free(png);
}

// Resource icons, cut from FTL's status boxes (img/statusUI/top_*_on.png: the icon sits at the left
// of a frame for the number) and img/ui_icons/icon_scrap.png. Each 24x24 symbol is averaged down to
// exactly 12x12 once and drawn pixel for pixel: letting the GPU shrink it by an odd amount skips
// most of its pixels and turns it to mush.
#define RES_ICON 12
static Texture sResTex[4];
static const int RES_CROP[4][2] = {{10, 8}, {10, 8}, {8, 8}, {6, 5}};  // top-left of each 24x24 symbol

static void res_icon_load(int which) {
    static const char* const files[4] = {"img/statusUI/top_fuel_on.png", "img/statusUI/top_missiles_on.png",
                                         "img/statusUI/top_drones_on.png", "img/ui_icons/icon_scrap.png"};
    uint32_t size;
    uint8_t* png = ftldat_read(sDat, files[which], &size);
    uint8_t* rgba = NULL;
    int w, h;
    if (png && texture_decode_png(png, size, 0, 0, &rgba, &w, &h)) {
        // Symbol coverage at full size. The status icons are pale tiles with the symbol cut out
        // as holes, which vanish when small, so they're inverted: transparent pixels that can't be
        // reached from outside the tile are the symbol. The scrap icon already is a symbol.
        enum { N = RES_ICON * 2 };
        const int cx = RES_CROP[which][0], cy = RES_CROP[which][1];
        uint8_t alpha[N][N];
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                int sx = cx + x, sy = cy + y;
                alpha[y][x] = sx < w && sy < h ? rgba[((size_t)sy * w + sx) * 4 + 3] : 0;
            }
        uint8_t cover[N][N];
        if (which == RES_SCRAP) {
            memcpy(cover, alpha, sizeof(cover));
        } else {
            bool outside[N][N] = {{false}};
            int stack[N * N][2], top = 0;
            for (int i = 0; i < N; i++) {  // seed from the border
                int seeds[4][2] = {{i, 0}, {i, N - 1}, {0, i}, {N - 1, i}};
                for (int k = 0; k < 4; k++) {
                    int x = seeds[k][0], y = seeds[k][1];
                    if (alpha[y][x] < 128 && !outside[y][x]) {
                        outside[y][x] = true;
                        stack[top][0] = x;
                        stack[top++][1] = y;
                    }
                }
            }
            while (top > 0) {
                top--;
                int x = stack[top][0], y = stack[top][1];
                static const int D[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4; k++) {
                    int nx = x + D[k][0], ny = y + D[k][1];
                    if (nx < 0 || ny < 0 || nx >= N || ny >= N || outside[ny][nx] || alpha[ny][nx] >= 128) continue;
                    outside[ny][nx] = true;
                    stack[top][0] = nx;
                    stack[top++][1] = ny;
                }
            }
            for (int y = 0; y < N; y++)
                for (int x = 0; x < N; x++) cover[y][x] = outside[y][x] ? 0 : (uint8_t)(255 - alpha[y][x]);
        }
        // Halve, keeping the strongest pixel of each 2x2 so one-pixel strokes stay solid.
        uint8_t out[RES_ICON * RES_ICON * 4];
        for (int y = 0; y < RES_ICON; y++)
            for (int x = 0; x < RES_ICON; x++) {
                uint8_t m = 0;
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++)
                        if (cover[y * 2 + j][x * 2 + i] > m) m = cover[y * 2 + j][x * 2 + i];
                uint8_t* o = out + (y * RES_ICON + x) * 4;
                o[0] = o[1] = o[2] = 255;
                o[3] = m > 96 ? 255 : 0;
            }
        if (texture_from_rgba(&sResTex[which], out, RES_ICON, RES_ICON))
            C3D_TexSetFilter(sResTex[which].image.tex, GPU_NEAREST, GPU_NEAREST);
    }
    free(rgba);
    free(png);
}

static float draw_resource(float x, float y, int which, int value, bool warn) {
    static bool tried[4];
    if (!tried[which]) {
        tried[which] = true;
        res_icon_load(which);
    }
    x = floorf(x);
    y = floorf(y);
    if (sResTex[which].loaded) C2D_DrawImageAt(sResTex[which].image, x, y, 0.0f, NULL, 1.0f, 1.0f);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", value);
    draw_text(x + RES_ICON + 2, y, 0.4f, warn ? C2D_Color32(255, 110, 90, 255) : C2D_Color32(235, 235, 235, 255), buf);
    return x + RES_ICON + 4 + font_width(0.4f, buf) + 5;
}

// An image scaled to w x h at (x, y), optionally tinted (alpha only when tint is 0).
static void draw_tex(const Texture* t, float x, float y, float w, float h, float alpha) {
    if (!t->loaded) return;
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawImageAt(t->image, x, y, 0.0f, alpha < 1.0f ? &tint : NULL, w / t->width, h / t->height);
}

// The beacon's space backdrop (events_imageList.xml), loaded when it changes.
static Texture sBgTex, sPlanetTex;
static char sBgName[64], sPlanetName[64];

// Backdrops are big PNGs (1280x720), so they're decoded on a worker thread below the main
// thread's priority; only making the GPU texture happens on the main thread.
typedef struct {
    char path[64];
    int minW, minH;
    uint8_t* rgba;
    int w, h;
    volatile bool done;
    Thread thread;
    Texture* target;
    char* name;  // where the loaded name is recorded
} ImageJob;

static void image_job_run(void* arg) {
    ImageJob* j = arg;
    uint32_t size;
    uint8_t* png = ftldat_read(sDat, j->path, &size);
    if (png && !texture_decode_png(png, size, j->minW, j->minH, &j->rgba, &j->w, &j->h)) j->rgba = NULL;
    free(png);
    __sync_synchronize();
    j->done = true;
}

static ImageJob sBgJob, sPlanetJob;

// Starts loading `path` into the job's texture, unless that's what it holds or is loading.
static void image_job_want(ImageJob* j, Texture* t, char* name, const char* path, int minW, int minH) {
    if (j->thread || !strcmp(name, path)) return;  // busy (checked again next frame), or current
    snprintf(j->path, sizeof(j->path), "%s", path);
    j->minW = minW;
    j->minH = minH;
    j->target = t;
    j->name = name;
    j->rgba = NULL;
    j->done = false;
    if (!path[0]) {
        j->done = true;
    } else {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        j->thread = threadCreate(image_job_run, j, 64 * 1024, prio < 0x3F ? prio + 1 : prio, -1, false);
        if (!j->thread) image_job_run(j);  // no thread: load now
    }
    if (!j->thread) j->thread = (Thread)1;  // marks "finished, not yet collected"
}

// Collects a finished job: swaps in the new texture. Returns true while still loading.
static bool image_job_poll(ImageJob* j) {
    if (!j->thread) return false;
    if (!j->done) return true;
    if (j->thread != (Thread)1) {
        threadJoin(j->thread, U64_MAX);
        threadFree(j->thread);
    }
    j->thread = NULL;
    gpu_idle();
    texture_free(j->target);
    if (j->rgba) texture_from_rgba(j->target, j->rgba, j->w, j->h);
    free(j->rgba);
    j->rgba = NULL;
    snprintf(j->name, 64, "%s", j->path);
    return false;
}

static bool sBackdropLoading;

static void backdrop_sync(const Run* r) {
    image_job_want(&sBgJob, &sBgTex, sBgName, r->bg, TOP_W, TOP_H);
    int px = r->planetSize * TOP_W / 1280;
    image_job_want(&sPlanetJob, &sPlanetTex, sPlanetName, r->planet, px, px);
    bool a = image_job_poll(&sBgJob), b = image_job_poll(&sPlanetJob);
    sBackdropLoading = a || b || strcmp(sBgName, r->bg) || strcmp(sPlanetName, r->planet);
}

// Warp streaks and the white flash at the moment of the jump, over the top screen.
static void draw_jump_fx(void) {
    if (sJumpTime < 0.0f) return;
    float strength = sJumpTime < JUMP_OUT ? sJumpTime / JUMP_OUT : 1.0f - (sJumpTime - JUMP_OUT) / JUMP_IN;
    for (int i = 0; i < 40; i++) {
        float y = (float)((i * 53) % TOP_H) + 2, speed = 500.0f + (i * 97) % 500;
        float len = (30.0f + (i * 37) % 90) * strength;
        float x = TOP_W - fmodf(sClock * speed + i * 71.0f, TOP_W + 200.0f);
        C2D_DrawRectSolid(x, y, 0, len, 1, C2D_Color32(220, 235, 255, (u8)(170 * strength)));
    }
    float flash = sJumpTime < JUMP_OUT ? (sJumpTime - JUMP_OUT * 0.7f) / (JUMP_OUT * 0.3f)
                                       : 1.0f - (sJumpTime - JUMP_OUT) / (JUMP_IN * 0.5f);
    if (flash > 0.0f) C2D_DrawRectSolid(0, 0, 0, TOP_W, TOP_H, C2D_Color32(255, 255, 255, (u8)((flash > 1 ? 1 : flash) * 255)));
}

// Covers the rectangle with the backdrop, darkened by dim (0..1).
// Asteroid fields: FTL's three rock layers (img/asteroids/asteroid_back1-3.png) drifting left at
// different speeds over the backdrop (xftl AsteroidEnvironment: a layer crosses ~400 px of the
// 1280 px screen in 13, 10 and 8 s). Loaded at half size only while at such a beacon.
static void gpu_idle(void);
static Texture sRockLayer[3];
static bool sRockLoaded;
static float sRockClock;

static void rock_layers(bool want) {
    if (want == sRockLoaded) return;
    if (!want) {
        gpu_idle();
        for (int i = 0; i < 3; i++) texture_free(&sRockLayer[i]);
        sRockLoaded = false;
        return;
    }
    sRockLoaded = true;
    for (int i = 0; i < 3; i++) {
        char path[48];
        snprintf(path, sizeof(path), "img/asteroids/asteroid_back%d.png", i + 1);
        uint32_t size;
        uint8_t* png = ftldat_read(sDat, path, &size);
        if (png && size >= 24) {
            int pw = (png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19];
            int ph = (png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23];
            if (texture_load_png_fit(&sRockLayer[i], png, size, pw / 2, ph / 2)) texture_set_smooth(&sRockLayer[i]);
        }
        free(png);
    }
}

static void draw_rock_layers(float x, float y, float w, float h) {
    static const float SPEED[3] = {400.0f / 13, 400.0f / 10, 400.0f / 8};  // 1280-wide screen px per second
    for (int i = 0; i < 3; i++) {
        const Texture* t = &sRockLayer[i];
        if (!t->loaded) continue;
        float s = w / 1280.0f * 2.0f;  // stored at half size
        float tw = t->width * s, th = t->height * s;
        float off = fmodf(sRockClock * SPEED[i] * w / 1280.0f, tw);
        for (float tx = x - off; tx < x + w; tx += tw)
            for (float ty = y; ty < y + h; ty += th) C2D_DrawImageAt(t->image, tx, ty, 0.0f, NULL, s, s);
    }
}


// ---- Nebula clouds (xftl doc/visuals): over the stars at a nebula or ion storm beacon ----
// Clouds (img/stars/nebula_large_a-c) sit on a 350 x 230 grid (1280 x 720 screen units) jittered
// by +-60 x 40, start at 300% or 400% scale and grow 4.8% a second; each fades in at 80% a second,
// then out at 3.2% a second, and once it's down to a random 60-90% a fresh cloud starts in its place
// while the old one goes at 16% a second. In an ion storm (darker) each cloud flashes lightning for
// 0.25 s every 6.25-18.75 s.
#define MAX_CLOUDS 48
typedef struct {
    bool active, rising, dying;
    float x, y, scale, alpha, trigger, lightning, lightLeft;
    int img;
} Cloud;
static Cloud sClouds[MAX_CLOUDS];
static Texture sCloudTex[3], sLightningTex;
static int sCloudEnv;  // ENV_NEBULA / ENV_STORM while loaded, else 0

// The clouds are 20-40 big translucent sprites stacked over each other: drawn straight onto the
// screen that's every pixel painted 15-40 times a frame (twice with 3D on), more than the GPU can
// keep up with. So they're drawn into a small texture instead (a quarter of the top screen's pixels,
// every other frame; they drift slowly and are soft anyway) and that is stretched over the screen.
#define CLOUD_LAYER_W 256
#define CLOUD_LAYER_H 144
static C3D_Tex sCloudLayerTex;
static Tex3DS_SubTexture sCloudLayerSub;
static C3D_RenderTarget* sCloudLayer;
static bool sCloudLayerDrawn;  // holds this beacon's clouds
static unsigned sCloudLayerFrame;

static void cloud_layer_free(void) {  // wait for the GPU first
    if (!sCloudLayer) return;
    C3D_RenderTargetDelete(sCloudLayer);
    C3D_TexDelete(&sCloudLayerTex);
    sCloudLayer = NULL;
    sCloudLayerDrawn = false;
}

static bool cloud_layer_make(void) {
    if (sCloudLayer) return true;
    if (!C3D_TexInitVRAM(&sCloudLayerTex, CLOUD_LAYER_W, 256, GPU_RGBA8)) return false;
    C3D_TexSetFilter(&sCloudLayerTex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&sCloudLayerTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sCloudLayer = C3D_RenderTargetCreateFromTex(&sCloudLayerTex, GPU_TEXFACE_2D, 0, -1);
    if (!sCloudLayer) {
        C3D_TexDelete(&sCloudLayerTex);
        return false;
    }
    sCloudLayerSub = (Tex3DS_SubTexture){CLOUD_LAYER_W, CLOUD_LAYER_H, 0.0f, 1.0f, 1.0f, 1.0f - CLOUD_LAYER_H / 256.0f};
    sCloudLayerDrawn = false;
    return true;
}

static float cloud_rand(float lo, float hi) { return lo + (hi - lo) * (rand() / (float)RAND_MAX); }

static void cloud_spawn(float x, float y) {
    for (int i = 0; i < MAX_CLOUDS; i++) {
        Cloud* c = &sClouds[i];
        if (c->active) continue;
        memset(c, 0, sizeof(*c));
        c->active = c->rising = true;
        c->x = x;
        c->y = y;
        c->scale = rand() % 2 ? 3.0f : 4.0f;
        c->trigger = cloud_rand(0.6f, 0.9f);
        c->lightning = cloud_rand(6.25f, 18.75f);
        c->img = rand() % 3;
        return;
    }
}

// Outside a frame: loads or frees the art as the beacon's environment changes.
static void clouds_want(int env) {
    if (env != ENV_NEBULA && env != ENV_STORM) env = 0;
    if (env == sCloudEnv) return;
    if (!env || !sCloudEnv) {
        gpu_idle();
        for (int i = 0; i < 3; i++) texture_free(&sCloudTex[i]);
        texture_free(&sLightningTex);
        cloud_layer_free();
    }
    sCloudLayerDrawn = false;
    sCloudEnv = env;
    memset(sClouds, 0, sizeof(sClouds));
    if (!env) return;
    for (int i = 0; i < 3 && !sCloudTex[i].loaded; i++) {
        char path[40];
        snprintf(path, sizeof(path), "img/stars/nebula_large_%c.png", 'a' + i);
        load_crop(&sCloudTex[i], path, 0, 0, 0, 0, false);
        texture_set_smooth(&sCloudTex[i]);
    }
    if (!sLightningTex.loaded) load_crop(&sLightningTex, "img/stars/lightning.png", 0, 0, 0, 0, false);
    for (float y = 0; y < 720 + 115; y += 230)
        for (float x = 0; x < 1280 + 175; x += 350) {
            cloud_spawn(x + cloud_rand(-60, 60), y + cloud_rand(-40, 40));
            // Start part-way through their lives, so the sky isn't empty on arrival.
            Cloud* c = NULL;
            for (int i = MAX_CLOUDS - 1; i >= 0 && !c; i--)
                if (sClouds[i].active) c = &sClouds[i];
            if (c) {
                c->rising = false;
                c->alpha = cloud_rand(c->trigger, 1.0f);
            }
        }
}

static void clouds_update(float dt) {
    if (!sCloudEnv) return;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        Cloud* c = &sClouds[i];
        if (!c->active) continue;
        c->scale *= 1.0f + 0.048f * dt;
        if (c->rising) {
            c->alpha += 0.8f * dt;
            if (c->alpha >= 1.0f) {
                c->alpha = 1.0f;
                c->rising = false;
            }
        } else if (c->dying) {
            c->alpha -= 0.16f * dt;
            if (c->alpha <= 0.0f) c->active = false;
        } else {
            c->alpha -= 0.032f * dt;
            if (c->alpha <= c->trigger) {
                c->dying = true;
                cloud_spawn(c->x, c->y);
            }
        }
        if (sCloudEnv == ENV_STORM) {
            if (c->lightLeft > 0.0f) c->lightLeft -= dt;
            if ((c->lightning -= dt) <= 0.0f) {
                c->lightning = cloud_rand(6.25f, 18.75f);
                c->lightLeft = 0.25f;
            }
        }
    }
}

// The clouds themselves over a w x h area (1280 x 720 screen units scaled to fit).
static void draw_cloud_sprites(float x, float y, float w, float h) {
    if (!sCloudEnv) return;
    float k = w / 1280.0f, ky = h / 720.0f;
    bool storm = sCloudEnv == ENV_STORM;
    for (int i = 0; i < MAX_CLOUDS; i++) {
        const Cloud* c = &sClouds[i];
        const Texture* t = &sCloudTex[c->img];
        if (!c->active || !t->loaded) continue;
        float s = c->scale * k, cx = x + c->x * k, cy = y + c->y * ky;
        C2D_ImageTint tint;
        u8 shade = storm ? 110 : 255;  // storms are darker
        C2D_PlainImageTint(&tint, C2D_Color32(shade, shade, shade, (u8)(c->alpha * 200)), storm ? 0.45f : 0.0f);
        for (int corner = 0; corner < 4; corner++) tint.corners[corner].color = (tint.corners[corner].color & 0x00FFFFFF) | ((u32)(c->alpha * 200) << 24);
        C2D_DrawImageAt(t->image, cx - t->width * s * 0.5f, cy - t->height * s * 0.5f, 0, &tint, s, s);
        if (storm && c->lightLeft > 0.0f && sLightningTex.loaded) {
            float ls = s * 0.5f;
            C2D_DrawImageAt(sLightningTex.image, cx - sLightningTex.width * ls * 0.5f, cy - sLightningTex.height * ls * 0.5f, 0, NULL, ls, ls);
        }
    }
}

#define BLEND_NORMAL() C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA)

// Inside a frame, before any screen is drawn: brings the cloud layer up to date.
static void clouds_render_layer(void) {
    if (!sCloudEnv || !cloud_layer_make()) return;
    if ((sCloudLayerFrame++ & 1) && sCloudLayerDrawn) return;
    C2D_TargetClear(sCloudLayer, 0);
    C2D_SceneBegin(sCloudLayer);
    C2D_ViewReset();
    // Alpha adds up as layers do (one over another), leaving premultiplied colour in the layer.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    draw_cloud_sprites(0, 0, CLOUD_LAYER_W, CLOUD_LAYER_H);
    C2D_Flush();
    BLEND_NORMAL();
    sCloudLayerDrawn = true;
}

// The clouds over a w x h area: the layer, stretched.
static void draw_clouds(float x, float y, float w, float h) {
    if (!sCloudEnv || !sCloudLayerDrawn) return;
    C2D_Image img = {&sCloudLayerTex, &sCloudLayerSub};
    C2D_Flush();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    C2D_DrawImageAt(img, x, y, 0, NULL, w / CLOUD_LAYER_W, h / CLOUD_LAYER_H);
    C2D_Flush();
    BLEND_NORMAL();
}

static void draw_backdrop(const Run* r, float x, float y, float w, float h, float dim) {
    if (sBgTex.loaded) {
        float s = w / sBgTex.width > h / sBgTex.height ? w / sBgTex.width : h / sBgTex.height;
        float bw = sBgTex.width * s, bh = sBgTex.height * s;
        C2D_DrawImageAt(sBgTex.image, x + (w - bw) * 0.5f, y + (h - bh) * 0.5f, 0.0f, NULL, s, s);
    }
    if (sPlanetTex.loaded) {
        float size = r->planetSize * w / 1280.0f;
        draw_tex(&sPlanetTex, x + r->planetX * w - size * 0.5f, y + r->planetY * h - size * 0.5f, size, size, 1.0f);
    }
    if (sRockLoaded) draw_rock_layers(x, y, w, h);
    if (dim <= 0.5f) draw_clouds(x, y, w, h);  // behind the map, events and the store they barely show: skip the cost
    if (dim > 0.0f) C2D_DrawRectSolid(x, y, 0, w, h, C2D_Color32(0, 0, 0, (u8)(dim * 255)));
}

// FTL's windows: rounded corners, a light border and a dark body. Colours must be opaque (the
// corners overlap).
#define UI_EDGE C2D_Color32(235, 235, 235, 255)
#define UI_BODY C2D_Color32(30, 37, 47, 255)

static void round_rect(float x, float y, float w, float h, float r, u32 col) {
    C2D_DrawRectSolid(x + r, y, 0, w - 2 * r, h, col);
    C2D_DrawRectSolid(x, y + r, 0, r, h - 2 * r, col);
    C2D_DrawRectSolid(x + w - r, y + r, 0, r, h - 2 * r, col);
    C2D_DrawCircleSolid(x + r, y + r, 0, r, col);
    C2D_DrawCircleSolid(x + w - r, y + r, 0, r, col);
    C2D_DrawCircleSolid(x + r, y + h - r, 0, r, col);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0, r, col);
}

static void ftl_box(float x, float y, float w, float h, u32 body) {
    round_rect(x, y, w, h, 7, UI_EDGE);
    round_rect(x + 2, y + 2, w - 4, h - 4, 5, body);
}

static float glyph_w(uint32_t cp, float scale) { return font_char_width(scale, cp); }

static float line_h(float scale) { return font_line_height(scale); }

// Copies s into out with line breaks so no line is wider than maxW. Returns the line count.
static int wrap_text(const char* s, float scale, float maxW, char* out, size_t size) {
    size_t o = 0;
    int lines = 1;
    float lineW = 0.0f, spaceW = glyph_w(' ', scale);
    const char* p = s;
    while (*p == ' ') p++;
    while (*p && o + 1 < size) {
        if (*p == '\n') {
            out[o++] = '\n';
            lineW = 0.0f;
            lines++;
            p++;
            while (*p == ' ') p++;
            continue;
        }
        const char* w = p;
        float ww = 0.0f;
        while (*w && *w != ' ' && *w != '\n') {
            uint32_t cp;
            ssize_t n = decode_utf8(&cp, (const uint8_t*)w);
            if (n <= 0) {
                n = 1;
                cp = '?';
            }
            ww += glyph_w(cp, scale);
            w += n;
        }
        if (lineW > 0.0f && lineW + spaceW + ww > maxW) {
            out[o++] = '\n';
            lineW = 0.0f;
            lines++;
        } else if (lineW > 0.0f) {
            out[o++] = ' ';
            lineW += spaceW;
        }
        size_t len = (size_t)(w - p);
        if (o + len >= size) len = size - 1 - o;
        memcpy(out + o, p, len);
        o += len;
        lineW += ww;
        p = w;
        while (*p == ' ') p++;
    }
    out[o] = '\0';
    return lines;
}

// The event's text, in an FTL window centred on the top screen.
static void draw_event_top(const EventView* v) {
    static char wrapped[1600];
    float scale = 0.5f;
    int lines = wrap_text(v->text, scale, TOP_W - 52, wrapped, sizeof(wrapped));
    while (lines * line_h(scale) > TOP_H - 44 && scale > 0.36f) {
        scale -= 0.03f;
        lines = wrap_text(v->text, scale, TOP_W - 52, wrapped, sizeof(wrapped));
    }
    float h = lines * line_h(scale) + 22, y = (TOP_H - h) * 0.5f;
    ftl_box(14, y, TOP_W - 28, h, UI_BODY);
    draw_text(26, y + 10, scale, C2D_Color32(235, 235, 235, 255), wrapped);
}

static float sChoiceY[MAX_CHOICES + 1];  // row tops, then the bottom of the last one

// The choices, in an FTL window centred on the touch screen. Each row is a touch target.
static void draw_event_choices(const Run* r, const EventView* v) {
    draw_backdrop(r, 0, 0, BOT_W, BOT_H, 0.55f);
    const float pad = 8, gap = 10;
    float scale = 0.48f, total = 0;
    char label[220], wrapped[400];
    int lines[MAX_CHOICES];
    for (int pass = 0; pass < 4; pass++) {  // shrink the text if the window doesn't fit
        total = 2 * pad;
        for (int i = 0; i < v->choiceCount; i++) {
            snprintf(label, sizeof(label), "%d. %s", i + 1, v->choiceText[i]);
            lines[i] = wrap_text(label, scale, BOT_W - 44, wrapped, sizeof(wrapped));
            total += lines[i] * line_h(scale) + (i ? gap : 0);
        }
        if (total <= BOT_H - 12) break;
        scale -= 0.04f;
    }
    float y = (BOT_H - total) * 0.5f;
    ftl_box(8, y, BOT_W - 16, total, UI_BODY);
    y += pad;
    for (int i = 0; i < v->choiceCount; i++) {
        float h = lines[i] * line_h(scale);
        sChoiceY[i] = y - gap * 0.5f;
        if (i) C2D_DrawRectSolid(18, y - gap * 0.5f, 0, BOT_W - 36, 1, C2D_Color32(70, 82, 100, 255));
        snprintf(label, sizeof(label), "%d. %s", i + 1, v->choiceText[i]);
        wrap_text(label, scale, BOT_W - 44, wrapped, sizeof(wrapped));
        u32 tc = v->choiceDisabled[i] ? C2D_Color32(110, 116, 126, 255)
                 : v->choiceBlue[i]   ? C2D_Color32(120, 200, 255, 255)
                                      : C2D_Color32(235, 235, 235, 255);
        draw_text(20, y, scale, tc, wrapped);
        y += h + gap;
    }
    sChoiceY[v->choiceCount] = y - gap * 0.5f;
}

static int event_choice_at(const EventView* v, float py) {
    for (int i = 0; i < v->choiceCount; i++)
        if (py >= sChoiceY[i] && py < sChoiceY[i + 1]) return i;
    return -1;
}

// Sector map, in FTL's beacon map frame (img/map/map_overlay.png, 548 x 430) scaled to the
// touch screen. The map itself is 660 x 440 inside the frame's window.
#define MAPF_S (BOT_H / 430.0f)
#define MAPF_X ((BOT_W - 548 * MAPF_S) * 0.5f)
#define RMAP_S (345 * MAPF_S / 440.0f)
#define RMAP_X (MAPF_X + 12 * MAPF_S + (523 * MAPF_S - 660 * RMAP_S) * 0.5f)
#define RMAP_Y (40 * MAPF_S)
#define RBTN_W 70
#define MAP_ACT_X (BOT_W - 96)  // button in the frame's bottom-right corner
#define MAP_ACT_Y (388 * MAPF_S)
enum { MAPT_NONE = -1, MAPT_CLOSE = -2, MAPT_ACTION = -3 };

static Texture sMapFrame, sBeaconNew, sBeaconSeen, sMapShip, sBeaconRing;

static Texture sMapNebula[3];

static void map_art_load(void) {
    if (sMapFrame.loaded) return;
    load_tex(&sMapFrame, "img/map/map_overlay.png", 0, 0);
    load_tex(&sBeaconNew, "img/map/map_icon_diamond_yellow.png", 0, 0);
    load_tex(&sBeaconSeen, "img/map/map_icon_diamond_blue.png", 0, 0);
    load_tex(&sMapShip, "img/map/map_icon_ship.png", 0, 0);
    load_tex(&sBeaconRing, "img/map/map_icon_diamond_circle.png", 0, 0);
    for (int i = 0; i < 3; i++) {
        char path[40];
        snprintf(path, sizeof(path), "img/map/nebula_large_%d.png", i);
        load_tex(&sMapNebula[i], path, 0, 0);
        texture_set_smooth(&sMapNebula[i]);
    }
}

// A small FTL button: rounded, light edge; lit ones are filled.
static void ftl_button(float x, float y, float w, float h, bool lit, const char* label) {
    round_rect(x, y, w, h, 4, lit ? C2D_Color32(235, 235, 235, 255) : C2D_Color32(150, 160, 175, 255));
    round_rect(x + 1.5f, y + 1.5f, w - 3, h - 3, 3, lit ? C2D_Color32(40, 110, 140, 255) : C2D_Color32(30, 37, 47, 255));
    float tw = font_width(0.4f, label);
    float s = tw > w - 6 ? (w - 6) / tw : 1.0f;
    font_draw(floorf(x + (w - tw * s) * 0.5f), floorf(y + (h - font_cap_height(0.4f)) * 0.5f), 0.4f,
              C2D_Color32(235, 235, 235, 255), label, s);
}

// WAIT on the map: out of fuel (or in the Last Stand) and not fighting. At a store it's offered
// only when no fuel can be bought there (none left, or not enough scrap), so a run can't get stuck.
static bool map_can_wait(const Run* r) {
    if ((r->fuel > 0 && !r->lastStand) || sFighting) return false;
    if (r->beacons[r->current].storeSlot <= 0) return true;
    return r->storeFuel <= 0 || r->scrap < run_price(r, STORE_FUEL);
}

// Beside a beacon we've been to (Long-Ranged Scanners reach that far).
static bool map_next_to_seen(const Run* r, int i) {
    for (int k = 0; k < r->beacons[i].nbCount; k++)
        if (r->beacons[r->beacons[i].nb[k]].visited) return true;
    return false;
}

static void draw_run_map(const Run* r) {
    map_art_load();
    const u32 white = C2D_Color32(235, 235, 235, 255);
    draw_backdrop(r, 0, 0, BOT_W, BOT_H, 0.6f);
    // The map's window, then the rebel fleet's area in red.
    C2D_DrawRectSolid(MAPF_X + 10 * MAPF_S, 36 * MAPF_S, 0, 528 * MAPF_S, 352 * MAPF_S, C2D_Color32(28, 52, 78, 255));
    for (float sx = 0; sx < 660; sx += 6) {
        float dx = sx + 3 - r->fleetX;
        if (fabsf(dx) >= 767.0f) continue;
        float half = sqrtf(767.0f * 767.0f - dx * dx), y0 = r->fleetY - half, y1 = r->fleetY + half;
        if (y0 < 0) y0 = 0;
        if (y1 > 440) y1 = 440;
        if (y1 > y0)
            C2D_DrawRectSolid(RMAP_X + sx * RMAP_S, RMAP_Y + y0 * RMAP_S, 0, 6 * RMAP_S + 0.5f, (y1 - y0) * RMAP_S,
                              ((int)(sx / 12) & 1) ? C2D_Color32(150, 40, 40, 170) : C2D_Color32(120, 30, 30, 170));
    }
    draw_tex(&sMapFrame, MAPF_X, 0, 548 * MAPF_S, BOT_H, 1.0f);
    // Nebulas (and ion storms) show as clouds over their beacons, as on FTL's map.
    for (int i = 0; i < r->beaconCount; i++) {
        int env = run_beacon_env(r, i);
        if (env != ENV_NEBULA && env != ENV_STORM) continue;
        const Texture* t = &sMapNebula[i % 3];
        if (!t->loaded) continue;
        float size = 150.0f * RMAP_S, x = RMAP_X + r->beacons[i].x * RMAP_S, y = RMAP_Y + r->beacons[i].y * RMAP_S;
        float s = size / (t->width > t->height ? t->width : t->height);
        C2D_ImageTint tint;
        C2D_PlainImageTint(&tint, env == ENV_STORM ? C2D_Color32(90, 80, 120, 200) : C2D_Color32(255, 255, 255, 200), env == ENV_STORM ? 0.4f : 0.0f);
        for (int c = 0; c < 4; c++) tint.corners[c].color = (tint.corners[c].color & 0x00FFFFFF) | (200u << 24);
        C2D_DrawImageAt(t->image, x - t->width * s * 0.5f, y - t->height * s * 0.5f, 0, &tint, s, s);
    }

    for (int a = 0; a < r->beaconCount; a++)
        for (int k = 0; k < r->beacons[a].nbCount; k++) {
            int b = r->beacons[a].nb[k];
            if (b < a) continue;
            // FTL's beacon map only draws the routes out of the current beacon.
            bool live = (a == r->current || b == r->current);
            if (!live) continue;
            u32 col = C2D_Color32(255, 255, 255, 230);
            C2D_DrawLine(RMAP_X + r->beacons[a].x * RMAP_S, RMAP_Y + r->beacons[a].y * RMAP_S, col,
                         RMAP_X + r->beacons[b].x * RMAP_S, RMAP_Y + r->beacons[b].y * RMAP_S, col, live ? 1.5f : 1.0f, 0);
        }
    const float icon = 15.0f;
    for (int i = 0; i < r->beaconCount; i++) {
        const Beacon* b = &r->beacons[i];
        float x = RMAP_X + b->x * RMAP_S, y = RMAP_Y + b->y * RMAP_S;
        if (run_can_jump(r, i)) {
            float pulse = 0.6f + 0.4f * sinf(sClock * 5.0f);
            draw_tex(&sBeaconRing, x - icon, y - icon, icon * 2, icon * 2, pulse);
        }
        const Texture* t = b->visited ? &sBeaconSeen : &sBeaconNew;
        draw_tex(t, x - icon * 0.5f, y - icon * 0.5f, icon, icon, 1.0f);
        if (run_beacon_overtaken(r, i)) C2D_DrawCircleSolid(x, y, 0, 2.5f, C2D_Color32(230, 50, 40, 255));
        if (b->fleetSoon && fmodf(sClock, 0.8f) < 0.45f)  // the rebels take it next turn
            for (int k = 0; k < 16; k++) {
                float a = k * 0.3927f;
                C2D_DrawCircleSolid(x + cosf(a) * 10, y + sinf(a) * 10, 0, 1.6f, C2D_Color32(240, 60, 45, 255));
            }
        int scanEnv;
        bool scanShip;
        if (run_has_aug(r, "ADV_SCANNERS") && map_next_to_seen(r, i) && run_beacon_scan(r, i, &scanEnv, &scanShip)) {
            static const char* const envTag[] = {"", "NEBULA", "STORM", "ASTEROIDS", "SUN", "PULSAR", "ASB"};
            char scan[24];
            snprintf(scan, sizeof(scan), "%s%s%s", scanShip ? "SHIP" : "", scanShip && scanEnv ? " " : "", envTag[scanEnv]);
            if (scan[0]) {
                float lw = font_width(0.32f, scan) + 4, lx = x - lw * 0.5f;
                C2D_DrawRectSolid(lx, y + 8, 0, lw, 10, C2D_Color32(20, 26, 36, 220));
                draw_text(lx + 2, y + 7, 0.32f, scanShip ? C2D_Color32(255, 120, 100, 255) : C2D_Color32(255, 200, 90, 255), scan);
            }
        }
        // What FTL marks (wiki: Beacon): the exit always; quests from any distance; stores and
        // distress beacons once we've been next to them (stores stay marked, distress until visited).
        bool near = b->visited || map_next_to_seen(r, i);
        bool store = near && run_beacon_store(r, i);
        const char* tag = b->exit                                   ? "EXIT"
                          : store                                   ? "STORE"
                          : b->quest && !b->visited                 ? "QUEST"
                          : near && run_beacon_distress(r, i)       ? "DISTRESS"
                                                                    : NULL;
        if (tag) {
            u32 tc = b->exit                   ? C2D_Color32(120, 255, 140, 255)
                     : store                   ? C2D_Color32(255, 220, 120, 255)
                     : b->quest && !b->visited ? C2D_Color32(120, 200, 255, 255)
                                               : C2D_Color32(255, 140, 110, 255);
            // Right of the beacon, or left of it if that would run past the map's edge.
            float lw = font_width(0.36f, tag) + 4, lx = x + 6;
            float mapRight = RMAP_X + 660 * RMAP_S;
            if (lx + lw > mapRight) lx = x - 6 - lw;
            C2D_DrawRectSolid(lx, y - 13, 0, lw, 11, C2D_Color32(20, 26, 36, 220));
            draw_text(lx + 2, y - 14, 0.36f, tc, tag);
        }
    }
    if (r->lastStand && r->flagStage <= 3) {
        // The flagship (img/map/map_icon_boss.png) and where it goes next: a dotted line while it
        // gets ready, solid red when it jumps this turn. The base shows how long it has been there.
        static Texture bossIcon;
        static bool bossTried;
        if (!bossTried) {
            bossTried = true;
            load_crop(&bossIcon, "img/map/map_icon_boss.png", 0, 0, 0, 0, false);
        }
        const Beacon* f = &r->beacons[r->flagBeacon];
        float fx = RMAP_X + f->x * RMAP_S, fy = RMAP_Y + f->y * RMAP_S;
        if (r->flagNext >= 0) {
            const Beacon* n = &r->beacons[r->flagNext];
            float nx = RMAP_X + n->x * RMAP_S, ny = RMAP_Y + n->y * RMAP_S;
            u32 lc = r->flagJumping ? C2D_Color32(255, 70, 50, 255) : C2D_Color32(255, 120, 90, 200);
            int dashes = 10;
            for (int k = 0; k < dashes; k++) {
                if (!r->flagJumping && (k & 1)) continue;
                float a = k / (float)dashes, b2 = (k + 1) / (float)dashes;
                C2D_DrawLine(fx + (nx - fx) * a, fy + (ny - fy) * a, lc, fx + (nx - fx) * b2, fy + (ny - fy) * b2, lc, 2.0f, 0);
            }
        }
        if (bossIcon.loaded) {
            float s = 24.0f, rot = sClock * 0.31f;  // circling its beacon, once every 20 s
            C2D_DrawImageAtRotated(bossIcon.image, fx + cosf(rot) * 8, fy + sinf(rot) * 8, 0, rot, NULL, s / bossIcon.width, s / bossIcon.height);
        } else {
            C2D_DrawCircleSolid(fx, fy, 0, 6, C2D_Color32(230, 50, 40, 255));
        }
        const Beacon* base = &r->beacons[r->baseBeacon];
        char tag[24];
        snprintf(tag, sizeof(tag), r->flagBeacon == r->baseBeacon ? "BASE %d/3" : "BASE", r->baseTurns);
        float bx = RMAP_X + base->x * RMAP_S, by = RMAP_Y + base->y * RMAP_S;
        float lw = font_width(0.36f, tag) + 4, lx = bx + 6;
        if (lx + lw > RMAP_X + 660 * RMAP_S) lx = bx - 6 - lw;
        C2D_DrawRectSolid(lx, by - 13, 0, lw, 11, C2D_Color32(20, 26, 36, 220));
        draw_text(lx + 2, by - 14, 0.36f, r->flagBeacon == r->baseBeacon ? C2D_Color32(255, 90, 70, 255) : C2D_Color32(120, 200, 255, 255), tag);
    }
    const Beacon* cur = &r->beacons[r->current];
    if (sMapShip.loaded) {
        float s = 26.0f;
        draw_tex(&sMapShip, RMAP_X + cur->x * RMAP_S - s * 0.5f, RMAP_Y + cur->y * RMAP_S - s * 0.62f, s, s, 1.0f);
    }

    // Header: fuel and scrap beside the frame's title, CLOSE on the right.
    char buf[96];
    C2D_DrawRectSolid(MAPF_X + 268 * MAPF_S, 0, 0, BOT_W - 56 - (MAPF_X + 268 * MAPF_S), 13, C2D_Color32(10, 13, 24, 255));
    float rx = draw_resource(MAPF_X + 272 * MAPF_S, 0, RES_FUEL, r->fuel, r->fuel <= 3);
    draw_resource(rx, 0, RES_SCRAP, r->scrap, false);
    ftl_button(BOT_W - 52, 1, 50, 17, false, "CLOSE");
    // Footer: the frame's SECTOR boxes hold the number and name.
    snprintf(buf, sizeof(buf), "%d", r->sector + 1);
    draw_text(MAPF_X + 160 * MAPF_S, 391 * MAPF_S, 0.45f, white, buf);
    draw_text_fit_left(MAPF_X + 212 * MAPF_S, 392 * MAPF_S, 0.4f, MAP_ACT_X - 4 - (MAPF_X + 212 * MAPF_S), white, r->sectorName);
    if (map_can_wait(r))
        ftl_button(MAP_ACT_X, MAP_ACT_Y, BOT_W - MAP_ACT_X - 2, 20, true, "WAIT");
    else if (cur->exit) ftl_button(MAP_ACT_X, MAP_ACT_Y, BOT_W - MAP_ACT_X - 2, 20, true, "NEXT SECTOR");
    else if (cur->storeSlot > 0) ftl_button(MAP_ACT_X, MAP_ACT_Y, BOT_W - MAP_ACT_X - 2, 20, true, "STORE");
}

// ---- Sector map: the run's eight columns of sectors; pick where to go from the exit.

#define SMAP_X0 18
#define SMAP_DX ((BOT_W - 36) / 7.0f)
#define SMAP_Y0 34
#define SMAP_H 150

static float smap_x(int col) { return SMAP_X0 + col * SMAP_DX; }
static float smap_y(const Run* r, int col, int row) { return SMAP_Y0 + (row + 0.5f) * SMAP_H / r->sectorCount[col]; }

static u32 sector_colour(int cls, bool dim) {
    static const u8 c[4][3] = {{100, 210, 110}, {220, 80, 70}, {170, 110, 230}, {245, 205, 70}};
    int k = dim ? 2 : 1;
    return C2D_Color32(c[cls][0] / k, c[cls][1] / k, c[cls][2] / k, 255);
}

static void draw_sector_map(const Run* r) {
    draw_backdrop(r, 0, 0, BOT_W, BOT_H, 0.65f);
    draw_text(6, 3, 0.5f, C2D_Color32(255, 220, 120, 255), "SECTOR MAP");
    ftl_button(BOT_W - 52, 1, 50, 17, false, "CLOSE");
    ftl_box(4, SMAP_Y0 - 12, BOT_W - 8, SMAP_H + 24, C2D_Color32(18, 24, 38, 255));
    for (int col = 0; col + 1 < RUN_SECTORS; col++)
        for (int row = 0; row < r->sectorCount[col]; row++) {
            const SectorNode* n = &r->sectors[col][row];
            for (int k = 0; k < n->nextCount; k++) {
                bool live = col == r->sector && row == r->sectorRow;
                u32 lc = live ? C2D_Color32(255, 255, 255, 230) : C2D_Color32(90, 100, 125, 200);
                C2D_DrawLine(smap_x(col), smap_y(r, col, row), lc, smap_x(col + 1), smap_y(r, col + 1, n->next[k]), lc,
                             live ? 1.5f : 1.0f, 0);
            }
        }
    for (int col = 0; col < RUN_SECTORS; col++)
        for (int row = 0; row < r->sectorCount[col]; row++) {
            float x = smap_x(col), y = smap_y(r, col, row);
            bool here = col == r->sector && row == r->sectorRow, next = col == r->sector + 1 && run_sector_reachable(r, row);
            if (next) {
                float pulse = 9.0f + sinf(sClock * 5.0f);
                C2D_DrawCircleSolid(x, y, 0, pulse, sSectorPick == row ? C2D_Color32(255, 220, 90, 255) : C2D_Color32(235, 235, 235, 255));
            }
            if (here) C2D_DrawCircleSolid(x, y, 0, 9.0f, C2D_Color32(255, 220, 90, 255));
            C2D_DrawCircleSolid(x, y, 0, 6.0f, C2D_Color32(10, 12, 20, 255));
            C2D_DrawCircleSolid(x, y, 0, 5.0f, sector_colour(r->sectors[col][row].cls, col < r->sector));
        }
    // Legend and the pick.
    float ly = SMAP_Y0 + SMAP_H + 18;
    static const char* const names[3] = {"Civilian", "Hostile", "Nebula"};
    for (int i = 0; i < 3; i++) {
        C2D_DrawCircleSolid(14 + i * 72, ly + 6, 0, 4.0f, sector_colour(i, false));
        draw_text(22 + i * 72, ly, 0.4f, C2D_Color32(200, 205, 215, 255), names[i]);
    }
    if (sSectorPick >= 0) {
        draw_text_fit_left(8, BOT_H - 20, 0.45f, BOT_W - 100, C2D_Color32(255, 220, 120, 255), r->sectors[r->sector + 1][sSectorPick].name);
        ftl_button(BOT_W - 82, BOT_H - 24, 78, 21, true, "JUMP");
    } else {
        draw_text(8, BOT_H - 20, 0.4f, C2D_Color32(170, 180, 195, 255), "Tap a highlighted sector to go there next.");
    }
}

enum { SMAP_NONE = -1, SMAP_CLOSE = -2, SMAP_JUMP = -3 };
static int sector_map_touch(const Run* r, float px, float py) {
    if (px >= BOT_W - 52 && py < 20) return SMAP_CLOSE;
    if (sSectorPick >= 0 && px >= BOT_W - 82 && py >= BOT_H - 24) return SMAP_JUMP;
    int col = r->sector + 1;
    if (col >= RUN_SECTORS) return SMAP_NONE;
    for (int row = 0; row < r->sectorCount[col]; row++) {
        float dx = px - smap_x(col), dy = py - smap_y(r, col, row);
        if (dx * dx + dy * dy < 16 * 16 && run_sector_reachable(r, row)) return row;
    }
    return SMAP_NONE;
}

static int run_map_touch(const Run* r, float px, float py) {
    if (px >= BOT_W - 52 && py < 20) return MAPT_CLOSE;
    if (px >= MAP_ACT_X && py >= MAP_ACT_Y) return MAPT_ACTION;
    int best = MAPT_NONE;
    float bestD = 16.0f * 16.0f;
    for (int i = 0; i < r->beaconCount; i++) {
        float dx = px - (RMAP_X + r->beacons[i].x * RMAP_S), dy = py - (RMAP_Y + r->beacons[i].y * RMAP_S);
        if (dx * dx + dy * dy < bestD) {
            bestD = dx * dx + dy * dy;
            best = i;
        }
    }
    return best;
}

// ---- Stores and the ship screen ------------------------------------------------------------

static const char* const KIND_TAB[ITEM_KINDS] = {"WEAPONS", "DRONES", "AUGS", "CREW", "SYSTEMS"};

static const char* item_title_of(const char* name) {
    const ItemDef* it = item_find(name);
    return it ? it->title : name;
}

// What the top screen describes while a store or the ship screen is open.
static char sInfoTitle[64], sInfoDesc[400];

static void set_info(const char* title, const char* desc) {
    snprintf(sInfoTitle, sizeof(sInfoTitle), "%s", title ? title : "");
    snprintf(sInfoDesc, sizeof(sInfoDesc), "%s", desc ? desc : "");
}

static void set_info_item(const char* name) {
    const ItemDef* it = item_find(name);
    if (it) set_info(it->title, it->desc);
}

static void draw_info_top(void) {
    if (!sInfoTitle[0]) return;
    static char wrapped[600];
    float scale = 0.42f;
    int lines = sInfoDesc[0] ? wrap_text(sInfoDesc, scale, TOP_W - 52, wrapped, sizeof(wrapped)) : 0;
    float h = 24 + lines * line_h(scale) + 6, y = TOP_H - h - 6;
    ftl_box(14, y, TOP_W - 28, h, UI_BODY);
    draw_text(24, y + 6, 0.5f, C2D_Color32(255, 220, 120, 255), sInfoTitle);
    if (lines) draw_text(24, y + 24, scale, C2D_Color32(235, 235, 235, 255), wrapped);
}

// A system's panel icon centred on (cx, cy), scaled by the symbol rather than its padding.
static void draw_sys_icon(const ShipSystem* y, float cx, float cy, float size) {
    const Texture* icon = &y->panelOn;
    if (!icon->loaded) return;
    const int* ib = y->iconBounds;
    float gw = (float)(ib[2] - ib[0] + 1), gh = (float)(ib[3] - ib[1] + 1);
    float scale = size / (gw > gh ? gw : gh);
    C2D_DrawImageAt(icon->image, floorf(cx - (ib[0] + ib[2] + 1) * 0.5f * scale), floorf(cy - (ib[1] + ib[3] + 1) * 0.5f * scale),
                    0.0f, NULL, scale, scale);
}

// Shared header: title, scrap, a close button; then a row of tabs.
#define SHOP_TABS_Y 24
#define SHOP_TOP 46
static void shop_header(const Run* r, const char* title, const char* closeLabel, const char* const* tabs, int tabCount, int tab) {
    draw_backdrop(r, 0, 0, BOT_W, BOT_H, 0.65f);
    draw_text(6, 3, 0.5f, C2D_Color32(255, 220, 120, 255), title);
    char buf[32];
    (void)buf;
    draw_resource(120, 4, RES_SCRAP, r->scrap, false);
    ftl_button(BOT_W - 60, 2, 58, 18, false, closeLabel);
    float w = (BOT_W - 4.0f) / tabCount;
    for (int i = 0; i < tabCount; i++) ftl_button(2 + i * w, SHOP_TABS_Y, w - 2, 18, i == tab, tabs[i]);
}

static int shop_tab_at(int tabCount, float px, float py) {
    if (py < SHOP_TABS_Y || py >= SHOP_TABS_Y + 18) return -1;
    int i = (int)((px - 2) / ((BOT_W - 4.0f) / tabCount));
    return i >= 0 && i < tabCount ? i : -1;
}

// ---- Item pictures, for the store: weapons from their hull sprite (frame 0, turned to point
// right), drones from their sprite, crew as their race's figure, systems as their room icon.

typedef struct {
    char name[32];
    Texture tex;
    bool rotate;   // weapons are drawn pointing up in FTL's sheets
    bool tinted;   // system icons: tint green
} ItemArt;
static ItemArt sItemArt[16];
static int sItemArtNext;

static const ItemArt* item_art(int kind, const char* name) {
    if (!name[0]) return NULL;
    for (int i = 0; i < 16; i++)
        if (!strcmp(sItemArt[i].name, name)) return sItemArt[i].tex.loaded ? &sItemArt[i] : NULL;
    if (sItemArtNext >= 16) return NULL;  // full until the store closes (item_art_clear)
    ItemArt* a = &sItemArt[sItemArtNext++];
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "%s", name);
    char path[96];
    if (kind == ITEM_WEAPON) {
        const WeaponDef* w = weapon_find(name);
        WeaponAnimInfo wa;
        SheetInfo sh;
        if (w && animdb_weapon(sAnimDb, w->art, &wa) && animdb_sheet(sAnimDb, wa.sheet, &sh)) {
            load_crop(&a->tex, sh.file, wa.x * sh.fw, (sh.h / sh.fh - 1 - wa.y) * sh.fh, sh.fw, sh.fh, false);
            a->rotate = true;
        }
    } else if (kind == ITEM_DRONE) {
        const DroneDef* d = drone_find(name);
        if (d) {
            snprintf(path, sizeof(path), "img/ship/drones/%s_base.png", d->image);
            load_crop(&a->tex, path, 0, 0, 0, 0, false);
        }
    } else if (kind == ITEM_CREW) {
        snprintf(path, sizeof(path), "img/people/%s_base.png", name);
        load_crop(&a->tex, path, 0, 0, 35, 35, false);  // first walking-down frame
    } else if (kind == ITEM_SYSTEM) {
        snprintf(path, sizeof(path), "img/icons/s_%s_overlay.png", name);
        load_crop(&a->tex, path, 0, 0, 0, 0, false);
        a->tinted = true;
    }
    return a->tex.loaded ? a : NULL;
}

// Frees the pictures (call outside a frame: it waits for the GPU).
static void item_art_clear(void) {
    gpu_idle();
    for (int i = 0; i < sItemArtNext; i++) texture_free(&sItemArt[i].tex);
    memset(sItemArt, 0, sizeof(sItemArt));
    sItemArtNext = 0;
}

// Draws an item picture centred in a w x h box, unscaled (or halved if it's too big).
static void draw_item_art(const ItemArt* a, float x, float y, float w, float h) {
    if (!a) return;
    float iw = a->rotate ? a->tex.height : a->tex.width, ih = a->rotate ? a->tex.width : a->tex.height;
    float s = iw > w || ih > h ? 0.5f : 1.0f;
    float cx = floorf(x + w * 0.5f), cy = floorf(y + h * 0.5f);
    C2D_ImageTint tint;
    if (a->tinted) {
        C2D_SetTintMode(C2D_TintMult);
        C2D_PlainImageTint(&tint, C2D_Color32(120, 235, 120, 255), 1.0f);
    }
    if (a->rotate)
        C2D_DrawImageAtRotated(a->tex.image, cx, cy, 0.0f, (float)M_PI_2, a->tinted ? &tint : NULL, s, s);
    else
        C2D_DrawImageAt(a->tex.image, floorf(cx - iw * s * 0.5f), floorf(cy - ih * s * 0.5f), 0.0f, a->tinted ? &tint : NULL, s, s);
    if (a->tinted) C2D_SetTintMode(C2D_TintSolid);
}

// ---- Store

static int sStoreTab, sStoreSel = -1;

enum { SELL_WEAPON, SELL_DRONE, SELL_CARGO, SELL_AUG };
typedef struct {
    int what, index;
    const char* name;
} Sellable;

static int sellables(const Run* r, const Ship* s, Sellable* out) {
    int n = 0;
    for (int i = 0; i < s->weaponCount; i++) out[n++] = (Sellable){SELL_WEAPON, i, s->weapons[i].def->name};
    for (int i = 0; i < s->droneCount; i++) out[n++] = (Sellable){SELL_DRONE, i, s->drones[i].def->name};
    for (int i = 0; i < CARGO_SLOTS; i++)
        if (r->cargo[i][0]) out[n++] = (Sellable){SELL_CARGO, i, r->cargo[i]};
    for (int i = 0; i < r->augCount; i++) out[n++] = (Sellable){SELL_AUG, i, r->augs[i]};
    return n;
}

static int sell_price(const char* name) {
    const ItemDef* it = item_find(name);
    return it ? it->cost / 2 : 0;  // half price, rounded down (wiki: Scrap)
}

static const char* store_tab_name(const Run* r, int i) {
    return i == 0 ? "SUPPLY" : i <= r->storeSectionCount ? KIND_TAB[r->storeKind[i - 1]] : "SELL";
}

#define SELL_ROW_H 21

static void draw_store(const Run* r, const Ship* s) {
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(160, 170, 185, 255);
    const char* tabs[STORE_SECTIONS + 2];
    int tabCount = r->storeSectionCount + 2;
    for (int i = 0; i < tabCount; i++) tabs[i] = store_tab_name(r, i);
    shop_header(r, "STORE", "LEAVE", tabs, tabCount, sStoreTab);
    char buf[96];
    if (sStoreTab == 0) {
        const char* names[4] = {"Fuel", "Missiles", "Drone parts", "Repair hull"};
        int stock[4] = {r->storeFuel, r->storeMissiles, r->storeParts, s->maxHull - s->hullHp};
        int have[4] = {r->fuel, s->missiles, s->droneParts, s->hullHp};
        for (int i = 0; i < 4; i++) {
            float y = SHOP_TOP + i * 47;
            ftl_box(4, y, BOT_W - 8, 44, UI_BODY);
            draw_text(14, y + 5, 0.5f, white, names[i]);
            if (i == STORE_HULL)
                snprintf(buf, sizeof(buf), "hull %d/%d", have[i], s->maxHull);
            else
                snprintf(buf, sizeof(buf), "have %d   in stock %d", have[i], stock[i]);
            draw_text(14, y + 24, 0.4f, dim, buf);
            snprintf(buf, sizeof(buf), "BUY  %d", run_price(r, i));
            ftl_button(BOT_W - 82, y + 11, 70, 22, stock[i] > 0 && r->scrap >= run_price(r, i), buf);
        }
    } else if (sStoreTab <= r->storeSectionCount) {
        int sec = sStoreTab - 1;
        for (int k = 0; k < STORE_ITEMS; k++) {
            float y = SHOP_TOP + k * 63;
            const char* name = r->storeItem[sec][k];
            ftl_box(4, y, BOT_W - 8, 60, k == sStoreSel ? C2D_Color32(50, 62, 80, 255) : UI_BODY);
            if (!name[0]) {
                draw_text(14, y + 20, 0.5f, dim, "SOLD");
                continue;
            }
            const char* title = name;
            if (r->storeKind[sec] == ITEM_SYSTEM) {
                const SysBlueprint* sb = item_system(system_from_name(name));
                title = sb ? sb->title : name;
            } else {
                title = item_title_of(name);
            }
            // Picture on the left, in a dark well.
            C2D_DrawRectSolid(10, y + 6, 0, 66, 48, C2D_Color32(14, 18, 26, 255));
            draw_item_art(item_art(r->storeKind[sec], name), 10, y + 6, 66, 48);
            draw_text_fit_left(84, y + 8, 0.52f, BOT_W - 184, white, title);
            if (r->storeKind[sec] == ITEM_CREW && r->storeSkill[sec][k][0]) {
                // Trained: each skill's icon in its level's colour.
                for (int j = 0; j < 2; j++) {
                    int v = r->storeSkill[sec][k][j];
                    if (v) draw_skill_icon(v / 4, 94 + j * 26, y + 42, skill_colour(v % 4));
                }
            } else {
                const char* note = "tap for details";
                if (r->storeKind[sec] == ITEM_SYSTEM) {
                    int t = system_from_name(name);
                    note = t == SYS_CLONEBAY && ship_find_system(s, SYS_MEDBAY) >= 0   ? "replaces the medbay"
                           : t == SYS_MEDBAY && ship_find_system(s, SYS_CLONEBAY) >= 0 ? "replaces the clone bay"
                                                                                       : "installs in its room";
                }
                draw_text(84, y + 32, 0.4f, dim, note);
            }
            int price = run_store_price(r, sec, k);
            snprintf(buf, sizeof(buf), "BUY  %d", price);
            ftl_button(BOT_W - 90, y + 18, 78, 24, r->scrap >= price, buf);
        }
    } else {
        Sellable items[16];
        int n = sellables(r, s, items);
        if (!n) draw_text(10, SHOP_TOP + 6, 0.45f, dim, "Nothing to sell.");
        for (int i = 0; i < n; i++) {
            float x = 4 + (i % 2) * 157, y = SHOP_TOP + (i / 2) * (SELL_ROW_H + 2);
            ftl_box(x, y, 155, SELL_ROW_H, UI_BODY);
            draw_text_fit_left(x + 7, y + 4, 0.38f, 92, white, item_title_of(items[i].name));
            snprintf(buf, sizeof(buf), "SELL %d", sell_price(items[i].name));
            ftl_button(x + 101, y + 2, 51, SELL_ROW_H - 4, true, buf);
        }
    }
}

enum { ST_NONE = -1, ST_LEAVE = -2 };
#define ST_TAB 1000
#define ST_RES 2000
#define ST_BUY 3000
#define ST_PICK 4000
#define ST_SELL 5000

static int store_touch(const Run* r, const Ship* s, float px, float py) {
    if (px >= BOT_W - 60 && py < 22) return ST_LEAVE;
    int t = shop_tab_at(r->storeSectionCount + 2, px, py);
    if (t >= 0) return ST_TAB + t;
    if (py < SHOP_TOP) return ST_NONE;
    if (sStoreTab == 0) {
        int row = (int)((py - SHOP_TOP) / 47);
        return row < 4 && px >= BOT_W - 82 ? ST_RES + row : ST_NONE;
    }
    if (sStoreTab <= r->storeSectionCount) {
        int k = (int)((py - SHOP_TOP) / 63);
        if (k >= STORE_ITEMS) return ST_NONE;
        return px >= BOT_W - 90 ? ST_BUY + k : ST_PICK + k;
    }
    Sellable items[16];
    int n = sellables(r, s, items);
    int i = (int)((py - SHOP_TOP) / (SELL_ROW_H + 2)) * 2 + (px >= 4 + 157 ? 1 : 0);
    float x = 4 + (i % 2) * 157;
    if (i < n && px >= x + 101) return ST_SELL + i;
    if (i < n) set_info_item(items[i].name);
    return ST_NONE;
}

// Sells a weapon/drone/cargo/augment. Frees textures, so it waits for the GPU first.
static bool store_sell(Run* r, Ship* s, int i) {
    Sellable items[16];
    int n = sellables(r, s, items);
    if (i < 0 || i >= n) return false;
    Sellable it = items[i];
    if (it.what == SELL_DRONE && (s->drones[it.index].deployed || s->drones[it.index].powered)) return false;
    r->scrap += sell_price(it.name);
    gpu_idle();
    switch (it.what) {
        case SELL_WEAPON: combat_remove_weapon(s, it.index); break;
        case SELL_DRONE: combat_remove_drone(s, it.index); break;
        case SELL_CARGO: r->cargo[it.index][0] = '\0'; break;
        default:
            for (int k = it.index; k < r->augCount - 1; k++) memcpy(r->augs[k], r->augs[k + 1], sizeof(r->augs[k]));
            r->augCount--;
    }
    sound_play("buy");
    return true;
}

// ---- Ship screen: upgrades and equipment

static bool sShipOpen;
static int sShipTab;        // 0 upgrades, 1 equipment
static int sEquipSel = -1;  // selected equipment cell: row * 8 + column

#define UP_COLS 3
#define UP_W ((BOT_W - 4) / UP_COLS)
#define UP_H 37

// Cost of the next level of system i (or the reactor when i == systemCount), 0 if maxed.
static int upgrade_cost(const Ship* s, int i) {
    if (i == s->systemCount) return s->reactor < REACTOR_MAX ? item_reactor_cost(s->reactor) : 0;
    const SysBlueprint* sb = item_system(s->systems[i].def->type);
    int lv = s->systems[i].def->level;
    if (!sb || lv >= sb->maxPower || lv - 1 >= sb->upgradeCount) return 0;
    return sb->upgrade[lv - 1];
}

static void draw_upgrades(const Run* r, const Ship* s) {
    const u32 white = C2D_Color32(235, 235, 235, 255);
    char buf[32];
    for (int i = 0; i <= s->systemCount; i++) {
        float x = 2 + (i % UP_COLS) * UP_W, y = SHOP_TOP + (i / UP_COLS) * (UP_H + 1);
        ftl_box(x, y, UP_W - 2, UP_H, UI_BODY);
        int cost = upgrade_cost(s, i);
        if (i < s->systemCount) {
            const ShipSystem* sy = &s->systems[i];
            draw_sys_icon(sy, x + 16, y + UP_H * 0.5f, 22);
            const SysBlueprint* sb = item_system(sy->def->type);
            snprintf(buf, sizeof(buf), "%d/%d", sy->def->level, sb ? sb->maxPower : sy->def->level);
        } else {
            draw_text(x + 6, y + 4, 0.38f, C2D_Color32(120, 220, 120, 255), "REACTOR");
            snprintf(buf, sizeof(buf), "%d/%d", s->reactor, REACTOR_MAX);
        }
        draw_text(x + (i < s->systemCount ? 32 : 6), y + (i < s->systemCount ? 3 : 18), 0.4f, white, buf);
        if (cost) {
            snprintf(buf, sizeof(buf), "+  %d", cost);
            ftl_button(x + UP_W - 54, y + UP_H - 21, 48, 17, r->scrap >= cost, buf);
        } else {
            draw_text(x + UP_W - 40, y + UP_H - 19, 0.38f, C2D_Color32(150, 160, 175, 255), "MAX");
        }
    }
    draw_text(4, BOT_H - 14, 0.34f, C2D_Color32(150, 160, 175, 255), "Tap + to buy a level. Systems: tap the icon for details.");
}

static int upgrade_touch(const Ship* s, float px, float py) {
    if (py < SHOP_TOP) return -1;
    int i = (int)((py - SHOP_TOP) / (UP_H + 1)) * UP_COLS + (int)((px - 2) / UP_W);
    if (i < 0 || i > s->systemCount) return -1;
    float x = 2 + (i % UP_COLS) * UP_W;
    if (px >= x + UP_W - 56) return i;  // the + button
    if (i < s->systemCount) {
        const SysBlueprint* sb = item_system(s->systems[i].def->type);
        if (sb) set_info(sb->title, "");
    }
    return -1;
}

// Equipment rows: weapons, drones, augments, cargo.
#define EQ_ROW_H 46
#define EQ_CELL_W 77
static const char* equip_name(const Run* r, const Ship* s, int row, int col) {
    switch (row) {
        case 0: return col < s->weaponCount ? s->weapons[col].def->name : "";
        case 1: return col < s->droneCount ? s->drones[col].def->name : "";
        case 2: return col < r->augCount ? r->augs[col] : "";
        default: return col < CARGO_SLOTS ? r->cargo[col] : "";
    }
}

static int equip_slots(const Ship* s, int row) {
    return row == 0 ? s->weaponSlots : row == 1 ? s->droneSlots : row == 2 ? MAX_AUGS : CARGO_SLOTS;
}

static void draw_equipment(const Run* r, const Ship* s) {
    static const char* const rows[4] = {"WEAPONS", "DRONES", "AUGMENTS", "CARGO"};
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(150, 160, 175, 255);
    for (int row = 0; row < 4; row++) {
        float y = SHOP_TOP + row * EQ_ROW_H;
        draw_text(6, y, 0.36f, dim, rows[row]);
        for (int col = 0; col < equip_slots(s, row); col++) {
            float x = 4 + col * (EQ_CELL_W + 2);
            bool sel = sEquipSel == row * 8 + col;
            ftl_box(x, y + 12, EQ_CELL_W, 31, sel ? C2D_Color32(90, 80, 30, 255) : UI_BODY);
            const char* name = equip_name(r, s, row, col);
            if (name[0]) {
                // Weapons and drones use FTL's short names (e.g. "Burst II") on one line;
                // augments have only full titles, so those wrap.
                const WeaponDef* w = weapon_find(name);
                const DroneDef* dr = w ? NULL : drone_find(name);
                const char* shortName = w && w->shortName[0] ? w->shortName : dr && dr->shortName[0] ? dr->shortName : NULL;
                if (shortName) {
                    draw_text_centred(x + 3, y + 20, EQ_CELL_W - 6, 0.45f, white, shortName);
                } else {
                    static char wrapped[80];
                    float sc = 0.4f;
                    int lines = wrap_text(item_title_of(name), sc, EQ_CELL_W - 10, wrapped, sizeof(wrapped));
                    if (lines > 2) {
                        sc = 0.34f;
                        lines = wrap_text(item_title_of(name), sc, EQ_CELL_W - 10, wrapped, sizeof(wrapped));
                    }
                    draw_text(x + 5, y + (lines > 1 ? 14 : 20), sc, white, wrapped);
                }
            }
        }
    }
    if (sFighting)
        draw_text(4, BOT_H - 14, 0.34f, C2D_Color32(255, 120, 90, 255), "Equipment can't be changed during combat.");
    else
        draw_text(4, BOT_H - 14, 0.34f, dim, "Tap an item, then a slot or the cargo hold to move it.");
}

static int equip_cell_at(const Ship* s, float px, float py) {
    if (py < SHOP_TOP) return -1;
    int row = (int)((py - SHOP_TOP) / EQ_ROW_H), col = (int)((px - 4) / (EQ_CELL_W + 2));
    if (row > 3 || col < 0 || col >= equip_slots(s, row)) return -1;
    return row * 8 + col;
}

// Moves or swaps the selected item with the tapped cell. Weapons and drones go between their
// own slots and the cargo hold. Frees textures, so it waits for the GPU first.
static bool equip_move(Run* r, Ship* s, FtlDat* dat, const AnimDb* db, int from, int to) {
    int fr = from / 8, fc = from % 8, tr = to / 8, tc = to % 8;
    const char* a = equip_name(r, s, fr, fc);
    const char* b = equip_name(r, s, tr, tc);
    char an[32], bn[32];
    snprintf(an, sizeof(an), "%s", a);
    snprintf(bn, sizeof(bn), "%s", b);
    if (fr == 2 || tr == 2 || !an[0]) return false;  // augments stay put
    bool aWeapon = weapon_find(an) != NULL, bWeapon = bn[0] && weapon_find(bn) != NULL;
    // Where each item may go: weapons row 0 or cargo; drones row 1 or cargo.
    if ((tr == 0 && !aWeapon) || (tr == 1 && aWeapon) || (fr == 0 && bn[0] && !bWeapon) || (fr == 1 && bn[0] && bWeapon))
        return false;
    if (fr == tr && fr == 3) {  // within the hold
        memcpy(r->cargo[fc], bn, sizeof(bn));
        memcpy(r->cargo[tc], an, sizeof(an));
        return true;
    }
    if (fr == tr && fr == 0) {  // reorder weapons
        if (tc >= s->weaponCount) return false;
        WeaponSlot t = s->weapons[fc];
        s->weapons[fc] = s->weapons[tc];
        s->weapons[tc] = t;
        return true;
    }
    if (fr == tr) return false;
    if (fr == 1 && (s->drones[fc].deployed || s->drones[fc].powered)) return false;
    if (tr == 1 && tc < s->droneCount && (s->drones[tc].deployed || s->drones[tc].powered)) return false;
    if (tr == 1 && ship_find_system(s, SYS_DRONES) < 0) return false;
    gpu_idle();
    // Take both out, then put each where the other was.
    if (fr == 0) combat_remove_weapon(s, fc);
    if (fr == 1) combat_remove_drone(s, fc);
    if (fr == 3) r->cargo[fc][0] = '\0';
    if (tr == 0 && tc < s->weaponCount) combat_remove_weapon(s, tc);
    if (tr == 1 && tc < s->droneCount) combat_remove_drone(s, tc);
    if (tr == 3) r->cargo[tc][0] = '\0';
    if (tr == 0) combat_add_weapon(s, dat, db, an);
    if (tr == 1) combat_add_drone(s, dat, db, an);
    if (tr == 3) snprintf(r->cargo[tc], sizeof(r->cargo[tc]), "%s", an);
    if (bn[0]) {
        if (fr == 0) combat_add_weapon(s, dat, db, bn);
        if (fr == 1) combat_add_drone(s, dat, db, bn);
        if (fr == 3) snprintf(r->cargo[fc], sizeof(r->cargo[fc]), "%s", bn);
    }
    return true;
}

static void draw_ship_screen(const Run* r, const Ship* s) {
    static const char* const tabs[2] = {"UPGRADES", "EQUIPMENT"};
    shop_header(r, "SHIP", "CLOSE", tabs, 2, sShipTab);
    if (sShipTab == 0)
        draw_upgrades(r, s);
    else
        draw_equipment(r, s);
}

// Linear memory a ship's art holds: hull, floor, shields, doors, gibs, cloak, systems, weapons, drones.
static unsigned ship_texture_bytes(const Ship* s) {
    if (!s) return 0;
    unsigned b = texture_bytes(&s->hull) + texture_bytes(&s->floor) + texture_bytes(&s->shieldImage) +
                 texture_bytes(&s->doorSheet) + texture_bytes(&s->cloakImage);
    for (int i = 0; i < s->gibCount; i++) b += texture_bytes(&s->gibs[i].tex);
    for (int i = 0; i < s->systemCount; i++)
        b += texture_bytes(&s->systems[i].interior) + texture_bytes(&s->systems[i].icon) +
             texture_bytes(&s->systems[i].panelOn) + texture_bytes(&s->systems[i].panelOff);
    for (int i = 0; i < s->weaponCount; i++) b += texture_bytes(&s->weapons[i].art) + texture_bytes(&s->weapons[i].proj);
    for (int i = 0; i < s->droneCount; i++)
        b += texture_bytes(&s->drones[i].base) + texture_bytes(&s->drones[i].on) + texture_bytes(&s->drones[i].charged) +
             texture_bytes(&s->drones[i].proj);
    return b;
}

static unsigned sMinLinearFree = 0xFFFFFFFFu;  // lowest seen this session
static bool sShowMemory;                        // test.txt "memory 1"

// Memory readout on the top screen while the debug menu is open: the 3DS's two heaps (normal and
// the GPU/DSP-visible linear one), what textures and sounds hold, and each ship's share.
static void draw_memory_top(const Ship* player, const Ship* enemy) {
    struct mallinfo mi = mallinfo();
    unsigned linTotal = envGetLinearHeapSize(), linFree = linearSpaceFree();
    unsigned texB, texN, sndLin, sndHeap;
    int sndN;
    texture_stats(&texB, &texN);
    sound_stats(&sndLin, &sndHeap, &sndN);
    const float MB = 1024.0f * 1024.0f;
    char lines[8][64];
    snprintf(lines[0], 64, "Heap %.1f / %.1f MB used", mi.uordblks / MB, envGetHeapSize() / MB);
    snprintf(lines[1], 64, "Linear %.1f / %.1f MB used", (linTotal - linFree) / MB, linTotal / MB);
    snprintf(lines[2], 64, "Linear free low point %.1f MB", sMinLinearFree / MB);
    snprintf(lines[3], 64, "Textures %u: %.1f MB", texN, texB / MB);
    snprintf(lines[4], 64, "  our ship %.1f MB, enemy %.1f MB", ship_texture_bytes(player) / MB, ship_texture_bytes(enemy) / MB);
    snprintf(lines[5], 64, "Sounds %d: %.1f MB linear, %.1f heap", sndN, sndLin / MB, sndHeap / MB);
    snprintf(lines[6], 64, "VRAM free %.1f MB", vramSpaceFree() / MB);
    C2D_DrawRectSolid(TOP_W - 214, TOP_H - 100, 0, 212, 98, C2D_Color32(10, 14, 24, 230));
    for (int i = 0; i < 7; i++) draw_text(TOP_W - 210, TOP_H - 98 + i * 13, 0.38f, C2D_Color32(235, 235, 235, 255), lines[i]);
}

static void draw_debug(const char* shipName, const char* enemyName, const Combat* c) {
    const u32 dim = C2D_Color32(150, 160, 175, 255);
    C2D_DrawRectSolid(0, TAB_H, 0, BOT_W, BOT_H - TAB_H, C2D_Color32(14, 18, 30, 255));
    const char* labels[2] = {shipName, enemyName};
    const char* heads[2] = {"SHIP", "ENEMY"};
    for (int r = 0; r < 2; r++) {
        float y = r ? DBG_ROW2 : DBG_ROW1;
        draw_text(4, y + 3, 0.4f, dim, heads[r]);
        dbg_button(48, y, 22, 18, false, "<");
        C2D_DrawRectSolid(72, y, 0, BOT_W - 72 - 26, 18, C2D_Color32(24, 30, 44, 255));
        draw_text_fit_left(76, y + 3, 0.42f, BOT_W - 72 - 34, C2D_Color32(235, 235, 235, 255), labels[r]);
        dbg_button(BOT_W - 24, y, 22, 18, false, ">");
    }
    dbg_button(4, DBG_START_Y, BOT_W - 8, DBG_BTN_H, true, "START THIS FIGHT");
    static const char* const names[DBG_COUNT] = {
        "Destroy enemy", "Kill enemy crew", "Fire + breach on us", "Repair + heal all",
        "Enemy boards us", "We board (2 crew)", "+Resources/power", "Enemy hold fire",
        "Clear our cooldowns", "Break enemy systems", "Full sensors", "New run (this ship)"};
    for (int i = 0; i < DBG_COUNT; i++) {
        float x = 4 + (i % 2) * (BOT_W / 2 - 2), y = DBG_GRID_Y + (i / 2) * (DBG_BTN_H + 4);
        bool on = (i == DBG_HOLD_FIRE && c->enemyHoldFire) || (i == DBG_SENSORS && sDebugSensors);
        dbg_button(x, y, BOT_W / 2 - 6, DBG_BTN_H, on, names[i]);
    }
}

static int debug_touch(int px, int py) {
    for (int r = 0; r < 2; r++) {
        float y = r ? DBG_ROW2 : DBG_ROW1;
        if (py < y || py >= y + 18) continue;
        if (px >= 48 && px < 70) return r ? DBG_ENEMY_PREV : DBG_SHIP_PREV;
        if (px >= BOT_W - 24) return r ? DBG_ENEMY_NEXT : DBG_SHIP_NEXT;
    }
    if (py >= DBG_START_Y && py < DBG_START_Y + DBG_BTN_H) return DBG_START;
    if (py >= DBG_GRID_Y) {
        int row = (int)((py - DBG_GRID_Y) / (DBG_BTN_H + 4)), col = px >= BOT_W / 2 ? 1 : 0;
        int i = row * 2 + col;
        if (i < DBG_COUNT) return i;
    }
    return -1;
}

// ---- Main --------------------------------------------------------------------

// A ship and its crew. Textures are big (a hull alone can be 4 MB), so only one ship per side is
// kept in memory: loading waits for the GPU to finish the last frame, frees the old ship, then
// loads the new one.
typedef struct {
    Ship ships[1];
    CrewSet crews[1];
    int cur;  // always 0
    bool loaded;
} ShipSlots;

// Returns once the GPU has finished every frame submitted so far, so textures can be freed.
static void gpu_idle(void) {
    C3D_FrameBegin(0);
    C3D_FrameEnd(0);
}

static bool load_into(ShipSlots* sl, FtlDat* dat, const AnimDb* db, const NameList* names, const ShipBlueprint* bp,
                      char* err, size_t errlen) {
    if (!ship_load(&sl->ships[0], dat, bp, err, errlen)) return false;
    combat_arm(&sl->ships[0], dat, db);
    crew_spawn(&sl->crews[0], &sl->ships[0], dat, db, names);
    sl->loaded = true;
    return true;
}

static bool slots_load(ShipSlots* sl, FtlDat* dat, const AnimDb* db, const NameList* names, const ShipBlueprint* bp,
                       char* err, size_t errlen) {
    const ShipBlueprint* old = sl->loaded ? sl->ships[0].bp : NULL;
    if (sl->loaded) {
        gpu_idle();
        ship_free(&sl->ships[0]);
        crew_free(&sl->crews[0]);
        sl->loaded = false;
    }
    if (load_into(sl, dat, db, names, bp, err, errlen)) return true;
    if (old) load_into(sl, dat, db, names, old, err, errlen);  // keep playing the previous ship
    return false;
}

static void slots_free(ShipSlots* sl) {
    for (int i = 0; i < 1; i++) {
        ship_free(&sl->ships[i]);
        crew_free(&sl->crews[i]);
        texture_free(&sl->crews[i].destMarker);
    }
    sl->loaded = false;
}

// Our crew aboard an enemy that's gone (destroyed, escaped, or we jumped away) are lost; the clone
// bay can bring them back. Boarding drones are lost too.
static int sBossCrew;           // the flagship's crew outside its weapon rooms, kept up to date
static int sBossArtCrew[4];     // whether each weapon room (ARTILLERY_BOSS_1-4) still has its man
static bool sBossAutomated;     // its crew are all dead: the AI has taken over (shown once)

static bool tile_taken(const CrewSet* cs, int x, int y) {
    for (int k = 0; k < cs->count; k++)
        if (cs->crew[k].destX == x && cs->crew[k].destY == y) return true;
    return false;
}

static bool add_to_room(CrewSet* cs, const Ship* s, int race, const NameList* names, int room) {
    const Room* r = &s->rooms[room];
    for (int y = r->y; y < r->y + r->h; y++)
        for (int x = r->x; x < r->x + r->w; x++) {
            if (tile_taken(cs, x, y)) continue;
            int pick = names && names->count ? rand() % names->count : -1;
            return crew_add(cs, race, pick >= 0 ? names->names[pick] : "Rebel", pick >= 0 && names->female[pick], x, y);
        }
    return false;
}

static int artillery_weapon_no(const Ship* s, int k) {
    const char* n = s->artillery[k].def ? s->artillery[k].def->name : "";
    size_t l = strlen(n);
    return l && n[l - 1] >= '1' && n[l - 1] <= '4' && strstr(n, "ARTILLERY_BOSS_") ? n[l - 1] - '1' : -1;
}

static void boss_crew(const Ship* s, CrewSet* cs, const NameList* names, int mainCrew, const int artOk[4]) {
    int race = crew_race(cs, "human");
    if (race < 0) return;
    bool artRoom[MAX_ROOMS] = {false};
    for (int k = 0; k < s->artilleryCount; k++) {
        int room = s->systems[s->artSys[k]].room, w = artillery_weapon_no(s, k);
        artRoom[room] = true;
        if (w >= 0 && artOk[w]) add_to_room(cs, s, race, names, room);
    }
    // The rest at the stations first (pilot, shields, engines...), then anywhere.
    int placed = 0;
    for (int i = 0; i < s->systemCount && placed < mainCrew; i++)
        if (!artRoom[s->systems[i].room] && add_to_room(cs, s, race, names, s->systems[i].room)) placed++;
    for (int tries = 0; tries < 64 && placed < mainCrew; tries++) {
        int room = rand() % s->roomCount;
        if (!artRoom[room] && add_to_room(cs, s, race, names, room)) placed++;
    }
}

// Who's still alive aboard the flagship (while its fight goes on), for the next stage.
static void boss_crew_count(const Ship* s, const CrewSet* cs) {
    if (!s || s->hullHp <= 0) return;
    int onBoard = 0, art[4] = {0, 0, 0, 0};
    for (int i = 0; i < cs->count; i++) {
        const Crew* m = &cs->crew[i];
        if (m->foreign || m->drone) continue;
        int room = ship_room_at(s, m->destX, m->destY), w = -1;
        for (int k = 0; k < s->artilleryCount; k++)
            if (s->systems[s->artSys[k]].room == room) w = artillery_weapon_no(s, k);
        if (w >= 0) art[w] = 1;
        else onBoard++;
    }
    sBossCrew = onBoard;
    for (int k = 0; k < 4; k++) sBossArtCrew[k] = art[k];
}


// ---- Achievements and ship unlocks --------------------------------------------------------------
// Checked every frame of a run; conditions from the FTL wiki (Achievements, Ship Achievements),
// unlock rules in profile.h. Combat and life code record what happened in gFeats (feats.h).

// The enemy ship loaded during the jump's white flash, for the beacon's event (see the jump code):
// the fight starts with it in place, instead of stalling to load when the dialog closes.
static const void* sPreloadFor;  // the event's ship definition it was built for (NULL: none)
// A ship that's here but not fighting (as in FTL): an event's ship while its dialog is up, one that
// surrendered or made peace, or one whose crew we killed. Drawn in the enemy panel until we jump.
static bool sVisitor;

static int sAchFam = -1, sAchLayout;  // the run's ship family and layout
static bool sAchWasWon, sAchWasCloaked, sAchShieldBroken;
static int sAchPrevResult;
static int sAchScrap = -1, sAchHull = -1;
static int sAchFightHull;       // our hull lost this fight, from anything
static float sAchFightClock;    // gFeats.fight.clock last frame (it restarts with each fight)
static int sAchHome, sAchAway;  // our crew aboard our ship / theirs, last frame

static void ach(const char* id) { profile_award(id, sRun.difficulty); }

static void ach_reset(void) {
    sAchWasWon = sAchWasCloaked = sAchShieldBroken = false;
    sAchPrevResult = COMBAT_FIGHTING;
    sAchScrap = sAchHull = -1;
    sAchFightHull = 0;
}

// Our crew (not drones), wherever they are: on our ship (home) and aboard the enemy (away).
static void ach_crew_where(const CrewSet* pc, const CrewSet* ec, int* home, int* away) {
    *home = *away = 0;
    for (int i = 0; i < pc->count; i++)
        *home += !pc->crew[i].drone && pc->crew[i].foreign == (pc->crew[i].mind > 0.0f);
    for (int i = 0; ec && i < ec->count; i++)
        *away += !ec->crew[i].drone && ec->crew[i].foreign != (ec->crew[i].mind > 0.0f);
}

static void ach_fight_won(const Combat* c, const Ship* ps, const Ship* es) {
    const int f = sAchFam;
    bool destroyed = !c->crewKilled;
    const int* h = gFeats.fight.enemyHull;
    profile_add_ship_kill();
    // They never saw it coming: the Pre-Igniter's first volley, before they fired at all.
    if (destroyed && ps->aug[AUG_PREIGNITE] && gFeats.fight.enemyShots == 0) {
        bool once = true, any = false;
        for (int i = 0; i < 4; i++) {
            once &= gFeats.fight.weaponFires[i] <= 1;
            any |= gFeats.fight.weaponFires[i] > 0;
        }
        if (once && any) ach("ACH_ONE_VOLLEY");
    }
    if (sAchHome == 0 && sAchAway > 0) ach("ACH_INVADE_SHIP");  // everyone aboard them
    if (f == FAM_STEALTH && destroyed && ps->cloakTime > 0.0f && gFeats.fight.cloakFullHull) ach("ACH_STEALTH_DESTROY");
    if (f == FAM_ENGI && destroyed && h[SRC_DRONE] > 0 && h[SRC_WEAPON] + h[SRC_MISSILE] + h[SRC_ARTILLERY] + h[SRC_SHARD] == 0)
        ach("ACH_ONLY_DRONES");
    if (f == FAM_FED && destroyed && h[SRC_ARTILLERY] > 0 && h[SRC_WEAPON] + h[SRC_MISSILE] + h[SRC_DRONE] + h[SRC_SHARD] == 0 &&
        sAchFightHull == 0)
        ach("ACH_FED_PATIENCE");
    if (f == FAM_ROCK && destroyed && gFeats.fight.enemyDefenseDrone && h[SRC_MISSILE] > 0 &&
        h[SRC_WEAPON] + h[SRC_DRONE] + h[SRC_ARTILLERY] + h[SRC_SHARD] == 0)
        ach("ACH_ROCK_MISSILES");
    if (f == FAM_CRYSTAL && destroyed && gFeats.fight.lastEnemyHullSrc == SRC_SHARD) ach("ACH_CRYSTAL_SHARD");
    if (f == FAM_ZOLTAN && destroyed && ps->superShieldMax > 0 && !sAchShieldBroken) ach("ACH_ENERGY_SHIELDS");
    if (f == FAM_MANTIS && gFeats.fight.enemyCrewKilled >= 5 && sAchFightHull == 0 && gFeats.fight.ourCrewLost == 0)
        ach("ACH_MANTIS_SLAUGHTER");
    if (f == FAM_MANTIS && c->crewKilled && sAchHome == 0 && sAchAway == 1) ach("ACH_MANTIS_SURVIVOR");
    if (c->crewKilled) sRun.feats.crewKillWins++;
    if (destroyed && es && strstr(es->bp->name, "ROCK")) sRun.feats.rockShipsDestroyed++;
}

static void check_achievements(const Combat* c, Ship* ps, CrewSet* pc, const Ship* es, const CrewSet* ec) {
    if (!sInRun || sRun.sandbox) return;  // sandbox runs earn nothing
    sAchFam = profile_family_of(ps->bp->name, &sAchLayout);
    const int f = sAchFam;
#define F sRun.feats
    // What happened since last frame.
    if (gFeats.shotsFired) F.firedShot = true;
    if (gFeats.missileShots) F.usedMissiles = true;
    if (gFeats.dronesUsed) F.usedDrones = true;
    if (gFeats.offensiveDrones) F.offensive = true;
    if (gFeats.teleports) F.teleported = true;
    if (gFeats.ourDeaths) F.crewDied = true;
    gFeats.shotsFired = gFeats.missileShots = gFeats.dronesUsed = gFeats.offensiveDrones = gFeats.teleports = gFeats.ourDeaths = 0;
    if (sAchScrap >= 0 && sRun.scrap > sAchScrap) profile_add_scrap(sRun.scrap - sAchScrap);  // Rule Ten
    sAchScrap = sRun.scrap;
    if (gFeats.fight.clock < sAchFightClock) {  // a new fight
        sAchFightHull = 0;
        sAchShieldBroken = false;
    }
    sAchFightClock = gFeats.fight.clock;
    if (sAchHull >= 0 && ps->hullHp < sAchHull) sAchFightHull += sAchHull - ps->hullHp;
    sAchHull = ps->hullHp;
    if (ps->superShieldMax > 0 && ps->superShield <= 0) sAchShieldBroken = true;

    // Arriving in sector 5 / sector 8.
    if (sRun.sector != F.lastSector) {
        F.lastSector = sRun.sector;
        if (sRun.sector == 4) {
            ach("ACH_SECTOR_5");
            if (!F.upgradedSystem && !F.upgradedReactor) ach("ACH_NO_UPGRADES");
            if (!F.firedShot && !F.offensive && !F.teleported) ach("ACH_PACIFIST");
            if (!F.storeRepair) ach("ACH_NO_REPAIR");
            if (f == FAM_FED && !F.upgradedWeapons) ach("ACH_FED_UPGRADE");
            if (f == FAM_ZOLTAN && !F.upgradedReactor) ach("ACH_ENERGY_MANPOWER");
            if (f == FAM_KESTREL) profile_unlock(FAM_ENGI, 0);
        }
        if (sRun.sector == 7) {
            ach("ACH_SECTOR_8");
            if (!F.usedMissiles) ach("ACH_NO_MISSILES");
            if (!F.usedDrones) ach("ACH_NO_DRONES");
            if (!F.storeBuy) ach("ACH_NO_BUYING");
            if (!F.crewDied) ach("ACH_NO_DEATH");
            if (f == FAM_STEALTH && !F.dangerJump) ach("ACH_STEALTH_TACTICAL");
            if (f == FAM_LANIUS && !F.oxygenOver) ach("ACH_LANIUS_OXYGEN");
            if (sAchLayout == 1) profile_unlock(f, 2);
        }
    }

    // Our ship and crew.
    if (f == FAM_KESTREL) {
        const char* seen[8];
        int kinds = 0;
        for (int i = 0; i < pc->count; i++) {
            const Crew* m = &pc->crew[i];
            const char* r = pc->races[m->race].name;
            if (m->foreign || m->drone || !strcmp(r, "human")) continue;
            bool dup = false;
            for (int k = 0; k < kinds; k++) dup |= !strcmp(seen[k], r);
            if (!dup && kinds < 8) seen[kinds++] = r;
        }
        if (kinds >= 6) ach("ACH_UNITED_FEDERATION");
        if (ps->systemCount >= 11) ach("ACH_FULL_ARSENAL");
        if (ps->hullHp == 1) F.hullAtOne = true;
        if (F.hullAtOne && ps->hullHp >= ps->maxHull) ach("ACH_TOUGH_SHIP");
    }
    if (f == FAM_ENGI) {
        int on = 0;
        for (int i = 0; i < ps->droneCount; i++) on += ps->drones[i].powered && ps->drones[i].deployed;
        if (on >= 3) ach("ACH_ROBOTIC");
    }
    if (f == FAM_ZOLTAN) {
        int power = 0;
        for (int i = 0; i < ps->systemCount; i++)
            if (!system_is_subsystem(ps->systems[i].def->type)) power += ship_system_power(ps, i);
        if (power >= 29) ach("ACH_ENERGY_POWER");
    }
    if (f == FAM_LANIUS && sRun.scrap >= 600) ach("ACH_LANIUS_SCRAP");
    if (f == FAM_LANIUS) {  // net oxygen over 20%, from the first jump on
        int visited = 0;
        for (int i = 0; i < sRun.beaconCount; i++) visited += sRun.beacons[i].visited;
        if ((sRun.sector > 0 || visited > 1) && life_ship_oxygen(ps) > 20.0f) F.oxygenOver = true;
    }
    if (f == FAM_FED && F.blueCrewUses >= 4 && sRun.sector <= 4) ach("ACH_FED_DIPLOMACY");
    if (f == FAM_MANTIS && F.crewKillWins >= 20 && sRun.sector <= 5) ach("ACH_MANTIS_CREW_DEAD");
    if (f == FAM_CRYSTAL && F.rockShipsDestroyed >= 10) ach("ACH_CRYSTAL_CLASH");
    if (f == FAM_SLUG && F.nebulaJumps >= 30 && sRun.sector < 7) ach("ACH_SLUG_NEBULA");

    // The cloak going up starts Phase Shift's count and Bird of Prey's watch.
    if (ps->cloakTime > 0.0f && !sAchWasCloaked) {
        gFeats.fight.cloakAvoided = 0.0f;
        gFeats.fight.cloakFullHull = es && es->hullHp >= es->maxHull;
    }
    sAchWasCloaked = ps->cloakTime > 0.0f;
    if (f == FAM_STEALTH && ps->cloakTime > 0.0f && gFeats.fight.cloakAvoided >= 9.0f) ach("ACH_STEALTH_AVOID");

    // The fight.
    bool fighting = es && c->result == COMBAT_FIGHTING;
    if (fighting) {
        ach_crew_where(pc, ec, &sAchHome, &sAchAway);
        if (gFeats.fight.hitStreak >= 5) ach("ACH_BAD_DODGING");
        for (int i = 0; i < 4; i++)
            if (gFeats.fight.boarderKills[i] >= 4) ach("ACH_BOARDING_DRONE");
        if (gFeats.fight.allRoomsBeamed) ach("ACH_SLICE_DICE");
        if (es->bp->crewKinds > 0 && life_ship_oxygen(es) < 5.0f) ach("ACH_SUFFOCATE");
        bool allBurning = es->roomCount > 0;
        for (int r = 0; r < es->roomCount && allBurning; r++) {
            const Room* rm = &es->rooms[r];
            for (int y = rm->y; y < rm->y + rm->h && allBurning; y++)
                for (int x = rm->x; x < rm->x + rm->w && allBurning; x++) allBurning = es->fire[y][x] > 0.0f;
        }
        if (allBurning) ach("ACH_BURNING");
        if (f == FAM_ENGI) {
            int ioned = 0;
            for (int i = 0; i < es->systemCount; i++) ioned += es->systems[i].ionTimer > 0.0f && es->systems[i].ionLimit >= 0;
            if (ioned >= 4) ach("ACH_IONED");
        }
        if (f == FAM_SLUG) {
            int si = ship_find_system(ps, SYS_SENSORS);
            bool sensors = si >= 0 && ship_system_power(ps, si) > 0;
            bool all = es->roomCount > 0;
            for (int r = 0; r < es->roomCount && all; r++) all = !room_hidden(es, r);
            if (!sensors && all) ach("ACH_SLUG_VISION");
            if (gFeats.fight.bioKills >= 3) ach("ACH_SLUG_BIO");
        }
        if (f == FAM_ROCK && gFeats.fight.burningKill) ach("ACH_ROCK_FIRE");
        if (f == FAM_LANIUS && es->hackPulse > 0.0f && ps->batteryTime > 0.0f) {
            bool mind = false;
            for (int i = 0; ec && i < ec->count && !mind; i++) mind = ec->crew[i].mind > 0.0f && ec->crew[i].foreign;
            for (int i = 0; i < pc->count && !mind; i++) mind = pc->crew[i].mind > 0.0f && !pc->crew[i].foreign;
            if (mind) ach("ACH_LANIUS_ADVANCED");
        }
        if (f == FAM_CRYSTAL) {  // No Escape: four of theirs trapped in one locked-down room
            for (int side = 0; side < 2; side++) {
                const Ship* s = side ? es : ps;
                const CrewSet* cs = side ? ec : pc;
                for (int r = 0; cs && r < s->roomCount; r++) {
                    if (s->lockdown[r] <= 0.0f) continue;
                    int n = 0;
                    for (int i = 0; i < cs->count; i++) {
                        const Crew* m = &cs->crew[i];
                        bool theirs = side ? !m->foreign : m->foreign;
                        n += theirs && !m->drone && ship_room_at(s, (int)floorf(m->x / TILE), (int)floorf(m->y / TILE)) == r;
                    }
                    if (n >= 4) ach("ACH_CRYSTAL_LOCKDOWN");
                }
            }
        }
    }
    if (c->result == COMBAT_WON && sAchPrevResult == COMBAT_FIGHTING && es) ach_fight_won(c, ps, es);
    sAchPrevResult = c->result;

    // The run won: the victory for this layout, and what it unlocks.
    bool won = sRun.over && sRun.won;
    if (won && !sAchWasWon && f >= 0) profile_record_victory(f, sAchLayout, sRun.difficulty);
    sAchWasWon = won;
#undef F
}

// After a jump: Tactical Approach (no environmental danger) and Home Sweet Home (nebulas).
static void ach_arrived(void) {
    if (env_dangerous(sRun.env)) sRun.feats.dangerJump = true;
    if ((sRun.env == ENV_NEBULA || sRun.env == ENV_STORM) && sRun.sector < 7) sRun.feats.nebulaJumps++;
}

// ---- Popups: achievements earned and ships unlocked, one at a time on the top screen ----
static ProfilePopup sPopup;
static float sPopupTime;
static Texture sPopupIcon;

static int bp_of_family(const ShipBlueprint* bps, int count, int family, int layout) {
    for (int i = 0; i < count; i++) {
        int l, fam = profile_family_of(bps[i].name, &l);
        if (fam == family && l == layout) return i;
    }
    return -1;
}

// Outside a frame: it may free and load the icon.
static void popup_update(float dt, const ShipBlueprint* bps, int count) {
    if (sPopupTime > 0.0f) {
        sPopupTime -= dt;
        return;
    }
    if (!profile_next_popup(&sPopup)) return;
    gpu_idle();
    texture_free(&sPopupIcon);
    char path[96] = "";
    if (sPopup.ach >= 0) {
        snprintf(path, sizeof(path), "%s", profile_ach(sPopup.ach)->img);
    } else {
        int bi = bp_of_family(bps, count, sPopup.family, sPopup.layout);
        if (bi >= 0) snprintf(path, sizeof(path), "img/customizeUI/miniship_%s.png", bps[bi].img);
    }
    if (path[0]) {
        load_crop(&sPopupIcon, path, 0, 0, 0, 0, false);
        texture_set_smooth(&sPopupIcon);
    }
    sPopupTime = 4.0f;
}

static void draw_popup(const ShipBlueprint* bps, int count) {
    if (sPopupTime <= 0.0f) return;
    const float w = 236, h = 46, x = TOP_W - w - 4, y = TOP_H - h - 4;
    ftl_box(x, y, w, h, C2D_Color32(20, 26, 38, 235));
    if (sPopupIcon.loaded) {
        float s = 38.0f / (sPopupIcon.width > sPopupIcon.height ? sPopupIcon.width : sPopupIcon.height);
        C2D_DrawImageAt(sPopupIcon.image, x + 4 + (38 - sPopupIcon.width * s) * 0.5f, y + 4 + (38 - sPopupIcon.height * s) * 0.5f,
                        0, NULL, s, s);
    }
    char key[48], line[64];
    const char* title;
    if (sPopup.ach >= 0) {
        snprintf(key, sizeof(key), "%s_name", profile_ach(sPopup.ach)->id);
        title = text_get(key, profile_ach(sPopup.ach)->id);
        draw_text(x + 48, y + 4, 0.4f, C2D_Color32(255, 210, 90, 255), "ACHIEVEMENT");
    } else {
        int bi = bp_of_family(bps, count, sPopup.family, sPopup.layout);
        snprintf(line, sizeof(line), "%s Type %c", bi >= 0 ? text_get(bps[bi].classId, bps[bi].name) : "Ship", 'A' + sPopup.layout);
        title = line;
        draw_text(x + 48, y + 4, 0.4f, C2D_Color32(120, 230, 140, 255), "SHIP UNLOCKED");
    }
    draw_text_fit_left(x + 48, y + 22, 0.45f, w - 54, C2D_Color32(235, 235, 235, 255), title);
}

// ---- The achievements screen (STATS on the main menu) ----
static Texture sAchIcons[64];
static bool sAchIconsLoaded;
static int sAchSel, sAchScroll;
#define ACH_ROW_H 28
#define ACH_LIST_Y 22
#define ACH_ROWS ((BOT_H - ACH_LIST_Y - 30) / ACH_ROW_H)

static void ach_icons(bool want) {
    if (want == sAchIconsLoaded) return;
    gpu_idle();
    for (int i = 0; i < 64; i++) texture_free(&sAchIcons[i]);
    sAchIconsLoaded = want;
    for (int i = 0; want && i < profile_ach_count() && i < 64; i++) {
        load_crop(&sAchIcons[i], profile_ach(i)->img, 0, 0, 0, 0, false);
        texture_set_smooth(&sAchIcons[i]);
    }
}

static const char* const FAMILY_NAMES[SHIP_FAMILIES] = {"Kestrel", "Stealth", "Mantis", "Engi", "Federation",
                                                       "Slug", "Rock", "Zoltan", "Crystal", "Lanius"};

static void draw_ach_icon(int i, float x, float y, float size) {
    const Texture* t = &sAchIcons[i];
    if (!t->loaded) return;
    float s = size / (t->width > t->height ? t->width : t->height);
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, C2D_Color32(40, 44, 52, 255), profile_ach_level(i) ? 0.0f : 0.75f);  // greyed until earned
    C2D_DrawImageAt(t->image, x, y, 0, &tint, s, s);
}

static void draw_stats_top(void) {
    C2D_DrawRectSolid(0, 0, 0, TOP_W, TOP_H, C2D_Color32(8, 10, 20, 200));
    int earned = 0, ships = 0;
    for (int i = 0; i < profile_ach_count(); i++) earned += profile_ach_level(i) > 0;
    for (int f2 = 0; f2 < SHIP_FAMILIES; f2++) ships += profile_layout_unlocked(f2, 0);
    char buf[96];
    snprintf(buf, sizeof(buf), "Achievements %d / %d     Ships %d / %d", earned, profile_ach_count(), ships, SHIP_FAMILIES);
    draw_text_centred(0, 8, TOP_W, 0.5f, C2D_Color32(235, 235, 235, 255), buf);
    const AchDef* a = profile_ach(sAchSel);
    if (!a) return;
    ftl_box(20, 40, TOP_W - 40, 160, C2D_Color32(20, 26, 38, 240));
    draw_ach_icon(sAchSel, 32, 52, 64);
    char key[48];
    snprintf(key, sizeof(key), "%s_name", a->id);
    draw_text_fit_left(108, 52, 0.55f, TOP_W - 140, C2D_Color32(255, 220, 120, 255), text_get(key, a->id));
    if (a->family >= 0) {
        snprintf(buf, sizeof(buf), "%s Cruiser", FAMILY_NAMES[a->family]);
        draw_text(108, 74, 0.42f, C2D_Color32(150, 170, 200, 255), buf);
    }
    snprintf(key, sizeof(key), "%s_desc", a->id);
    static char wrapped[256];
    wrap_text(text_get(key, ""), 0.45f, TOP_W - 72, wrapped, sizeof(wrapped));
    draw_text(32, 124, 0.45f, C2D_Color32(225, 225, 225, 255), wrapped);
    static const char* const levels[4] = {"Not earned yet", "Earned", "Earned on Normal", "Earned on Hard"};
    int lv = profile_ach_level(sAchSel);
    const char* status = !lv ? levels[0] : !a->multi ? levels[1] : lv == 1 ? "Earned on Easy" : levels[lv > 3 ? 3 : lv];
    draw_text(108, 94, 0.42f, lv ? C2D_Color32(120, 230, 140, 255) : C2D_Color32(150, 160, 175, 255), status);
}

static void draw_stats(void) {
    C2D_DrawRectSolid(0, 0, 0, BOT_W, BOT_H, C2D_Color32(10, 13, 24, 255));
    draw_text(6, 3, 0.45f, C2D_Color32(235, 235, 235, 255), "ACHIEVEMENTS");
    int n = profile_ach_count();
    for (int row = 0; row < ACH_ROWS && sAchScroll + row < n; row++) {
        int i = sAchScroll + row;
        const AchDef* a = profile_ach(i);
        float y = ACH_LIST_Y + row * ACH_ROW_H;
        if (i == sAchSel) C2D_DrawRectSolid(2, y, 0, BOT_W - 4, ACH_ROW_H - 2, SELECT_BG);
        draw_ach_icon(i, 6, y + 1, 24);
        char key[48];
        snprintf(key, sizeof(key), "%s_name", a->id);
        u32 col = profile_ach_level(i) ? C2D_Color32(235, 235, 235, 255) : C2D_Color32(130, 138, 150, 255);
        draw_text_fit_left(36, y + 5, 0.45f, BOT_W - 120, col, text_get(key, a->id));
        if (a->family >= 0) draw_text(BOT_W - 78, y + 6, 0.38f, C2D_Color32(150, 170, 200, 255), FAMILY_NAMES[a->family]);
    }
    ftl_button(4, BOT_H - 28, 96, 24, false, "BACK");
    ftl_button(BOT_W - 108, BOT_H - 28, 48, 24, false, "UP");
    ftl_button(BOT_W - 56, BOT_H - 28, 52, 24, false, "DOWN");
}

static void stats_select(int i) {
    int n = profile_ach_count();
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    sAchSel = i;
    if (sAchSel < sAchScroll) sAchScroll = sAchSel;
    if (sAchSel >= sAchScroll + ACH_ROWS) sAchScroll = sAchSel - ACH_ROWS + 1;
}

// ---- Main menu, hangar, options, pause ---------------------------------------------------------
// FTL's main menu: its title art on the top screen and its own buttons (img/main_menus/*_on,
// _select2 and _off.png) on the touch screen. New Game opens the hangar: ship families with FTL's
// miniship pictures, types A-C, difficulty and START, with the chosen ship large on the top screen.
// Options sets the volumes (kept in sdmc:/3ds/ftl3ds/settings.txt). In a game START pauses, with
// RESUME, OPTIONS, MAIN MENU and QUIT.

enum { MENU_NONE, MENU_MAIN, MENU_HANGAR, MENU_OPTIONS, MENU_PAUSE, MENU_STATS, MENU_LOADOUT };
static int sMenu, sMenuBack;  // the open screen, and where Options goes back to
static int sMenuSel;          // the button the D-pad has picked
static Texture sTitleArt;  // main_base2.png, the logo included
static int sMusicVol = 8, sSfxVol = 8;  // 0-10
static bool sSaveExists;                // CONTINUE is offered
static int sDifficulty = DIFF_NORMAL;   // chosen in the hangar, for the next run

#define SETTINGS_FILE "sdmc:/3ds/ftl3ds/settings.txt"

static void settings_apply(void) { sound_set_volume(sMusicVol / 10.0f, sSfxVol / 10.0f); }

static void settings_load(void) {
    FILE* f = fopen(SETTINGS_FILE, "r");
    if (f) {
        char key[24];
        int v;
        while (fscanf(f, "%23s %d", key, &v) == 2) {
            if (!strcmp(key, "music")) sMusicVol = v;
            else if (!strcmp(key, "sfx")) sSfxVol = v;
            else if (!strcmp(key, "difficulty")) sDifficulty = v;
        }
        fclose(f);
    }
    if (sMusicVol < 0 || sMusicVol > 10) sMusicVol = 8;
    if (sSfxVol < 0 || sSfxVol > 10) sSfxVol = 8;
    if (sDifficulty < DIFF_EASY || sDifficulty > DIFF_HARD) sDifficulty = DIFF_NORMAL;
    settings_apply();
}

static void settings_save(void) {
    FILE* f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "music %d\nsfx %d\ndifficulty %d\n", sMusicVol, sSfxVol, sDifficulty);
    fclose(f);
}

// ---- The main menu's buttons ----
// The tutorial slot holds SANDBOX (this port has no tutorial).
enum { MM_CONTINUE, MM_NEWGAME, MM_SANDBOX, MM_STATS, MM_OPTIONS, MM_CREDITS, MM_QUIT, MM_COUNT };
static Texture sMenuBtn[MM_COUNT][3];  // off, on, selected
#define MM_SCALE 0.55f
#define MM_TOP 12
#define MM_PITCH 31

static bool mm_enabled(int i) {
    return i == MM_NEWGAME || i == MM_SANDBOX || i == MM_OPTIONS || i == MM_QUIT || i == MM_STATS || (i == MM_CONTINUE && sSaveExists);
}

static void menu_art(bool want) {
    if (want == sTitleArt.loaded) return;
    if (!want) {
        gpu_idle();
        texture_free(&sTitleArt);
        for (int i = 0; i < MM_COUNT; i++)
            for (int k = 0; k < 3; k++) texture_free(&sMenuBtn[i][k]);
        return;
    }
    uint32_t size;
    uint8_t* png = ftldat_read(sDat, "img/main_menus/main_base2.png", &size);
    if (png) texture_load_png_fit(&sTitleArt, png, size, TOP_W, TOP_H);
    free(png);
    static const char* const files[MM_COUNT] = {"continue", "start", "tutorial", "stats", "options", "credits", "quit"};
    static const char* const states[3] = {"off", "on", "select2"};
    for (int i = 0; i < MM_COUNT; i++)
        for (int k = 0; k < 3; k++) {
            char path[64];
            snprintf(path, sizeof(path), "img/main_menus/%s_%s.png", files[i], states[k]);
            load_crop(&sMenuBtn[i][k], path, 0, 0, 0, 0, false);
            texture_set_smooth(&sMenuBtn[i][k]);
        }
}

static void draw_title_top(const char* caption) {
    if (sTitleArt.loaded) {
        float s = TOP_W / (float)sTitleArt.width > TOP_H / (float)sTitleArt.height ? TOP_W / (float)sTitleArt.width
                                                                                    : TOP_H / (float)sTitleArt.height;
        C2D_DrawImageAt(sTitleArt.image, (TOP_W - sTitleArt.width * s) * 0.5f, (TOP_H - sTitleArt.height * s) * 0.5f, 0, NULL, s, s);
    }
    if (caption) {
        C2D_DrawRectSolid(0, TOP_H - 24, 0, TOP_W, 24, C2D_Color32(8, 10, 20, 200));
        draw_text_centred(0, TOP_H - 21, TOP_W, 0.45f, C2D_Color32(235, 235, 235, 255), caption);
    }
}

static int main_menu_touch(int px, int py) {
    if (py < MM_TOP) return -1;
    int i = (py - MM_TOP) / MM_PITCH;
    if (i >= MM_COUNT) return -1;
    const Texture* t = &sMenuBtn[i][1];
    float w = t->loaded ? t->width * MM_SCALE : 140;
    return px >= (BOT_W - w) * 0.5f - 10 && px <= (BOT_W + w) * 0.5f + 10 ? i : -1;
}

static void draw_main_menu(void) {
    C2D_DrawRectSolid(0, 0, 0, BOT_W, BOT_H, C2D_Color32(8, 10, 20, 255));
    for (int i = 0; i < MM_COUNT; i++) {
        int state = !mm_enabled(i) ? 0 : i == sMenuSel ? 2 : 1;
        if (i == MM_SANDBOX) {  // FTL has no art for it: its own words, in the buttons' colours
            const Texture* ref = &sMenuBtn[MM_NEWGAME][1];
            float h = ref->loaded ? ref->height * MM_SCALE : 20;
            u32 col = state == 2 ? C2D_Color32(255, 230, 120, 255) : C2D_Color32(225, 230, 235, 255);
            draw_text_centred(0, MM_TOP + i * MM_PITCH + h * 0.5f - 9, BOT_W, 0.7f, col, "SANDBOX");
            continue;
        }
        const Texture* t = &sMenuBtn[i][state];
        if (!t->loaded) continue;
        C2D_DrawImageAt(t->image, floorf((BOT_W - t->width * MM_SCALE) * 0.5f), MM_TOP + i * MM_PITCH, 0, NULL, MM_SCALE, MM_SCALE);
    }
}

// ---- The hangar: ship families, A-C, difficulty ----
#define MAX_FAMILIES 16
static int sFam[MAX_FAMILIES][3];  // blueprint index of each family's type A, B and C (-1: none)
static int sFamCount, sHangarFam, sHangarType;
static Texture sMiniShip;
static int sMiniFor = -1;  // the blueprint sMiniShip shows
// Loading a whole ship (hull, rooms, crew, weapons) takes a moment on a 3DS, so flicking through
// the hangar shows the picture at once and loads the chosen ship when it settles.
#define HANGAR_SETTLE 0.3f
static bool sHangarStarting;  // START pressed: this frame says "Preparing ship", the next loads it

static void build_families(const ShipBlueprint* bps, int count) {
    sFamCount = 0;
    for (int i = 0; i < count; i++) {
        char base[48];
        snprintf(base, sizeof(base), "%.47s", bps[i].name);
        size_t n = strlen(base);
        int type = 0;
        if (n > 2 && base[n - 2] == '_' && (base[n - 1] == '2' || base[n - 1] == '3')) {
            type = base[n - 1] - '1';
            base[n - 2] = '\0';
        }
        int f = -1;
        for (int k = 0; k < sFamCount && f < 0; k++) {
            int any = sFam[k][0] >= 0 ? sFam[k][0] : sFam[k][1] >= 0 ? sFam[k][1] : sFam[k][2];
            char other[48];
            snprintf(other, sizeof(other), "%.47s", bps[any].name);
            size_t m = strlen(other);
            if (m > 2 && other[m - 2] == '_' && (other[m - 1] == '2' || other[m - 1] == '3')) other[m - 2] = '\0';
            if (!strcmp(other, base)) f = k;
        }
        if (f < 0) {
            if (sFamCount >= MAX_FAMILIES) continue;
            f = sFamCount++;
            sFam[f][0] = sFam[f][1] = sFam[f][2] = -1;
        }
        sFam[f][type] = i;
    }
}

// Points the hangar at blueprint `index` (its family and type).
static void hangar_select(int index) {
    for (int f = 0; f < sFamCount; f++)
        for (int t = 0; t < 3; t++)
            if (sFam[f][t] == index) {
                sHangarFam = f;
                sHangarType = t;
            }
}

// Whether the hangar's blueprint bi is unlocked (ships outside FTL's ten are always available).
static bool sSandbox;  // the hangar was opened for a sandbox run: every ship, then a loadout

static bool hangar_unlocked(const ShipBlueprint* bps, int bi) {
    if (sSandbox) return bi >= 0;
    int layout, fam = bi >= 0 ? profile_family_of(bps[bi].name, &layout) : -1;
    return bi >= 0 && (fam < 0 || profile_layout_unlocked(fam, layout));
}

#define HG_MINI_Y 4
#define HG_NAME_Y 100
#define HG_TYPE_Y 122
#define HG_DIFF_Y 156
#define HG_BTN_Y 196
enum { HG_NONE = -1, HG_PREV, HG_NEXT, HG_TYPE_A, HG_TYPE_B, HG_TYPE_C, HG_EASY, HG_NORMAL, HG_HARD, HG_BACK, HG_START };

static float hg_type_x(int t) { return BOT_W * 0.5f - 76 + t * 52; }
static float hg_diff_x(int d) { return 8 + d * 102; }

static int hangar_touch(int px, int py) {
    if (py >= HG_MINI_Y && py < HG_MINI_Y + 92) {
        if (px < 60) return HG_PREV;
        if (px >= BOT_W - 60) return HG_NEXT;
    }
    if (py >= HG_TYPE_Y && py < HG_TYPE_Y + 26)
        for (int t = 0; t < 3; t++)
            if (px >= hg_type_x(t) && px < hg_type_x(t) + 48) return HG_TYPE_A + t;
    if (py >= HG_DIFF_Y && py < HG_DIFF_Y + 28)
        for (int d = 0; d < 3; d++)
            if (px >= hg_diff_x(d) && px < hg_diff_x(d) + 98) return HG_EASY + d;
    if (py >= HG_BTN_Y && py < HG_BTN_Y + 36) {
        if (px < 100) return HG_BACK;
        if (px >= BOT_W - 170) return HG_START;
    }
    return HG_NONE;
}

// FTL's picture of the chosen ship (img/customizeUI/miniship_<img>.png). Outside a frame: it waits for the GPU.
static void hangar_mini(const ShipBlueprint* bps) {
    int bi = sFam[sHangarFam][sHangarType];
    if (bi == sMiniFor) return;
    gpu_idle();
    texture_free(&sMiniShip);
    char path[80];
    snprintf(path, sizeof(path), "img/customizeUI/miniship_%s.png", bps[bi].img);
    load_crop(&sMiniShip, path, 0, 0, 0, 0, false);
    texture_set_smooth(&sMiniShip);
    sMiniFor = bi;
}

static void draw_hangar(const ShipBlueprint* bps) {
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(110, 118, 130, 255);
    C2D_DrawRectSolid(0, 0, 0, BOT_W, BOT_H, C2D_Color32(10, 13, 24, 255));
    int bi = sFam[sHangarFam][sHangarType];
    if (sMiniShip.loaded) {
        float s = 92.0f / sMiniShip.height;
        C2D_DrawImageAt(sMiniShip.image, floorf((BOT_W - sMiniShip.width * s) * 0.5f), HG_MINI_Y, 0, NULL, s, s);
    }
    bool locked = !hangar_unlocked(bps, bi);
    if (locked && sMiniShip.loaded) {  // a dark silhouette, as in FTL's hangar
        float s = 92.0f / sMiniShip.height;
        C2D_ImageTint tint;
        C2D_PlainImageTint(&tint, C2D_Color32(10, 13, 24, 255), 0.8f);
        C2D_DrawImageAt(sMiniShip.image, floorf((BOT_W - sMiniShip.width * s) * 0.5f), HG_MINI_Y, 0, &tint, s, s);
    }
    ftl_button(4, HG_MINI_Y + 30, 52, 32, false, "<");
    ftl_button(BOT_W - 56, HG_MINI_Y + 30, 52, 32, false, ">");
    draw_text_centred(0, HG_NAME_Y, BOT_W, 0.55f, white, text_get(bps[bi].classId, bps[bi].name));
    if (locked) {
        int layout, fam = profile_family_of(bps[bi].name, &layout);
        draw_text_centred(0, HG_MINI_Y + 36, BOT_W, 0.55f, C2D_Color32(255, 120, 100, 255), "LOCKED");
        draw_text_centred(0, HG_DIFF_Y - 18, BOT_W, 0.4f, C2D_Color32(255, 200, 120, 255), profile_unlock_hint(fam, layout));
    }
    static const char* const types[3] = {"A", "B", "C"};
    draw_text(hg_type_x(0) - 44, HG_TYPE_Y + 5, 0.42f, dim, "TYPE");
    for (int t = 0; t < 3; t++) {
        bool have = sFam[sHangarFam][t] >= 0;
        if (have) {
            ftl_button(hg_type_x(t), HG_TYPE_Y, 48, 26, t == sHangarType, types[t]);
            if (!hangar_unlocked(bps, sFam[sHangarFam][t]))
                C2D_DrawRectSolid(hg_type_x(t), HG_TYPE_Y, 0, 48, 26, C2D_Color32(10, 13, 24, 150));  // locked: dimmed
        } else
            C2D_DrawRectSolid(hg_type_x(t), HG_TYPE_Y, 0, 48, 26, C2D_Color32(24, 28, 38, 255));
    }
    static const char* const diffs[3] = {"EASY", "NORMAL", "HARD"};
    for (int d = 0; d < 3; d++) ftl_button(hg_diff_x(d), HG_DIFF_Y, 98, 28, d == sDifficulty, diffs[d]);
    ftl_button(4, HG_BTN_Y, 96, 36, false, "BACK");
    ftl_button(BOT_W - 170, HG_BTN_Y, 166, 36, !locked, locked ? "LOCKED" : "START");
}


// ---- The hangar's preview of the chosen ship -------------------------------------------------
// Loading a whole ship (every room's art, the crew's sprite sheets, weapons...) takes a second or
// more on a 3DS, far too slow for flicking through ships. The hangar only shows a picture: the
// rooms and system icons come straight from the small layout files, and the hull and floor images
// are decoded on the background thread (as backdrops are), with LOADING shown until they arrive.
// The real ship loads only when START is pressed.
static Ship sPreview;          // layout only (no textures of its own)
static int sPreviewFor = -1;   // the blueprint it shows
static Texture sPreviewHull, sPreviewFloor;
static char sPreviewHullName[64], sPreviewFloorName[64];
static ImageJob sPreviewHullJob, sPreviewFloorJob;
static char sPreviewHullWant[64], sPreviewFloorWant[64];

static void hangar_preview_set(const ShipBlueprint* bps, int bi) {
    if (bi == sPreviewFor || bi < 0) return;
    sPreviewFor = bi;
    if (!ship_load_layout(&sPreview, sDat, &bps[bi])) sPreview.roomCount = 0;
    snprintf(sPreviewHullWant, sizeof(sPreviewHullWant), "img/ship/%.40s_base.png", bps[bi].img);
    snprintf(sPreviewFloorWant, sizeof(sPreviewFloorWant), "img/ship/%.40s_floor.png", bps[bi].img);
}

// Outside a frame: starts the decodes and swaps in finished images.
static bool hangar_preview_update(void) {
    if (sPreviewFor < 0) return false;
    image_job_want(&sPreviewHullJob, &sPreviewHull, sPreviewHullName, sPreviewHullWant, 0, 0);
    image_job_want(&sPreviewFloorJob, &sPreviewFloor, sPreviewFloorName, sPreviewFloorWant, 0, 0);
    bool a = image_job_poll(&sPreviewHullJob), b = image_job_poll(&sPreviewFloorJob);
    return a || b || strcmp(sPreviewHullName, sPreviewHullWant) || strcmp(sPreviewFloorName, sPreviewFloorWant);
}

static void hangar_preview_free(void) {
    // Let any decode in flight finish first (its thread writes into the job).
    while (image_job_poll(&sPreviewHullJob) || image_job_poll(&sPreviewFloorJob)) svcSleepThread(5 * 1000000LL);
    gpu_idle();
    texture_free(&sPreviewHull);
    texture_free(&sPreviewFloor);
    sPreviewHullName[0] = sPreviewFloorName[0] = '\0';
    sPreviewFor = -1;
}

// The preview, fitted into the top screen above the caption.
static void draw_hangar_preview(bool loading) {
    const Ship* s = &sPreview;
    if (s->roomCount == 0) return;
    const float ox = (float)(s->xOffset * TILE), oy = (float)(s->yOffset * TILE);
    bool hull = sPreviewHull.loaded && !loading;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < s->roomCount; i++) {
        const Room* r = &s->rooms[i];
        if (ox + r->x * TILE < x0) x0 = ox + r->x * TILE;
        if (oy + r->y * TILE < y0) y0 = oy + r->y * TILE;
        if (ox + (r->x + r->w) * TILE > x1) x1 = ox + (r->x + r->w) * TILE;
        if (oy + (r->y + r->h) * TILE > y1) y1 = oy + (r->y + r->h) * TILE;
    }
    if (hull) {
        float hx = ox + s->hullX, hy = oy + s->hullY;
        if (hx < x0) x0 = hx;
        if (hy < y0) y0 = hy;
        if (hx + sPreviewHull.width > x1) x1 = hx + sPreviewHull.width;
        if (hy + sPreviewHull.height > y1) y1 = hy + sPreviewHull.height;
    }
    Camera c = {.vx = 0, .vy = 0, .vw = TOP_W, .vh = TOP_H - 26};
    c.x = (x0 + x1) * 0.5f;
    c.y = (y0 + y1) * 0.5f;
    float zx = (c.vw - 16) / (x1 - x0), zy = (c.vh - 16) / (y1 - y0);
    c.zoom = c.fitZoom = zx < zy ? zx : zy;
    if (hull) {
        draw_image(&c, &sPreviewHull, ox + s->hullX, oy + s->hullY);
        if (sPreviewFloor.loaded) draw_image(&c, &sPreviewFloor, ox + s->hullX + s->floorX, oy + s->hullY + s->floorY);
    }
    // Rooms (light floors with dark walls) and their systems' icons.
    for (int i = 0; i < s->roomCount; i++) {
        const Room* r = &s->rooms[i];
        float x = ox + r->x * TILE, y = oy + r->y * TILE, w = r->w * TILE, h = r->h * TILE;
        draw_rect(&c, x, y, w, h, C2D_Color32(64, 62, 60, 255));
        draw_rect(&c, x + 2, y + 2, w - 4, h - 4, C2D_Color32(226, 220, 212, 255));
    }
    for (int k = 0; k < s->bp->systemCount; k++) {
        const SystemDef* d = &s->bp->systems[k];
        for (int i = 0; i < s->roomCount; i++) {
            if (s->rooms[i].id != d->roomId) continue;
            const Texture* t = small_icon(d->type);
            if (!t) break;
            const Room* r = &s->rooms[i];
            float X, Y;
            to_screen(&c, ox + (r->x + r->w * 0.5f) * TILE, oy + (r->y + r->h * 0.5f) * TILE, &X, &Y);
            C2D_ImageTint tint;
            C2D_PlainImageTint(&tint, C2D_Color32(80, 200, 90, 255), 1.0f);
            C2D_DrawImageAt(t->image, floorf(X - t->width * 0.5f), floorf(Y - t->height * 0.5f), 0, &tint, 1, 1);
            break;
        }
    }
    if (loading) {  // the hull is still decoding: say so, with dots that move
        int dots = (int)(sClock * 3) % 4;
        char msg[16];
        snprintf(msg, sizeof(msg), "LOADING%.*s", dots, "...");
        draw_text_centred(0, 6, TOP_W, 0.45f, C2D_Color32(200, 210, 225, 255), msg);
    }
}

// ---- Options and pause: rows of buttons ----
#define OPT_Y 30
#define OPT_PITCH 44
enum { OPT_MUSIC, OPT_SFX, OPT_BACK, OPT_COUNT };
enum { PAUSE_RESUME, PAUSE_OPTIONS, PAUSE_MENU, PAUSE_QUIT, PAUSE_COUNT };

static void draw_volume_row(float y, const char* label, int v, bool sel) {
    const u32 white = C2D_Color32(235, 235, 235, 255);
    draw_text(20, y + 8, 0.5f, sel ? C2D_Color32(255, 220, 90, 255) : white, label);
    ftl_button(130, y, 40, 32, false, "-");
    for (int i = 0; i < 10; i++)
        C2D_DrawRectSolid(176 + i * 9, y + 6, 0, 7, 20, i < v ? C2D_Color32(110, 220, 100, 255) : C2D_Color32(44, 52, 66, 255));
    ftl_button(270, y, 40, 32, false, "+");
}


// ---- Sandbox: any ship (all unlocked) with any weapons, drones and augments --------------------
// The hangar opened from SANDBOX, then a loadout screen: a row per slot, each cycling through
// every item this port can use. Sandbox runs earn no achievements or unlocks.
enum { LO_WEAPON, LO_DRONE, LO_AUG };
#define LO_MAX_ROWS 12
#define LO_TOP 22
#define LO_ROW_H 16
#define LO_BTN_Y 214
static int sLoKind[LO_MAX_ROWS], sLoSlot[LO_MAX_ROWS], sLoRowCount;
static const char* sLoPick[LO_MAX_ROWS];  // item per row (NULL: empty)
static const ItemDef* sLoList[3][300];
static int sLoListCount[3];

static void loadout_init(const ShipBlueprint* bp) {
    static const int itemKind[3] = {ITEM_WEAPON, ITEM_DRONE, ITEM_AUGMENT};
    for (int k = 0; k < 3; k++) {
        const ItemDef* all[300];
        int n = items_all_of_kind(itemKind[k], all, 300);
        sLoListCount[k] = 0;
        for (int i = 0; i < n; i++) {
            const WeaponDef* w = k == LO_WEAPON ? weapon_find(all[i]->name) : NULL;
            const DroneDef* dr = k == LO_DRONE ? drone_find(all[i]->name) : NULL;
            if ((k == LO_WEAPON && (!w || !weapon_supported(w))) || (k == LO_DRONE && (!dr || !drone_supported(dr)))) continue;
            sLoList[k][sLoListCount[k]++] = all[i];
        }
    }
    int counts[3] = {bp->weaponSlots, bp->droneSlots, MAX_AUGS};
    sLoRowCount = 0;
    for (int k = 0; k < 3; k++)
        for (int s = 0; s < counts[k] && sLoRowCount < LO_MAX_ROWS; s++) {
            sLoKind[sLoRowCount] = k;
            sLoSlot[sLoRowCount] = s;
            sLoPick[sLoRowCount++] = k == LO_WEAPON ? (s < bp->weaponCount ? bp->weapons[s] : NULL)
                                     : k == LO_DRONE ? (s < bp->droneCount ? bp->drones[s] : NULL)
                                                     : (s < bp->augCount ? bp->augs[s] : NULL);
        }
}

// The next (dir 1) or previous (-1) item for a row, through "empty".
static void loadout_cycle(int row, int dir) {
    int k = sLoKind[row], n = sLoListCount[k], at = -1;
    for (int i = 0; i < n && sLoPick[row]; i++)
        if (!strcmp(sLoList[k][i]->name, sLoPick[row])) at = i;
    at = (at + 1 + dir + (n + 1)) % (n + 1) - 1;
    sLoPick[row] = at < 0 ? NULL : sLoList[k][at]->name;
}

static void draw_loadout(void) {
    const u32 white = C2D_Color32(235, 235, 235, 255), dim = C2D_Color32(150, 160, 175, 255);
    static const char* const kindName[3] = {"Weapon", "Drone", "Augment"};
    C2D_DrawRectSolid(0, 0, 0, BOT_W, BOT_H, C2D_Color32(8, 10, 20, 255));
    draw_text(6, 3, 0.5f, white, "SANDBOX LOADOUT");
    draw_text(BOT_W - 64, 5, 0.34f, dim, "tap < or >");
    for (int r = 0; r < sLoRowCount; r++) {
        float y = LO_TOP + r * LO_ROW_H;
        C2D_DrawRectSolid(2, y, 0, BOT_W - 4, LO_ROW_H - 1, r == sMenuSel ? C2D_Color32(50, 62, 80, 255) : C2D_Color32(24, 30, 42, 255));
        char label[24];
        snprintf(label, sizeof(label), "%s %d", kindName[sLoKind[r]], sLoSlot[r] + 1);
        draw_text(6, y + 1, 0.38f, dim, label);
        draw_text(86, y + 1, 0.4f, white, "<");
        draw_text(BOT_W - 14, y + 1, 0.4f, white, ">");
        draw_text_centred(98, y + 1, BOT_W - 116, 0.4f, sLoPick[r] ? white : dim, sLoPick[r] ? item_title_of(sLoPick[r]) : "- empty -");
    }
    ftl_button(4, LO_BTN_Y, 90, 24, false, "BACK");
    ftl_button(BOT_W - 124, LO_BTN_Y, 120, 24, true, "START");
}

enum { LO_NONE = -1, LO_BACK = -2, LO_START = -3 };
static int loadout_touch(int px, int py, int* dir) {
    if (py >= LO_BTN_Y) return px < 100 ? LO_BACK : px >= BOT_W - 124 ? LO_START : LO_NONE;
    if (py < LO_TOP) return LO_NONE;
    int r = (py - LO_TOP) / LO_ROW_H;
    if (r >= sLoRowCount) return LO_NONE;
    *dir = px < (86 + BOT_W) / 2 ? -1 : 1;  // left half: previous; right half: next
    return r;
}

// Fits the chosen loadout to the new run's ship: weapons and drones replace the blueprint's, the
// weapons (and drone) system grows to power them all (up to 8, with the reactor to match), and a
// drone system is fitted if the ship has room for one.
static void sandbox_apply(Ship* s, FtlDat* dat, const AnimDb* db) {
    gpu_idle();
    while (s->weaponCount) combat_remove_weapon(s, 0);
    while (s->droneCount) combat_remove_drone(s, 0);
    sRun.augCount = 0;
    int wpower = 0, dpower = 0;
    for (int r = 0; r < sLoRowCount; r++) {
        if (!sLoPick[r]) continue;
        if (sLoKind[r] == LO_WEAPON && combat_add_weapon(s, dat, db, sLoPick[r])) wpower += weapon_find(sLoPick[r])->power;
        if (sLoKind[r] == LO_DRONE) {
            if (ship_find_system(s, SYS_DRONES) < 0)
                for (int o = 0; o < s->bp->optionalCount; o++)
                    if (s->bp->optional[o].type == SYS_DRONES) ship_install_system(s, dat, &s->bp->optional[o]);
            if (ship_find_system(s, SYS_DRONES) >= 0 && combat_add_drone(s, dat, db, sLoPick[r])) dpower += drone_find(sLoPick[r])->power;
        }
        if (sLoKind[r] == LO_AUG && sRun.augCount < MAX_AUGS)
            snprintf(sRun.augs[sRun.augCount++], sizeof(sRun.augs[0]), "%s", sLoPick[r]);
    }
    const int types[2] = {SYS_WEAPONS, SYS_DRONES}, need[2] = {wpower, dpower};
    for (int k = 0; k < 2; k++) {
        int si = ship_find_system(s, types[k]);
        if (si < 0) continue;
        int want = need[k] > 8 ? 8 : need[k], have = s->sysDefs[si].level;
        if (want <= have) continue;
        s->sysDefs[si].level = want;
        s->reactor += want - have;
        if (s->reactor > REACTOR_MAX) s->reactor = REACTOR_MAX;
    }
}

static void draw_options(void) {
    C2D_DrawRectSolid(0, 0, 0, BOT_W, BOT_H, C2D_Color32(10, 13, 24, 255));
    draw_text(8, 6, 0.55f, C2D_Color32(255, 220, 120, 255), "OPTIONS");
    draw_volume_row(OPT_Y, "MUSIC", sMusicVol, sMenuSel == OPT_MUSIC);
    draw_volume_row(OPT_Y + OPT_PITCH, "SOUND", sSfxVol, sMenuSel == OPT_SFX);
    ftl_button(BOT_W / 2 - 60, OPT_Y + 2 * OPT_PITCH + 20, 120, 34, sMenuSel == OPT_BACK, "BACK");
}

static int options_touch(int px, int py, int* delta) {
    *delta = 0;
    for (int r = 0; r < 2; r++) {
        float y = OPT_Y + r * OPT_PITCH;
        if (py < y || py >= y + 32) continue;
        if (px >= 130 && px < 170) *delta = -1;
        if (px >= 270 && px < 310) *delta = 1;
        return *delta ? r : -1;
    }
    float by = OPT_Y + 2 * OPT_PITCH + 20;
    return py >= by && py < by + 34 && px >= BOT_W / 2 - 60 && px < BOT_W / 2 + 60 ? OPT_BACK : -1;
}

static void draw_pause(void) {
    static const char* const items[PAUSE_COUNT] = {"RESUME", "OPTIONS", "MAIN MENU", "QUIT"};
    C2D_DrawRectSolid(0, 0, 0, BOT_W, BOT_H, C2D_Color32(10, 13, 24, 255));
    draw_text(8, 6, 0.55f, C2D_Color32(255, 220, 120, 255), "PAUSED");
    for (int i = 0; i < PAUSE_COUNT; i++) ftl_button(BOT_W / 2 - 90, 36 + i * 48, 180, 38, i == sMenuSel, items[i]);
}

static int pause_touch(int px, int py) {
    if (px < BOT_W / 2 - 90 || px >= BOT_W / 2 + 90) return -1;
    for (int i = 0; i < PAUSE_COUNT; i++)
        if (py >= 36 + i * 48 && py < 36 + i * 48 + 38) return i;
    return -1;
}

// ---- Saving and continuing ---------------------------------------------------------------------
// The game saves at rest (see save.h); CONTINUE rebuilds the ship from the save: its systems (and
// any bought since), their levels, damage and power, weapons, drones and crew with their skills.

// No fight, no event open, the run going on (not lost) and no jump under way (uses main()'s haveEnemy).
#define PEACEFUL \
    (sInRun && !haveEnemy && !sRun.view.active && !sRun.over && sJumpTime < 0.0f && combat.result != COMBAT_LOST)
#define SAVE_ARGS &player, &dat, &anims, &names, blueprints, bpCount, &index

static void save_write(ShipSlots* pl, FtlDat* dat, const AnimDb* db, const NameList* nm, const ShipBlueprint* bps,
                       int bpCount, int* index) {
    (void)dat, (void)db, (void)nm, (void)bps, (void)bpCount, (void)index;
    static SaveShip ss;
    save_ship_capture(&ss, &pl->ships[0], &pl->crews[0]);
    if (save_store(&sRun, &ss)) sSaveExists = true;
}

static bool save_continue(ShipSlots* pl, FtlDat* dat, const AnimDb* db, const NameList* nm, const ShipBlueprint* bps,
                          int bpCount, int* index) {
    static SaveShip ss;
    static Run run;
    if (!save_load(&run, &ss)) return false;
    int bi = find_blueprint(bps, bpCount, ss.bp);
    char e[96];
    if (bi < 0 || !slots_load(pl, dat, db, nm, &bps[bi], e, sizeof(e))) return false;
    *index = bi;
    Ship* s = &pl->ships[0];
    CrewSet* cs = &pl->crews[0];
    s->reactor = ss.reactor;
    // Systems: install any bought since, then levels and damage.
    for (int k = 0; k < ss.systemCount; k++) {
        int si = ship_find_system(s, ss.sys[k].type);
        if (si < 0) {
            const SystemDef* def = NULL;
            for (int o = 0; o < s->bp->optionalCount && !def; o++)
                if (s->bp->optional[o].type == ss.sys[k].type) def = &s->bp->optional[o];
            // A clone bay bought in place of the medbay (or the reverse) takes over its room.
            int t = ss.sys[k].type, other = -1;
            if (t == SYS_CLONEBAY || t == SYS_MEDBAY) {
                int ot = t == SYS_CLONEBAY ? SYS_MEDBAY : SYS_CLONEBAY;
                bool savedOther = false;
                for (int o = 0; o < ss.systemCount; o++) savedOther |= ss.sys[o].type == ot;
                if (!savedOther) other = ship_find_system(s, ot);
            }
            if (def && other >= 0) {
                gpu_idle();
                si = ship_replace_system(s, dat, other, def);
            } else if (def) {
                si = ship_install_system(s, dat, def);
            }
        }
        if (si < 0) continue;
        ((SystemDef*)s->systems[si].def)->level = ss.sys[k].level;
        s->systems[si].damage = ss.sys[k].damage;
    }
    // Weapons and drones as they were.
    gpu_idle();
    while (s->weaponCount) combat_remove_weapon(s, 0);
    while (s->droneCount) combat_remove_drone(s, 0);
    for (int k = 0; k < ss.weaponCount; k++) combat_add_weapon(s, dat, db, ss.weapons[k]);
    for (int k = 0; k < ss.droneCount; k++) combat_add_drone(s, dat, db, ss.drones[k]);
    // Power: systems first, then the weapons that were on.
    for (int k = 0; k < ss.systemCount; k++) {
        int si = ship_find_system(s, ss.sys[k].type), t = ss.sys[k].type;
        if (si >= 0 && !system_is_subsystem(t) && t != SYS_WEAPONS && t != SYS_DRONES) ship_set_power(s, si, ss.sys[k].power);
    }
    for (int k = 0; k < s->weaponCount && k < ss.weaponCount; k++)
        if (ss.weaponOn[k] && !s->weapons[k].powered) combat_toggle_weapon(s, k);
    s->maxHull = ss.maxHull;
    s->hullHp = ss.hullHp;
    s->missiles = ss.missiles;
    s->droneParts = ss.droneParts;
    // Crew, with their skills and wounds.
    cs->count = 0;
    cs->selected = 0;
    cs->cloneCount = 0;
    for (int k = 0; k < ss.crewCount; k++) {
        int ri = crew_race_look(cs, ss.crew[k].race, ss.crew[k].look);
        if (ri < 0 || !crew_add(cs, ri, ss.crew[k].name, ss.crew[k].female, ss.crew[k].x, ss.crew[k].y)) continue;
        Crew* m = &cs->crew[cs->count - 1];
        m->health = ss.crew[k].health > 0.0f ? ss.crew[k].health : m->health;
        m->homeX = ss.crew[k].homeX;
        m->homeY = ss.crew[k].homeY;
        memcpy(m->xp, ss.crew[k].xp, sizeof(m->xp));
    }
    sRun = run;
    return true;
}

static void strand_boarders(const Ship* ship, CrewSet* crew, CrewSet* away) {
    if (!away) return;
    int cb = ship_find_system(ship, SYS_CLONEBAY);
    for (int i = away->count - 1; i >= 0; i--) {
        Crew* m = &away->crew[i];
        bool ours = m->foreign != (m->mind > 0.0f);  // in the enemy's list: owned by us
        if (!ours) continue;
        if (!m->drone && cb >= 0 && (ship_system_capacity(ship, cb) > 0 || ship->aug[AUG_BACKUP_DNA]) && crew->cloneCount < MAX_CREW) {
            int ri = crew_race_look(crew, away->races[m->race].name, away->races[m->race].look);
            if (ri >= 0) {
                crew->cloneQueue[crew->cloneCount].race = ri;
                crew->cloneQueue[crew->cloneCount].female = m->female;
                memcpy(crew->cloneQueue[crew->cloneCount].name, m->name, sizeof(crew->cloneQueue[0].name));
                memcpy(crew->cloneQueue[crew->cloneCount].xp, m->xp, sizeof(m->xp));
                crew->cloneCount++;
            }
        }
        crew_remove(away, i);
    }
}

// After a surrender the enemy is friendly: our boarders come home (drones stay behind).
static void bring_boarders_home(const Ship* ship, CrewSet* crew, CrewSet* away) {
    if (!away) return;
    for (int i = away->count - 1; i >= 0; i--) {
        Crew* m = &away->crew[i];
        if (m->foreign == (m->mind > 0.0f)) continue;  // theirs
        if (m->drone) {
            crew_remove(away, i);
            continue;
        }
        int tx = -1, ty = -1;
        for (int r = 0; r < ship->roomCount && tx < 0; r++) {
            const Room* rm = &ship->rooms[r];
            for (int y = rm->y; y < rm->y + rm->h && tx < 0; y++)
                for (int x = rm->x; x < rm->x + rm->w && tx < 0; x++) {
                    bool used = false;
                    for (int k = 0; k < crew->count && !used; k++)
                        used = !crew->crew[k].foreign && crew->crew[k].destX == x && crew->crew[k].destY == y;
                    if (!used) {
                        tx = x;
                        ty = y;
                    }
                }
        }
        if (tx >= 0) crew_transfer(away, i, crew, tx, ty);
    }
}

static void wait_for_exit(void) {
    printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

int main(void) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(6144);  // the top screen is drawn once per eye in 3D
    C2D_Prepare();
    srand((unsigned)osGetTime());

    static FtlDat dat;
    static AnimDb anims;
    static NameList names;
    static ShipBlueprint blueprints[MAX_BLUEPRINTS], enemyBps[MAX_BLUEPRINTS];
    static ShipSlots player, enemy;
    static Combat combat;
    char err[128] = "";
    bool opened = false;

    bool romfs = R_SUCCEEDED(romfsInit());  // fails harmlessly when there's no romfs (.3dsx builds)
    for (size_t i = 0; i < sizeof(DAT_PATHS) / sizeof(DAT_PATHS[0]) && !opened; i++)
        opened = ftldat_open(&dat, DAT_PATHS[i], err, sizeof(err));
    int bpCount = 0, enemyCount = 0;
    bool runLoaded = false;
    if (opened) {
        animdb_open(&anims, &dat);
        if (sound_init(&dat)) sound_music(sound_random_track());
        texture_low_memory = sound_free_idle;  // textures come first when linear memory runs out
        settings_load();
        sSaveExists = save_exists();
        names_load(&names, &dat);
        text_load(&dat);
        profile_init(&dat);
        weapons_load(&dat);
        runLoaded = run_load(&dat);
        items_load(&dat);
        bpCount = ship_list_blueprints(&dat, blueprints, MAX_BLUEPRINTS);
        enemyCount = ship_list_enemies(&dat, enemyBps, MAX_BLUEPRINTS);
    }
    static TestSetup test;
    bool testing = opened && test_load(&test);
    bool sInRunAtStart = runLoaded && !(testing && test.enemy[0]);
    if (testing && test.seed >= 0) srand((unsigned)test.seed);
    int index = 0;
    if (testing && find_blueprint(blueprints, bpCount, test.ship) >= 0) index = find_blueprint(blueprints, bpCount, test.ship);
    if (!opened || bpCount == 0 || !slots_load(&player, &dat, &anims, &names, &blueprints[index], err, sizeof(err))) {
        consoleInit(GFX_BOTTOM, NULL);
        if (!opened) {
            printf("Couldn't load ftl.dat:\n  %s\n\n", err);
            printf("Copy ftl.dat from your FTL install to\n  %s\n", DAT_PATHS[1]);
        } else {
            printf("%s\n", bpCount == 0 ? "No player ships found in ftl.dat." : err);
        }
        wait_for_exit();
        goto done;
    }

    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* topRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);  // the right eye, in 3D
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf = C2D_TextBufNew(8192);
    sDat = &dat;
    sAnimDb = &anims;
    font_init(&dat);
    if (!sInRunAtStart) run_backdrop(&sRun, "BACKGROUND", "PLANET");
    load_explosion(&dat, &anims);
    load_sheet_anim(&sFireAnim, &dat, &anims, "fire_large");
    load_sheet_anim(&sBreachAnim, &dat, &anims, "breach");

    Camera cam = {.vx = 0, .vy = 0, .vw = ENEMY_X, .vh = TOP_H};                    // player, top screen
    Camera ecam = {.vx = ENEMY_X + 2, .vy = 40, .vw = ENEMY_W - 4, .vh = TOP_H - 42};  // enemy, top screen
    Camera map = {.vx = 0, .vy = MAP_TOP, .vw = BOT_W, .vh = MAP_H - MAP_TOP};          // crew tab
    Camera htop = {.vx = 0, .vy = 0, .vw = TOP_W, .vh = TOP_H - 26};                    // hangar, top screen
    Camera emap = {.vx = 0, .vy = ARMS_MAP_TOP, .vw = BOT_W, .vh = ARMS_MAP_H, .rotated = true};  // arms tab
    int tab = TAB_CREW, selectedWeapon = -1;
    bool mapTouch = false, dragging = false;  // crew-tab map touch in progress, and whether it's a drag
    bool beamAiming = false;  // arms tab: dragging out a beam path
    bool debugOpen = false;   // the SELECT debug menu is showing
    int dbgShip = 0, dbgEnemy = 0;
    float dragX0 = 0, dragY0 = 0, dragX1 = 0, dragY1 = 0;
    bool paused = false, haveEnemy = false;
    bool preloadPending = false;  // load the beacon's ship once the jump animation ends
    camera_fit(&cam, &player.ships[player.cur], 8, false);
    camera_fit(&map, &player.ships[player.cur], 8, true);

#define PLAYER_SHIP (&player.ships[player.cur])
#define PLAYER_CREW (&player.crews[player.cur])
#define ENEMY_SHIP (haveEnemy ? &enemy.ships[enemy.cur] : NULL)
#define ENEMY_CREW (haveEnemy ? &enemy.crews[enemy.cur] : NULL)

    // The enemy's cameras: the top screen's panel and the arms tab's map.
#define FIT_ENEMY_CAMERAS_FOR(es)                                                                                      \
    do {                                                                                                         \
        camera_fit(&ecam, (es), 4, true);                                                                  \
        /* Turn the enemy on the arms tab only if that shows it bigger. */                                       \
        emap.rotated = false;                                                                                    \
        camera_fit(&emap, (es), 10, true);                                                                 \
        float flatZoom = emap.zoom;                                                                              \
        emap.rotated = true;                                                                                     \
        camera_fit(&emap, (es), 10, true);                                                                 \
        if (emap.zoom < flatZoom) {                                                                              \
            emap.rotated = false;                                                                                \
            camera_fit(&emap, (es), 10, true);                                                             \
        }                                                                                                        \
    } while (0)
#define FIT_ENEMY_CAMERAS() FIT_ENEMY_CAMERAS_FOR(ENEMY_SHIP)

    // A fresh random enemy, and a new fight against it.
#define NEW_FIGHT()                                                                                              \
    do {                                                                                                         \
        int pick = enemyCount > 0 ? rand() % enemyCount : -1;                                                    \
        sVisitor = false;                                                                                        \
        if (testEnemy >= 0) pick = testEnemy;                                                                    \
        testEnemy = -1;                                                                                          \
        if (pick >= 0) ship_generate_enemy(&sEnemyBp, &enemyBps[pick], sInRun ? sRun.sector : 3, sInRun ? sRun.difficulty : DIFF_NORMAL); \
        haveEnemy = pick >= 0 && !testNoEnemy && \
                    slots_load(&enemy, &dat, &anims, &names, &sEnemyBp, err, sizeof(err)); \
        if (haveEnemy) FIT_ENEMY_CAMERAS();                                                                      \
        combat_start(&combat, PLAYER_SHIP, PLAYER_CREW, ENEMY_SHIP, ENEMY_CREW);                                 \
        sFtlCharge = haveEnemy ? 0.0f : 1.0f;                                                                    \
        selectedWeapon = -1;                                                                                     \
        paused = false;                                                                                          \
    } while (0)

    // The rebel flagship's stage (data/bosses.xml, normal difficulty with Advanced Edition): its own
    // blueprint as written, its surviving crew, and for stage 3 a 12-point Zoltan shield.
#define START_BOSS(stage)                                                                                       \
    do {                                                                                                         \
        char bossName[32];                                                                                       \
        sVisitor = false;                                                                                        \
        static const char* const bossDiff[3] = {"EASY", "NORMAL", "HARD"};                                       \
        snprintf(bossName, sizeof(bossName), "BOSS_%d_%s_DLC", (stage), bossDiff[sInRun ? sRun.difficulty : DIFF_NORMAL]); \
        haveEnemy = ship_find_boss(&dat, bossName, &sEnemyBp) &&                                                 \
                    slots_load(&enemy, &dat, &anims, &names, &sEnemyBp, err, sizeof(err));                       \
        if (haveEnemy) {                                                                                         \
            FIT_ENEMY_CAMERAS();                                                                                 \
            int artCrewOk[4] = {1, 1, 1, 1};                                                                     \
            int mainCrew = sInRun ? sRun.flagCrew : 7;                                                           \
            for (int k = 0; k < 4 && sInRun; k++) artCrewOk[k] = sRun.flagArtCrew[k];                           \
            boss_crew(ENEMY_SHIP, ENEMY_CREW, &names, mainCrew, artCrewOk);                                      \
            if ((stage) == 3) ENEMY_SHIP->superShield = ENEMY_SHIP->superShieldMax = 12; \
            /* Stage 1 hacks, which takes drone parts: it carries 10 (wiki). */ \
            if (ENEMY_SHIP->droneParts == 0) ENEMY_SHIP->droneParts = 10;                         \
        }                                                                                                        \
        combat_start(&combat, PLAYER_SHIP, PLAYER_CREW, ENEMY_SHIP, ENEMY_CREW);                                 \
        sBossStage = haveEnemy ? (stage) : 0;                                                                    \
        combat_set_boss(&combat, sBossStage);                                                                    \
        sBossAutomated = false;                                                                                  \
        sFtlCharge = haveEnemy ? 0.0f : 1.0f;                                                                    \
        selectedWeapon = -1;                                                                                     \
        paused = false;                                                                                          \
    } while (0)

    // Back to peace: no enemy.
#define END_FIGHT()                                                          \
    do {                                                                     \
        haveEnemy = false;                                                   \
        sVisitor = false; /* callers keeping the ship in view set it after */ \
        sBossStage = 0;                                                      \
        sEscapeLeft = -1.0f;                                                 \
        combat_start(&combat, PLAYER_SHIP, PLAYER_CREW, NULL, NULL);         \
        selectedWeapon = -1;                                                 \
        pickMode = PICK_NONE;                                                \
    } while (0)
#define RCTX (&(RunCtx){PLAYER_SHIP, PLAYER_CREW, &names, &dat, &anims, gpu_idle})
#define START_RUN()                                                          \
    do {                                                                     \
        sInRun = true;                                                       \
        sRun.difficulty = sDifficulty;                                       \
        combat.difficulty = sDifficulty;                                     \
        sMapOpen = sStoreOpen = sSectorMapOpen = false;                      \
\
        END_FIGHT();                                                         \
        run_start(&sRun, RCTX);                                              \
        sync_augs(PLAYER_SHIP, PLAYER_CREW, NULL);                           \
        combat_jump_reset(PLAYER_SHIP, PLAYER_CREW);                         \
        ach_reset();                                                         \
    } while (0)

    int testEnemy = testing ? find_blueprint(enemyBps, enemyCount, test.enemy) : -1;
    bool testNoEnemy = testing && !strcmp(test.enemy, "none");
    // A test setup naming an enemy is a one-off fight; otherwise play a run.
    if (testing && test.boss >= 1 && test.boss <= 3) {
        START_BOSS(test.boss);
    } else if (runLoaded && !(testing && test.enemy[0])) {
        START_RUN();
        if (testing && test.lastStand) {
            sRun.sector = RUN_SECTORS - 2;
            run_next_sector(&sRun, RCTX, 0);
        }
        if (testing && test.event[0]) run_show_named(&sRun, RCTX, test.event);
    } else {
        NEW_FIGHT();
    }
    // The game opens on the main menu (scenario tests skip it unless they ask for it).
    sMenu = runLoaded && (!testing || test.title) ? MENU_MAIN : MENU_NONE;
    sMenuSel = MM_NEWGAME;
    camera_fit(&htop, PLAYER_SHIP, 16, false);
    if (testing) {
        static const char* const tabNames[TAB_COUNT] = {"crew", "arms", "sys", "power"};
        for (int i = 0; i < TAB_COUNT; i++)
            if (!strcmp(test.tab, tabNames[i])) tab = i;
        if (!strcmp(test.tab, "fight")) tab = TAB_ARMS;
        if (test.weaponsOn)
            for (int i = 0; i < PLAYER_SHIP->weaponCount; i++) combat_toggle_weapon(PLAYER_SHIP, i);
        if (test.paused) paused = true;
        sShowMemory = test.memory != 0;
        sTestEnv = test.env;
        for (int tries = 0; tries < 16 && crew_native_count(PLAYER_CREW) < test.crew; tries++)
            run_add_crew(RCTX, "random", NULL);  // test setup: fill the crew
        for (int i = 0; test.skills > 0 && i < PLAYER_CREW->count; i++) {
            Crew* m = &PLAYER_CREW->crew[i];
            for (int k = 0; k < SKILL_COUNT; k++) {
                int lvl = test.skills - 1, t = crew_skill_threshold(PLAYER_CREW, m, k);
                m->xp[k] = (short)(lvl * t + (lvl < 2 ? t - 1 : 0));
            }
        }
        int tp = ship_find_system(PLAYER_SHIP, SYS_TELEPORTER);
        if (test.board > 0 && tp >= 0 && haveEnemy) {
            // Put crew on the pads, then send them to the enemy's shields (or any system) room.
            const Room* r = &PLAYER_SHIP->rooms[PLAYER_SHIP->systems[tp].room];
            int placed = 0;
            for (int i = PLAYER_CREW->count - 1; i >= 0 && placed < test.board && placed < r->w * r->h; i--, placed++) {
                Crew* c = &PLAYER_CREW->crew[i];
                c->destX = r->x + placed % r->w;
                c->destY = r->y + placed / r->w;
                c->x = c->destX * TILE + TILE * 0.5f;
                c->y = c->destY * TILE + TILE * 0.5f;
                c->pathLen = c->pathPos = 0;
            }
            int sh = ship_find_system(ENEMY_SHIP, SYS_SHIELDS);
            combat_teleport_send(&combat, SIDE_PLAYER,
                                 ENEMY_SHIP->systems[sh >= 0 ? sh : 0].room);
        }
        if (haveEnemy && test.enemyHull > 0) ENEMY_SHIP->hullHp = test.enemyHull;
        if (test.enemyFire == 0) {
            combat.enemyHoldFire = true;
            for (int i = 0; haveEnemy && i < ENEMY_SHIP->weaponCount; i++)
                ENEMY_SHIP->weapons[i].powered = ENEMY_SHIP->weapons[i].wanted = false;
        }
    }
    u64 lastTime = osGetTime(), startTime = lastTime;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld();
        if (down & KEY_START) {
            if (sMenu == MENU_NONE) {
                sMenu = MENU_PAUSE;
                sMenuSel = PAUSE_RESUME;
                sound_play("hoverBeep");
                down &= ~KEY_START;
            } else if (sMenu == MENU_PAUSE) {
                sMenu = MENU_NONE;
                menu_art(false);
                down &= ~KEY_START;
            }
        }

        u64 now = osGetTime(), lastFrameMs = lastTime;
        (void)lastFrameMs;
        float dt = (now - lastTime) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        lastTime = now;
        sClock = (now - startTime) / 1000.0f;
#ifdef FTL_TEST
        sTestFrameTime += ((now - lastFrameMs) / 1000.0f - sTestFrameTime) * 0.05f;
        // A scenario's button presses and taps from last frame, then this frame's commands.
        down |= sTestKeys;
        held |= sTestKeys;
        sTestKeys = 0;
        sTestTapNow = sTestTap;
        sTestTap = false;
        if (sTestTapNow) {
            down |= KEY_TOUCH;
            held |= KEY_TOUCH;
        }
        if (testing) {
#include "test_exec.inc"
            if (sTestQuit) break;
        }
#endif
        if (sMenu != MENU_NONE) {
            bool touched = (down & KEY_TOUCH) != 0, quitGame = false;
            touchPosition touch = {0};
            if (touched) read_touch(&touch);
            menu_art(true);
            if (sMenu == MENU_MAIN) {
                int act = -1;
                if (down & (KEY_DUP | KEY_DDOWN)) {
                    int dir = (down & KEY_DUP) ? MM_COUNT - 1 : 1;
                    do sMenuSel = (sMenuSel + dir) % MM_COUNT;
                    while (!mm_enabled(sMenuSel));
                }
                if (down & KEY_A) act = sMenuSel;
                if (touched) {
                    int t = main_menu_touch(touch.px, touch.py);
                    if (t >= 0 && mm_enabled(t)) {
                        act = t;
                        sMenuSel = t;
                    } else if (t >= 0) {
                        sound_play("powerUpFail");  // not in this port yet
                    }
                }
                if (act >= 0) sound_play("hoverBeep");
                if (act == MM_NEWGAME || act == MM_SANDBOX) {
                    sSandbox = act == MM_SANDBOX;
                    if (!sFamCount) build_families(blueprints, bpCount);
                    hangar_select(hangar_unlocked(blueprints, index) ? index : bp_of_family(blueprints, bpCount, FAM_KESTREL, 0));
                    if (sFam[sHangarFam][sHangarType] >= 0) index = sFam[sHangarFam][sHangarType];
                    hangar_preview_set(blueprints, index);
                    sMenu = MENU_HANGAR;
                } else if (act == MM_CONTINUE) {
                    if (save_continue(SAVE_ARGS)) {
                        ach_reset();
                        // Back to the saved beacon, at peace.
                        sInRun = true;
                        sMapOpen = sStoreOpen = sSectorMapOpen = sShipOpen = false;
                        END_FIGHT();
                        combat.difficulty = sRun.difficulty;
                        sDifficulty = sRun.difficulty;
                        camera_fit(&cam, PLAYER_SHIP, 8, false);
                        camera_fit(&map, PLAYER_SHIP, 8, true);
                        if (sRun.track[0]) sound_music(sRun.track);
                        sMenu = MENU_NONE;
                        menu_art(false);
                        continue;
                    }
                    sSaveExists = false;
                    sound_play("powerUpFail");
                } else if (act == MM_STATS) {
                    sMenu = MENU_STATS;
                    stats_select(0);
                } else if (act == MM_OPTIONS) {
                    sMenuBack = MENU_MAIN;
                    sMenu = MENU_OPTIONS;
                    sMenuSel = OPT_MUSIC;
                } else if (act == MM_QUIT) {
                    quitGame = true;
                }
            } else if (sMenu == MENU_HANGAR) {
                int h = touched ? hangar_touch(touch.px, touch.py) : HG_NONE;
                if (down & KEY_DLEFT) h = HG_PREV;
                if (down & KEY_DRIGHT) h = HG_NEXT;
                if (down & KEY_A) h = HG_START;
                if (down & KEY_B) h = HG_BACK;
                if (down & KEY_Y) h = HG_EASY + (sDifficulty + 1) % 3;
                if (down & KEY_X)
                    for (int k = 1; k <= 3 && h == HG_NONE; k++)
                        if (sFam[sHangarFam][(sHangarType + k) % 3] >= 0) h = HG_TYPE_A + (sHangarType + k) % 3;
                int fam = sHangarFam, type = sHangarType;
                if ((h == HG_PREV || h == HG_NEXT) && sFamCount > 0) {
                    fam = (fam + (h == HG_NEXT ? 1 : sFamCount - 1)) % sFamCount;
                    if (sFam[fam][type] < 0) type = sFam[fam][0] >= 0 ? 0 : sFam[fam][1] >= 0 ? 1 : 2;
                }
                if (h >= HG_TYPE_A && h <= HG_TYPE_C && sFam[fam][h - HG_TYPE_A] >= 0) type = h - HG_TYPE_A;
                if (h >= HG_EASY && h <= HG_HARD) {
                    sDifficulty = h - HG_EASY;
                    settings_save();
                }
                if (h != HG_NONE) sound_play("hoverBeep");
                if ((fam != sHangarFam || type != sHangarType) && sFam[fam][type] >= 0) {
                    index = sFam[fam][type];
                    sHangarFam = fam;
                    sHangarType = type;
                }
                hangar_preview_set(blueprints, index);
                if (h == HG_START && sSandbox) {  // on to the loadout
                    loadout_init(&blueprints[index]);
                    sMenu = MENU_LOADOUT;
                    sMenuSel = 0;
                    h = HG_NONE;
                }
                if (sHangarStarting) {
                    // START was pressed last frame (that frame said "Preparing ship"): load it now.
                    sHangarStarting = false;
                    h = HG_START;
                } else if (h == HG_START && hangar_unlocked(blueprints, sFam[sHangarFam][sHangarType])) {
                    sHangarStarting = true;  // show the message first
                    sound_play("jumpLeave");
                    h = HG_NONE;
                }
                if (h == HG_BACK) {
                    sMenu = MENU_MAIN;
                    sMenuSel = MM_NEWGAME;
                    hangar_preview_free();
                } else if (h == HG_START && !hangar_unlocked(blueprints, sFam[sHangarFam][sHangarType])) {
                    sound_play("powerUpFail");
                } else if (h == HG_START) {
                    hangar_preview_free();
                    sMenu = MENU_NONE;
                    menu_art(false);
                    gpu_idle();
                    texture_free(&sMiniShip);
                    sMiniFor = -1;
                    // A fresh ship: after "main menu" from a run, the old one still has its damage.
                    if (slots_load(&player, &dat, &anims, &names, &blueprints[index], err, sizeof(err))) {
                        camera_fit(&cam, PLAYER_SHIP, 8, false);
                        camera_fit(&map, PLAYER_SHIP, 8, true);
                    }
                    sRun.sandbox = false;
                    START_RUN();
                    continue;
                }
            } else if (sMenu == MENU_LOADOUT) {
                int dir = 0, row = -1, act = 0;  // act: -1 back, 1 start
                if (down & KEY_DUP) sMenuSel = (sMenuSel + sLoRowCount - 1) % sLoRowCount;
                if (down & KEY_DDOWN) sMenuSel = (sMenuSel + 1) % sLoRowCount;
                if (down & (KEY_DLEFT | KEY_DRIGHT)) {
                    row = sMenuSel;
                    dir = (down & KEY_DLEFT) ? -1 : 1;
                }
                if (down & KEY_B) act = -1;
                if (down & KEY_A) act = 1;
                if (touched) {
                    int t = loadout_touch(touch.px, touch.py, &dir);
                    if (t == LO_BACK) act = -1;
                    else if (t == LO_START) act = 1;
                    else if (t >= 0) row = sMenuSel = t;
                }
                if (row >= 0 && dir) {
                    loadout_cycle(row, dir);
                    sound_play("hoverBeep");
                }
                if (act == -1) {
                    sound_play("hoverBeep");
                    sMenu = MENU_HANGAR;
                } else if (act == 1) {
                    sound_play("jumpLeave");
                    hangar_preview_free();
                    sMenu = MENU_NONE;
                    menu_art(false);
                    gpu_idle();
                    texture_free(&sMiniShip);
                    sMiniFor = -1;
                    if (slots_load(&player, &dat, &anims, &names, &blueprints[index], err, sizeof(err))) {
                        camera_fit(&cam, PLAYER_SHIP, 8, false);
                        camera_fit(&map, PLAYER_SHIP, 8, true);
                    }
                    sRun.sandbox = true;
                    START_RUN();
                    sandbox_apply(PLAYER_SHIP, &dat, &anims);
                    continue;
                }
            } else if (sMenu == MENU_STATS) {
                if (down & KEY_DUP) stats_select(sAchSel - 1);
                if (down & KEY_DDOWN) stats_select(sAchSel + 1);
                if (down & KEY_DLEFT) stats_select(sAchSel - ACH_ROWS);
                if (down & KEY_DRIGHT) stats_select(sAchSel + ACH_ROWS);
                bool back = (down & KEY_B) != 0;
                if (touched) {
                    if (touch.py >= BOT_H - 28) {
                        if (touch.px < 100) back = true;
                        else if (touch.px >= BOT_W - 108 && touch.px < BOT_W - 56) stats_select(sAchSel - ACH_ROWS);
                        else if (touch.px >= BOT_W - 56) stats_select(sAchSel + ACH_ROWS);
                    } else if (touch.py >= ACH_LIST_Y) {
                        int row = (touch.py - ACH_LIST_Y) / ACH_ROW_H;
                        if (row < ACH_ROWS && sAchScroll + row < profile_ach_count()) stats_select(sAchScroll + row);
                    }
                }
                if (back) {
                    sound_play("hoverBeep");
                    sMenu = MENU_MAIN;
                    sMenuSel = MM_STATS;
                    ach_icons(false);
                }
            } else if (sMenu == MENU_OPTIONS) {
                int delta = 0, row = -1;
                if (down & KEY_DUP) sMenuSel = (sMenuSel + OPT_COUNT - 1) % OPT_COUNT;
                if (down & KEY_DDOWN) sMenuSel = (sMenuSel + 1) % OPT_COUNT;
                if (down & KEY_DLEFT) delta = -1;
                if (down & KEY_DRIGHT) delta = 1;
                if (delta) row = sMenuSel;
                if (touched) {
                    int td;
                    int t = options_touch(touch.px, touch.py, &td);
                    if (t >= 0) {
                        sMenuSel = t;
                        row = t;
                        delta = td;
                    }
                }
                bool back = (down & KEY_B) || ((down & KEY_A) && sMenuSel == OPT_BACK) || (row == OPT_BACK && touched);
                if (row == OPT_MUSIC || row == OPT_SFX) {
                    int* v = row == OPT_MUSIC ? &sMusicVol : &sSfxVol;
                    *v += delta;
                    if (*v < 0) *v = 0;
                    if (*v > 10) *v = 10;
                    settings_apply();
                    settings_save();
                    sound_play("hoverBeep");
                }
                if (back) {
                    sound_play("hoverBeep");
                    sMenu = sMenuBack;
                    sMenuSel = sMenuBack == MENU_PAUSE ? PAUSE_OPTIONS : MM_OPTIONS;
                }
            } else if (sMenu == MENU_PAUSE) {
                int act = -1;
                if (down & KEY_DUP) sMenuSel = (sMenuSel + PAUSE_COUNT - 1) % PAUSE_COUNT;
                if (down & KEY_DDOWN) sMenuSel = (sMenuSel + 1) % PAUSE_COUNT;
                if (down & KEY_A) act = sMenuSel;
                if (down & KEY_B) act = PAUSE_RESUME;
                if (touched) {
                    int t = pause_touch(touch.px, touch.py);
                    if (t >= 0) act = sMenuSel = t;
                }
                if (act >= 0) sound_play("hoverBeep");
                if (act == PAUSE_RESUME) {
                    sMenu = MENU_NONE;
                    menu_art(false);
                    continue;
                } else if (act == PAUSE_OPTIONS) {
                    sMenuBack = MENU_PAUSE;
                    sMenu = MENU_OPTIONS;
                    sMenuSel = OPT_MUSIC;
                } else if (act == PAUSE_MENU) {
                    if (PEACEFUL) save_write(SAVE_ARGS);  // CONTINUE picks it up (else the last save point)
                    END_FIGHT();
                    sMenu = MENU_MAIN;
                    sMenuSel = sSaveExists ? MM_CONTINUE : MM_NEWGAME;
                } else if (act == PAUSE_QUIT) {
                    if (PEACEFUL) save_write(SAVE_ARGS);
                    quitGame = true;
                }
            }
            if (quitGame) break;
            if (sMenu == MENU_NONE) continue;
            sound_update(dt);
            bool previewLoading = false;
            if (sMenu == MENU_HANGAR) hangar_mini(blueprints);
            if (sMenu == MENU_HANGAR || sMenu == MENU_LOADOUT) previewLoading = hangar_preview_update();
            if (sMenu == MENU_STATS) ach_icons(true);
            popup_update(dt, blueprints, bpCount);
            gfxSet3D(false);  // menus are flat
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TextBufClear(textBuf);
            C2D_TargetClear(top, C2D_Color32(8, 10, 20, 255));
            C2D_SceneBegin(top);
            if (sMenu == MENU_HANGAR || sMenu == MENU_LOADOUT) {
                // The chosen ship, large, with its crew.
                draw_title_top(NULL);
                C2D_DrawRectSolid(0, 0, 0, TOP_W, TOP_H, C2D_Color32(8, 10, 20, 170));
                draw_hangar_preview(previewLoading);
                if (sHangarStarting) {
                    C2D_DrawRectSolid(0, TOP_H / 2 - 16, 0, TOP_W, 30, C2D_Color32(8, 10, 20, 225));
                    draw_text_centred(0, TOP_H / 2 - 12, TOP_W, 0.55f, C2D_Color32(255, 220, 120, 255), "PREPARING SHIP...");
                }
                static const char* const diffs[3] = {"Easy", "Normal", "Hard"};
                char cap[96];
                snprintf(cap, sizeof(cap), "%s%s - Type %c - %s", sSandbox ? "Sandbox: " : "",
                         text_get(blueprints[index].classId, blueprints[index].name), 'A' + sHangarType, diffs[sDifficulty]);
                C2D_DrawRectSolid(0, TOP_H - 24, 0, TOP_W, 24, C2D_Color32(8, 10, 20, 220));
                draw_text_centred(0, TOP_H - 21, TOP_W, 0.45f, C2D_Color32(235, 235, 235, 255), cap);
            } else {
                draw_title_top(sMenu == MENU_PAUSE ? "PAUSED" : sMenu == MENU_OPTIONS ? "OPTIONS" : NULL);
                if (sMenu == MENU_STATS) draw_stats_top();
            }
            draw_popup(blueprints, bpCount);
            C2D_TargetClear(bottom, C2D_Color32(8, 10, 20, 255));
            C2D_SceneBegin(bottom);
            if (sMenu == MENU_MAIN) draw_main_menu();
            else if (sMenu == MENU_HANGAR) draw_hangar(blueprints);
            else if (sMenu == MENU_OPTIONS) draw_options();
            else if (sMenu == MENU_STATS) draw_stats();
            else if (sMenu == MENU_LOADOUT) draw_loadout();
            else draw_pause();
            C3D_FrameEnd(0);
            continue;
        }

        // Switching ships, or restarting after losing, starts a new fight.
        if (sInRun && PLAYER_SHIP->hullHp <= 0) combat.result = COMBAT_LOST;  // e.g. event damage
        // The run's augments, as counts on the ship for combat.
        sync_augs(PLAYER_SHIP, PLAYER_CREW, ENEMY_CREW);
        if (PLAYER_SHIP->aug[AUG_CRYSTAL_SHARDS]) combat_load_shard(&dat, &anims);
        bool restart = (down & KEY_A) && (combat.result == COMBAT_LOST || (sInRun && sRun.over));
        if (restart && sInRun) {
            if (slots_load(&player, &dat, &anims, &names, &blueprints[index], err, sizeof(err))) {
                camera_fit(&cam, PLAYER_SHIP, 8, false);
                camera_fit(&map, PLAYER_SHIP, 8, true);
                camera_fit(&htop, PLAYER_SHIP, 16, false);
                END_FIGHT();
                sMenu = MENU_MAIN;  // back to the main menu after a run
                sMenuSel = MM_NEWGAME;
            }
        } else if (restart) {
            if (slots_load(&player, &dat, &anims, &names, &blueprints[index], err, sizeof(err))) {
                camera_fit(&cam, PLAYER_SHIP, 8, false);
                camera_fit(&map, PLAYER_SHIP, 8, true);
                if (sInRun)
                    START_RUN();
                else
                    NEW_FIGHT();
            }
        } else if (down & KEY_A) {
            paused = !paused;
        }
        if (combat.result == COMBAT_WON && combat.resultTime > 4.0f) {
            if (sInRun) {
                strand_boarders(PLAYER_SHIP, PLAYER_CREW, ENEMY_CREW);  // still aboard when it blew up
                if (sBossStage) {
                    sRun.flagCrew = sBossCrew;
                    for (int k = 0; k < 4; k++) sRun.flagArtCrew[k] = sBossArtCrew[k];
                    run_flagship_defeated(&sRun, RCTX);
                } else {
                    run_fight_won(&sRun, RCTX, !combat.crewKilled);
                }
                bool derelict = combat.crewKilled && !sBossStage;
                END_FIGHT();
                sVisitor = derelict;
            } else {
                NEW_FIGHT();
            }
        }
        // The flagship is here: its stage's fight once the text closes.
        if (sInRun && sRun.bossFight && !sRun.view.active) {
            sRun.bossFight = false;
            START_BOSS(sRun.flagStage);
        }
        if (sBossStage && haveEnemy && combat.result == COMBAT_FIGHTING) {
            boss_crew_count(ENEMY_SHIP, ENEMY_CREW);
            if (!sBossAutomated && crew_native_count(ENEMY_CREW) == 0) {
                sBossAutomated = true;  // its crew are dead, but the flagship fights on (wiki)
                if (sInRun) run_show_named(&sRun, RCTX, "BOSS_AUTOMATED");
            }
        }
        // The event asked for a fight or a store once its dialog closed.
        // Mid-fight events (an escape warning, say) carry <ship hostile="true"/> to mean "still
        // hostile": that's this fight carrying on, not a new one.
        if (sInRun && sRun.view.ship && haveEnemy && combat.result == COMBAT_FIGHTING) sRun.view.ship = NULL;
        if (sInRun && !sRun.view.active && sRun.view.ship) {
            const void* key = sRun.view.shipDef ? (const void*)sRun.view.shipDef : (const void*)sRun.view.ship;
            if (sPreloadFor && key == sPreloadFor && enemy.loaded) {
                // Built during the jump: straight into the fight.
                haveEnemy = true;
                FIT_ENEMY_CAMERAS();
                combat_start(&combat, PLAYER_SHIP, PLAYER_CREW, ENEMY_SHIP, ENEMY_CREW);
                sFtlCharge = 0.0f;
                selectedWeapon = -1;
                paused = false;
                sRun.view.ship = NULL;
            } else {
                const char* bp = run_ship_blueprint(sRun.view.ship, sRun.view.shipDef);
                testEnemy = bp ? find_blueprint(enemyBps, enemyCount, bp) : -1;
                sRun.view.ship = NULL;
                NEW_FIGHT();
            }
            sPreloadFor = NULL;
            run_ship_thresholds(&sRun, &sSurrenderHp, &sEscapeHp, &sEscapeTime);
            sEscapeLeft = -1.0f;
            if (testing && test.enemyHull > 0 && haveEnemy) {  // test setup: the run's first fight
                ENEMY_SHIP->hullHp = test.enemyHull;
                test.enemyHull = 0;
            }
        }
        // An accepted surrender (or another peaceful end) once its dialog closes.
        if (sInRun && !sRun.view.active && sRun.view.peace) {
            sRun.view.peace = false;
            if (haveEnemy && combat.result == COMBAT_FIGHTING) {
                bring_boarders_home(PLAYER_SHIP, PLAYER_CREW, ENEMY_CREW);
                END_FIGHT();
                sVisitor = true;  // at peace now, but still here
                sRun.view.shipDef = NULL;
            }
        }
        if (sInRun && !sRun.view.active && sRun.view.store) {
            sRun.view.store = false;
            sStoreOpen = true;
            sStoreTab = 0;
            sStoreSel = -1;
            sInfoTitle[0] = '\0';
        }
        // The beacon's environment. With a hazard and no enemy the drive still has to charge
        // (wiki: Engines, "FTL Drive charge time").
        sEnvNow = sInRun ? sRun.env : sTestEnv;
        rock_layers(sInRun && sEnvNow == ENV_ASTEROID);
        clouds_want(sEnvNow);
        clouds_update(dt);
        sRockClock += dt;
        combat_set_environment(&combat, sEnvNow, sInRun ? sRun.envTarget : ENV_TARGET_PLAYER);
        static bool wasDanger;
        bool danger = env_dangerous(sEnvNow);
        if (danger && !wasDanger && !haveEnemy) sFtlCharge = 0.0f;
        wasDanger = danger;
        if (!haveEnemy && !danger) sFtlCharge = 1.0f;
        sFighting = haveEnemy && combat.result == COMBAT_FIGHTING;
        {
            // Autosave whenever the game comes to rest (no fight, event, map, store or ship menu
            // open); the run's end deletes the save, as in FTL.
            static bool wasPeaceful;
            bool peaceful = PEACEFUL && !sMapOpen && !sStoreOpen && !sShipOpen && !sSectorMapOpen;
            if (peaceful && !wasPeaceful) save_write(SAVE_ARGS);
            wasPeaceful = peaceful;
            if (sInRun && (sRun.over || combat.result == COMBAT_LOST) && sSaveExists) {
                save_delete();
                sSaveExists = false;
            }
        }
        sCanJump = sInRun && combat.result != COMBAT_LOST && !(sRun.over && sRun.won) && sJumpTime < 0.0f &&
                   // Not while a win is waiting to pay out (its reward comes ~4 s after the kill).
                   (!haveEnemy ? sFtlCharge >= 1.0f : (combat.result == COMBAT_FIGHTING && sFtlCharge >= 1.0f));
        // The jump itself happens at the white flash.
        if (sJumpTime >= 0.0f) {
            float before = sJumpTime;
            sJumpTime += dt;
            if (before < JUMP_OUT && sJumpTime >= JUMP_OUT) {
                if (haveEnemy) {  // escaped: no reward, and anyone still over there is left behind
                    strand_boarders(PLAYER_SHIP, PLAYER_CREW, ENEMY_CREW);
                    END_FIGHT();
                    sRun.view.shipDef = NULL;
                }
                combat_jump_reset(PLAYER_SHIP, PLAYER_CREW);  // before the arrival's event can start a fight
                // Let go of the last beacon's sounds (the last enemy's weapons, hazards...), then
                // decode ours again; the next enemy's are queued when it loads.
                sound_release_unused();
                combat_prefetch_ship_sounds(PLAYER_SHIP);
                gpu_idle();
                crew_prune_races(PLAYER_CREW);  // boarders' sprite sheets (about 1 MB each)
                explosion_cache_clear();
                item_art_clear();
                sVisitor = false;
                if (!haveEnemy && enemy.loaded) slots_free(&enemy);  // the last enemy ship
                sPreloadFor = NULL;
                preloadPending = !haveEnemy;  // the beacon's ship, once we've arrived (below)
                if (sJumpTarget < 0) {
                    sRun.fuel--;
                    run_next_sector(&sRun, RCTX, sNextRow);
                    sound_play("jumpArrive");
                } else {
                    run_jump(&sRun, RCTX, sJumpTarget);  // plays jumpArrive
                }
                ach_arrived();
            }
            if (before >= JUMP_OUT && sBackdropLoading && sJumpTime > JUMP_OUT + 0.05f) sJumpTime = JUMP_OUT + 0.05f;
            if (sJumpTime >= JUMP_OUT + JUMP_IN) sJumpTime = -1.0f;
        }
        // The beacon's ship, if its event has one, built once the jump is over (doing it at the
        // flash froze the animation for a second): it shows while the event's text is up, and the
        // fight starts with it in place instead of stalling to load then.
        if (preloadPending && sJumpTime < 0.0f) {
            preloadPending = false;
            const XNode *arrShip, *arrDef;
            if (sInRun && !haveEnemy && run_arrival_ship(&sRun, &arrShip, &arrDef)) {
                const char* bp = run_ship_blueprint(arrShip, arrDef);
                int pick = bp ? find_blueprint(enemyBps, enemyCount, bp) : -1;
                if (pick >= 0) {
                    gpu_idle();
                    ship_generate_enemy(&sEnemyBp, &enemyBps[pick], sRun.sector, sRun.difficulty);
                    if (slots_load(&enemy, &dat, &anims, &names, &sEnemyBp, err, sizeof(err))) {
                        sPreloadFor = arrDef ? (const void*)arrDef : (const void*)arrShip;
                        sVisitor = true;  // in view while its event plays out
                        FIT_ENEMY_CAMERAS_FOR(&enemy.ships[enemy.cur]);
                    }
                }
            }
        }

        Ship* ship = PLAYER_SHIP;
        CrewSet* crew = PLAYER_CREW;
        Ship* foe = ENEMY_SHIP;
        CrewSet* foeCrew = ENEMY_CREW;

        if (down & KEY_SELECT) {
            debugOpen = !debugOpen;
            dbgShip = index;
            sound_play("hoverBeep");
        }
        if (down & KEY_X) {
            tab = (tab + 1) % TAB_COUNT;
            sound_play("hoverBeep");
        }
        // Debug: L+R+B destroys the enemy ship.
        if ((down & KEY_B) && (held & KEY_L) && (held & KEY_R) && foe) {
            foe->hullHp = 0;
            down &= ~KEY_B;
        }
        // Debug: L+R+Y starts a fire and a hull breach in random rooms of our ship.
        if ((down & KEY_Y) && (held & KEY_L) && (held & KEY_R)) {
            life_ignite(ship, rand() % ship->roomCount);
            life_breach(ship, rand() % ship->roomCount);
            down &= ~KEY_Y;
        }
        if ((down & KEY_Y) && crew_native_count(crew) > 0) {
            int first = -1;  // select the next of our crew after the first selected one
            for (int i = 0; i < crew->count && first < 0; i++)
                if (crew_is_selected(crew, i)) first = i;
            for (int k = 1; k <= crew->count; k++) {
                int i = (first + k + crew->count) % crew->count;
                if (!crew->crew[i].foreign && !crew->crew[i].drone) {
                    crew->selected = 1u << i;
                    break;
                }
            }
        }
        if (down & KEY_B) {
            pickMode = PICK_NONE;
            crew->selected = 0;
            selectedWeapon = -1;
        }

        if ((down & KEY_TOUCH) && debugOpen) {
            touchPosition touch;
            read_touch(&touch);
            int act = touch.py >= TAB_H ? debug_touch(touch.px, touch.py) : -1;
            if (act >= 0) sound_play("hoverBeep");
            switch (act) {
                case DBG_SHIP_PREV: dbgShip = (dbgShip + bpCount - 1) % bpCount; break;
                case DBG_SHIP_NEXT: dbgShip = (dbgShip + 1) % bpCount; break;
                case DBG_ENEMY_PREV: dbgEnemy = (dbgEnemy + enemyCount - 1) % enemyCount; break;
                case DBG_ENEMY_NEXT: dbgEnemy = (dbgEnemy + 1) % enemyCount; break;
                case DBG_START:
                    if (slots_load(&player, &dat, &anims, &names, &blueprints[dbgShip], err, sizeof(err))) {
                        index = dbgShip;
                        camera_fit(&cam, PLAYER_SHIP, 8, false);
                        camera_fit(&map, PLAYER_SHIP, 8, true);
                        testEnemy = dbgEnemy;
                        sInRun = sMapOpen = sStoreOpen = false;
                        NEW_FIGHT();
                        debugOpen = false;
                    }
                    break;
                case DBG_DESTROY:
                    if (foe) foe->hullHp = 0;
                    break;
                case DBG_KILL_CREW:
                    for (int i = 0; foeCrew && i < foeCrew->count; i++)
                        if (!foeCrew->crew[i].foreign) foeCrew->crew[i].health = -1000.0f;
                    for (int i = 0; i < crew->count; i++)
                        if (crew->crew[i].foreign) crew->crew[i].health = -1000.0f;
                    break;
                case DBG_FIRE:
                    life_ignite(ship, rand() % ship->roomCount);
                    life_breach(ship, rand() % ship->roomCount);
                    break;
                case DBG_FIX:
                    ship->hullHp = ship->maxHull;
                    for (int i = 0; i < ship->systemCount; i++) ship->systems[i].damage = 0;
                    for (int i = 0; i < crew->count; i++)
                        if (!crew->crew[i].foreign) crew->crew[i].health = crew->races[crew->crew[i].race].maxHealth;
                    memset(ship->fire, 0, sizeof(ship->fire));
                    memset(ship->breach, 0, sizeof(ship->breach));
                    for (int r = 0; r < ship->roomCount; r++) ship->oxygen[r] = 100.0f;
                    break;
                case DBG_ENEMY_BOARDS:
                    if (foe) combat_debug_board(&combat, SIDE_ENEMY, 2);
                    break;
                case DBG_WE_BOARD:
                    if (foe) combat_debug_board(&combat, SIDE_PLAYER, 2);
                    break;
                case DBG_RESOURCES:
                    ship->missiles = 99;
                    ship->droneParts = 99;
                    ship->reactor += 10;
                    sRun.scrap += 200;
                    sRun.fuel += 10;
                    break;
                case DBG_HOLD_FIRE: combat.enemyHoldFire = !combat.enemyHoldFire; break;
                case DBG_COOLDOWNS:
                    for (int i = 0; i < ship->systemCount; i++) {
                        ship->systems[i].ionTimer = 0.0f;
                        ship->systems[i].ionLimit = -1;
                    }
                    for (int i = 0; i < ship->weaponCount; i++) ship->weapons[i].charge = ship->weapons[i].fullCharge;
                    for (int i = 0; i < ship->droneCount; i++) ship->drones[i].redeploy = 0.0f;
                    for (int ak = 0; ak < ship->artilleryCount; ak++)
                        ship->artillery[ak].charge = ship_artillery_time(ship, ak) - 0.5f;
                    break;
                case DBG_BREAK_ENEMY:
                    for (int i = 0; foe && i < foe->systemCount; i++) foe->systems[i].damage = foe->systems[i].def->level;
                    break;
                case DBG_SENSORS: sDebugSensors = !sDebugSensors; break;
                case DBG_NEW_RUN:
                    if (slots_load(&player, &dat, &anims, &names, &blueprints[dbgShip], err, sizeof(err))) {
                        index = dbgShip;
                        camera_fit(&cam, PLAYER_SHIP, 8, false);
                        camera_fit(&map, PLAYER_SHIP, 8, true);
                        START_RUN();
                        debugOpen = false;
                    }
                    break;
            }
        } else if ((down & KEY_TOUCH) && sInRun && sRun.view.active) {
            touchPosition touch;
            read_touch(&touch);
            int ch = event_choice_at(&sRun.view, touch.py);
            if (ch >= 0 && sRun.view.choiceDisabled[ch]) {
                sound_play("powerUpFail");
            } else if (ch >= 0) {
                sound_play("hoverBeep");
                run_choose(&sRun, RCTX, ch);
            }
        } else if ((down & KEY_TOUCH) && sMapOpen) {
            touchPosition touch;
            read_touch(&touch);
            int t = run_map_touch(&sRun, touch.px, touch.py);
            if (t == MAPT_CLOSE) {
                sMapOpen = false;
                sound_play("hoverBeep");
            } else if (t == MAPT_ACTION && map_can_wait(&sRun)) {
                // Out of fuel (FTL's WAIT): the fleet closes in and something may happen.
                sMapOpen = false;
                sound_play("hoverBeep");
                run_wait(&sRun, RCTX);
            } else if (t == MAPT_ACTION && sRun.beacons[sRun.current].exit) {
                if (sRun.fuel <= 0) {
                    sound_play("powerUpFail");
                } else if (sRun.sector + 1 >= RUN_SECTORS) {
                    sMapOpen = false;  // the last sector's exit: the run is over
                    sound_play("jumpLeave");
                    sJumpTarget = -1;
                    sJumpTime = 0.0f;
                } else {
                    sMapOpen = false;
                    sSectorMapOpen = true;
                    sSectorPick = -1;
                    sound_play("hoverBeep");
                }
            } else if (t == MAPT_ACTION && sRun.beacons[sRun.current].storeSlot > 0) {
                run_store_enter(&sRun, RCTX);  // this beacon's stock, as we left it
                sMapOpen = false;
                sStoreOpen = true;
                sStoreTab = 0;
                sStoreSel = -1;
                sInfoTitle[0] = '\0';
            } else if (t >= 0 && run_can_jump(&sRun, t)) {
                sMapOpen = false;
                sound_play("jumpLeave");
                sJumpTarget = t;
                sJumpTime = 0.0f;
            } else if (t >= 0) {
                sound_play("powerUpFail");
            }
        } else if ((down & KEY_TOUCH) && sSectorMapOpen) {
            touchPosition touch;
            read_touch(&touch);
            int t = sector_map_touch(&sRun, touch.px, touch.py);
            if (t == SMAP_CLOSE) {
                sSectorMapOpen = false;
                sMapOpen = true;
                sound_play("hoverBeep");
            } else if (t == SMAP_JUMP && sRun.fuel > 0) {
                sSectorMapOpen = false;
                sNextRow = sSectorPick;
                sound_play("jumpLeave");
                sJumpTarget = -1;
                sJumpTime = 0.0f;
            } else if (t >= 0) {
                sSectorPick = t;
                sound_play("hoverBeep");
            }
        } else if ((down & KEY_TOUCH) && sStoreOpen) {
            touchPosition touch;
            read_touch(&touch);
            int t = store_touch(&sRun, ship, touch.px, touch.py);
            bool ok = true;
            if (t == ST_LEAVE) {
                sStoreOpen = false;
                item_art_clear();
                sInfoTitle[0] = '\0';
                sound_play("hoverBeep");
            } else if (t >= ST_SELL) {
                ok = store_sell(&sRun, ship, t - ST_SELL);
                selectedWeapon = -1;
            } else if (t >= ST_PICK) {
                sStoreSel = t - ST_PICK;
                int sec = sStoreTab - 1;
                const char* name = sRun.storeItem[sec][sStoreSel];
                if (sRun.storeKind[sec] == ITEM_SYSTEM) {
                    const SysBlueprint* sb = item_system(system_from_name(name));
                    if (sb) set_info(sb->title, "A new system, installed in the room your ship has for it.");
                } else if (name[0]) {
                    set_info_item(name);
                }
            } else if (t >= ST_BUY) {
                ok = run_store_buy(&sRun, RCTX, sStoreTab - 1, t - ST_BUY);
                if (ok) sRun.feats.storeBuy = true;
            } else if (t >= ST_RES) {
                ok = run_buy(&sRun, RCTX, t - ST_RES);
                if (ok && t - ST_RES == STORE_HULL) sRun.feats.storeRepair = true;
                else if (ok) sRun.feats.storeBuy = true;
            } else if (t >= ST_TAB) {
                sStoreTab = t - ST_TAB;
                sStoreSel = -1;
                sInfoTitle[0] = '\0';
                sound_play("hoverBeep");
            }
            if (!ok) sound_play("powerUpFail");
        } else if ((down & KEY_TOUCH) && sShipOpen) {
            touchPosition touch;
            read_touch(&touch);
            int t;
            if (touch.px >= BOT_W - 60 && touch.py < 22) {
                sShipOpen = false;
                sEquipSel = -1;
                sInfoTitle[0] = '\0';
                sound_play("hoverBeep");
            } else if ((t = shop_tab_at(2, touch.px, touch.py)) >= 0) {
                sShipTab = t;
                sEquipSel = -1;
                sInfoTitle[0] = '\0';
                sound_play("hoverBeep");
            } else if (sShipTab == 0) {
                int i = upgrade_touch(ship, touch.px, touch.py);
                int cost = i >= 0 ? upgrade_cost(ship, i) : 0;
                if (i >= 0 && cost && sRun.scrap >= cost) {
                    sRun.scrap -= cost;
                    if (i == ship->systemCount) {
                        ship->reactor++;
                        sRun.feats.upgradedReactor = true;
                    } else {
                        ship->sysDefs[i].level++;
                        sRun.feats.upgradedSystem = true;
                        if (ship->sysDefs[i].type == SYS_WEAPONS) sRun.feats.upgradedWeapons = true;
                    }
                    // Subsystems always run at their full level.
                    if (i < ship->systemCount && system_is_subsystem(ship->sysDefs[i].type)) ship->systems[i].power = ship->sysDefs[i].level;
                    sound_play("upgradeSystem");
                } else if (i >= 0) {
                    sound_play("powerUpFail");
                }
            } else if (sFighting) {
                sound_play("powerUpFail");  // FTL locks equipment while an enemy is here
            } else {
                int cell = equip_cell_at(ship, touch.px, touch.py);
                if (cell >= 0 && sEquipSel < 0) {
                    const char* name = equip_name(&sRun, ship, cell / 8, cell % 8);
                    if (name[0]) {
                        sEquipSel = cell;
                        set_info_item(name);
                    }
                } else if (cell >= 0 && cell == sEquipSel) {
                    sEquipSel = -1;
                } else if (cell >= 0) {
                    if (!equip_move(&sRun, ship, &dat, &anims, sEquipSel, cell)) sound_play("powerUpFail");
                    sEquipSel = -1;
                    selectedWeapon = -1;  // weapon indices may have moved
                }
            }
        } else if (down & KEY_TOUCH) {
            touchPosition touch;
            read_touch(&touch);
            int tabsX = BOT_W - TAB_COUNT * TAB_W;
            if (touch.py < TAB_H) {
                bool intruders = false;
                for (int i = 0; i < crew->count && !intruders; i++) intruders = crew->crew[i].foreign && !crew->crew[i].drone;
                if (sInRun && touch.px < SHIPBTN_W + 1 && (env_dangerous(sEnvNow) || sFighting || intruders)) {
                    sound_play("powerUpFail");  // in danger: no upgrades or equipment changes
                } else if (sInRun && touch.px < SHIPBTN_W + 1) {
                    sShipOpen = true;
                    sEquipSel = -1;
                    sInfoTitle[0] = '\0';
                    sound_play("hoverBeep");
                } else if (sInRun && touch.px >= JUMPBTN_X && touch.px < PAUSE_X) {
                    if (sCanJump) {
                        sMapOpen = true;
                        sound_play("hoverBeep");
                    } else {
                        sound_play("powerUpFail");
                    }
                } else if (touch.px >= PAUSE_X && touch.px < PAUSE_X + PAUSE_W && combat.result != COMBAT_LOST) {
                    paused = !paused;
                    sound_play("hoverBeep");
                } else if (touch.px >= tabsX) {
                    int newTab = (touch.px - tabsX) / TAB_W;
                    if (newTab != tab) sound_play("hoverBeep");
                    tab = newTab;
                }
            } else if (tab == TAB_POWER) {
                power_tab_touch(ship, &combat, touch.px, touch.py);
            } else if (tab == TAB_ARMS && pickMode && foe && touch.py >= ARMS_MAP_TOP && touch.py < CARDS_TOP) {
                float sxp, syp;
                to_ship(&emap, touch.px, touch.py, &sxp, &syp);
                int room = ship_room_at(foe, (int)floorf(sxp / TILE - foe->xOffset), (int)floorf(syp / TILE - foe->yOffset));
                if (room >= 0) {
                    bool ok = pickMode == PICK_TELEPORT ? combat_teleport_send(&combat, SIDE_PLAYER, room) > 0
                              : pickMode == PICK_HACK   ? combat_hack_launch(&combat, SIDE_PLAYER, room)
                                                        : combat_mind_control(&combat, SIDE_PLAYER, room, false);
                    if (!ok) sound_play("powerUpFail");
                    pickMode = PICK_NONE;
                }
            } else if (tab == TAB_ARMS && foe && selectedWeapon >= 0 && touch.py >= ARMS_MAP_TOP &&
                       touch.py < CARDS_TOP && ship->weapons[selectedWeapon].def->type == WEAPON_BEAM) {
                beamAiming = true;  // finished on release
                dragX0 = dragX1 = touch.px;
                dragY0 = dragY1 = touch.py;
            } else if (tab == TAB_ARMS) {
                selectedWeapon = arms_tab_touch(ship, foe, &combat, &emap, selectedWeapon, touch.px, touch.py);
            } else if (tab == TAB_SYS) {
                sys_tab_touch(ship, &combat, touch.px, touch.py);
                if (pickMode) tab = TAB_ARMS;
            } else if (touch.py >= DOORBTN_Y && touch.py < DOORBTN_Y + DOORBTN_H && touch.px >= DOORBTN_X &&
                       touch.px < DOORBTN_X + 2 * DOORBTN_W + 2) {
                life_set_all_doors(ship, touch.px < DOORBTN_X + DOORBTN_W);
            } else if (touch.py >= DOORBTN_Y && touch.py < DOORBTN_Y + DOORBTN_H && touch.px >= STATIONBTN_X &&
                       touch.px < STATIONBTN_X + 2 * DOORBTN_W + 2) {
                if (touch.px < STATIONBTN_X + DOORBTN_W)
                    crew_save_stations(crew);
                else
                    crew_return_to_stations(crew, ship);
                sound_play("hoverBeep");
            } else if (selected_crystal(crew) >= 0 && touch.py >= LOCKBTN_Y && touch.py < LOCKBTN_Y + LOCKBTN_H &&
                       touch.px >= LOCKBTN_X) {
                if (!combat_crystal_lockdown(&combat, SIDE_PLAYER, selected_crystal(crew))) sound_play("powerUpFail");
            } else if (touch.py >= DOORBTN_Y && touch.py < DOORBTN_Y + 16 && touch.px >= ALLBTN_X) {
                crew->selected = 0;
                for (int i = 0; i < crew->count; i++)
                    if (!crew->crew[i].foreign && !crew->crew[i].drone) crew->selected |= 1u << i;
                sound_play("hoverBeep");
            } else if (touch.py >= MAP_H) {
                int ci = bar_crew(crew, touch.px / bar_slot_width(crew));
                if (ci >= 0) crew->selected = 1u << ci;
            } else if (touch.py >= MAP_TOP) {
                // The map acts on release: a short touch is a tap, a longer drag draws a box.
                mapTouch = true;
                dragging = false;
                dragX0 = dragX1 = touch.px;
                dragY0 = dragY1 = touch.py;
            }
        }
        if (beamAiming && (held & KEY_TOUCH)) {
            touchPosition touch;
            read_touch(&touch);
            dragX1 = touch.px;
            dragY1 = touch.py;
        }
        if (beamAiming && (hidKeysUp() & KEY_TOUCH)) {
            beamAiming = false;
            if (foe && selectedWeapon >= 0) {
                float x0, y0, x1, y1;
                to_ship(&emap, dragX0, dragY0, &x0, &y0);
                to_ship(&emap, dragX1, dragY1, &x1, &y1);
                // A short drag (a tap) aims the beam to the right of where you tapped.
                float angle = hypotf(dragX1 - dragX0, dragY1 - dragY0) > 6 ? atan2f(y1 - y0, x1 - x0) : 0.0f;
                combat_set_beam(&combat, selectedWeapon, x0, y0, angle);
                if (ship->weapons[selectedWeapon].beamAimed) {
                    sound_play("hoverBeep");
                    selectedWeapon = -1;
                }
            }
        }
        if (mapTouch && (held & KEY_TOUCH) && tab == TAB_CREW) {
            touchPosition touch;
            read_touch(&touch);
            dragX1 = touch.px;
            dragY1 = touch.py < MAP_TOP ? MAP_TOP : touch.py > MAP_H ? MAP_H : touch.py;
            if (fabsf(dragX1 - dragX0) > 6 || fabsf(dragY1 - dragY0) > 6) dragging = true;
        }
        if (mapTouch && (hidKeysUp() & KEY_TOUCH)) {
            mapTouch = false;
            if (tab == TAB_CREW && dragging) {
                // Select everyone inside the box.
                float x0 = dragX0 < dragX1 ? dragX0 : dragX1, x1 = dragX0 < dragX1 ? dragX1 : dragX0;
                float y0 = dragY0 < dragY1 ? dragY0 : dragY1, y1 = dragY0 < dragY1 ? dragY1 : dragY0;
                crew->selected = 0;
                for (int i = 0; i < crew->count; i++) {
                    float X, Y;
                    to_screen(&map, ship->xOffset * TILE + crew->crew[i].x, ship->yOffset * TILE + crew->crew[i].y, &X, &Y);
                    if (!crew->crew[i].foreign && !crew->crew[i].drone && X >= x0 && X <= x1 && Y >= y0 && Y <= y1)
                        crew->selected |= 1u << i;
                }
            } else if (tab == TAB_CREW) {
                // Ship coordinates relative to the room grid.
                float px, py;
                to_ship(&map, dragX0, dragY0, &px, &py);
                px -= ship->xOffset * TILE;
                py -= ship->yOffset * TILE;
                int picked = crew_pick(crew, px, py, 14.0f / map.zoom > 18.0f ? 14.0f / map.zoom : 18.0f);
                int room = ship_room_at(ship, (int)floorf(px / TILE), (int)floorf(py / TILE));
                // Doors need a precise tap so they don't steal taps meant for the rooms beside them.
                float doorR = 5.0f / map.zoom < 10.0f ? 5.0f / map.zoom : 10.0f;
                int door = picked < 0 ? door_pick(ship, px, py, doorR) : -1;
                // With crew selected, tapping someone else's tile sends the selection to that room
                // (pick other crew with the crew bar, a drag box, or B to clear the selection).
                if (picked >= 0 && crew->selected && !crew_is_selected(crew, picked) && room >= 0)
                    crew_order_selected(crew, ship, room);
                else if (picked >= 0)
                    crew->selected = 1u << picked;
                else if (door >= 0)
                    life_toggle_door(ship, door);
                else if (crew->selected && room >= 0)
                    crew_order_selected(crew, ship, room);
                else
                    crew->selected = 0;
            }
            dragging = false;
        }

        float topW = tab == TAB_ARMS || (!foe && !(sVisitor && enemy.loaded)) ? TOP_W : ENEMY_X;
        if (cam.vw != topW) {
            cam.vw = topW;
            camera_fit(&cam, ship, 8, false);
        }

        // Music: the sector's track in a run, crossfading to its battle version while a hostile ship is here.
        if (sInRun && sound_music_ended() && sRun.trackCount > 1) {  // next track in the sector's playlist
            sRun.trackIndex = (sRun.trackIndex + 1) % sRun.trackCount;
            snprintf(sRun.track, sizeof(sRun.track), "%s", sRun.tracks[sRun.trackIndex]);
        }
        if (sInRun) sound_music(sRun.track);
        sound_update(dt);
        // Battle music (xftl doc/music): a hostile ship, intruders, or mind-controlled crew.
        bool battle = haveEnemy && combat.result == COMBAT_FIGHTING;
        for (int i = 0; i < crew->count && !battle; i++)
            battle = (crew->crew[i].foreign && !crew->crew[i].drone) || crew->crew[i].mind > 0.0f;
        sound_music_battle(battle);

        bool frozen = paused || sTestHold > 0.0f ||
                      (sInRun && (sRun.view.active || sMapOpen || sSectorMapOpen || sStoreOpen || sShipOpen));
        // Surrender and escape (xftl ShipAI): checked as the enemy's hull drops; an escaping ship
        // charges its FTL while its piloting and engines work, and leaves when done.
        if (sInRun && !frozen && haveEnemy && combat.result == COMBAT_FIGHTING && foe && foe->hullHp > 0) {
            if (sSurrenderHp > 0 && foe->hullHp <= sSurrenderHp) {
                sSurrenderHp = 0;
                run_ship_event(&sRun, RCTX, "surrender");
            } else if (sEscapeHp > 0 && foe->hullHp <= sEscapeHp) {
                sEscapeHp = 0;
                sEscapeLeft = sEscapeTime * (PLAYER_SHIP->aug[AUG_FTL_JAMMER] ? 2.0f : 1.0f);  // FTL Jammer
                run_ship_event(&sRun, RCTX, "escape");
            } else if (sEscapeLeft >= 0.0f) {
                if (combat_can_charge_ftl(&combat, SIDE_ENEMY)) sEscapeLeft -= dt;
                if (sEscapeLeft < 0.0f) {  // it got away, with any of our boarders
                    sound_play("jumpLeave");
                    strand_boarders(ship, crew, foeCrew);
                    END_FIGHT();
                    sVisitor = false;  // it jumped away
                    if (!run_ship_event(&sRun, RCTX, "gotaway")) {
                        memset(&sRun.view, 0, sizeof(sRun.view));
                        sRun.view.active = true;
                        snprintf(sRun.view.text, sizeof(sRun.view.text), "The enemy ship jumped away.");
                        sRun.view.choiceCount = 1;
                        snprintf(sRun.view.choiceText[0], sizeof(sRun.view.choiceText[0]), "Continue...");
                    }
                    sRun.view.shipDef = NULL;
                }
            }
        }
        if (!frozen && (haveEnemy ? combat.result == COMBAT_FIGHTING : env_dangerous(sEnvNow)) && sFtlCharge < 1.0f) {
            // Charges at the engines' rate while combat_can_charge_ftl allows it. With only a
            // hazard it takes 23 s at the slowest, sped up by the same engine, crew and booster
            // factors (wiki: Engines).
            int eng = ship_find_system(ship, SYS_ENGINES);
            int ep = eng >= 0 ? ship_system_power(ship, eng) : 0;
            float hazardScale = haveEnemy ? 1.0f : 23.0f / FTL_TIME[0][0];
            if (ep > 0 && combat_can_charge_ftl(&combat, SIDE_PLAYER))
                sFtlCharge += dt / (hazardScale * FTL_TIME[ship->systems[eng].manned ? 1 + ship->systems[eng].manLevel : 0][(ep > 8 ? 8 : ep) - 1]) *
                              (1.0f + 0.25f * ship->aug[AUG_FTL_BOOST]);
            if (sFtlCharge > 1.0f) sFtlCharge = 1.0f;
        }
        if (!frozen) {
            crew_update(crew, ship, dt);
            life_update(ship, crew, dt);
            life_take_dead_boarders(NULL, 0);  // enemy boarders who died here: the enemy has no clone queue
            if (foe) {
                crew_update(foeCrew, foe, dt);
                life_update(foe, foeCrew, dt);
                // Our boarders who died over there go into our clone bay's queue.
                DeadBoarder dead[8];
                int nd = life_take_dead_boarders(dead, 8);
                int cb = ship_find_system(ship, SYS_CLONEBAY);
                for (int k = 0; k < nd; k++) {
                    if (cb < 0 || (ship_system_capacity(ship, cb) <= 0 && !ship->aug[AUG_BACKUP_DNA]) || crew->cloneCount >= MAX_CREW) break;
                    bool woman = dead[k].female && !strcmp(dead[k].race, "human");
                    int ri = crew_race_look(crew, dead[k].race, woman ? "female" : dead[k].race);
                    if (ri < 0) continue;
                    crew->cloneQueue[crew->cloneCount].race = ri;
                    crew->cloneQueue[crew->cloneCount].female = dead[k].female;
                    memcpy(crew->cloneQueue[crew->cloneCount].name, dead[k].name, sizeof(crew->cloneQueue[0].name));
                    memcpy(crew->cloneQueue[crew->cloneCount].xp, dead[k].xp, sizeof(dead[k].xp));
                    crew->cloneCount++;
                }
            }
            combat_update(&combat, dt);
            bool anyOurs = crew->cloneCount > 0;
            for (int i = 0; i < crew->count && !anyOurs; i++)
                anyOurs = !crew->crew[i].drone && crew->crew[i].foreign == (crew->crew[i].mind > 0.0f);
            if (sInRun && !haveEnemy && combat.result == COMBAT_FIGHTING && !anyOurs) combat.result = COMBAT_LOST;
        }
        // Rarely-seen art, loaded just before it's needed: the enemy's pieces once its hull is
        // down to a third (or it blows up anyway), ours if we're destroyed, cloak glows on first use.
        if (foe && (foe->deathTime >= 0.0f || foe->hullHp * 3 <= foe->maxHull)) ship_load_gibs(foe);
        if (ship->deathTime >= 0.0f) ship_load_gibs(ship);
        if (ship->cloakTime > 0.0f) ship_load_cloak(ship);
        if (foe && foe->cloakTime > 0.0f) ship_load_cloak(foe);

        // The ship in the enemy panel: the one we're fighting, or a visitor.
        Ship* shown = foe ? foe : sVisitor && enemy.loaded ? &enemy.ships[enemy.cur] : NULL;
        CrewSet* shownCrew = foe ? foeCrew : shown ? &enemy.crews[enemy.cur] : NULL;
        if (shown && !foe && !frozen) crew_update(shownCrew, shown, dt);
        update_visibility(ship, crew, shown, shownCrew);
        check_achievements(&combat, ship, crew, foe, foeCrew);
        popup_update(dt, blueprints, bpCount);

        // Top screen: the player's ship, then the enemy panel over its right-hand side. On the arms
        // tab the enemy is on the touch screen, so our ship takes the whole top screen and enemy
        // fire comes in from the right edge.
        bool split = tab != TAB_ARMS && shown;  // no ship there: ours gets the whole screen
        backdrop_sync(&sRun);  // may wait for the GPU, so before the frame starts
        // Stereoscopic 3D with the slider up: the top screen once per eye, the stars set back
        // behind the screen and the readouts floating a little in front of it.
        float slider = osGet3DSliderState();
        bool stereo = slider > 0.0f;
        gfxSet3D(stereo);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TextBufClear(textBuf);
        clouds_render_layer();
        for (int eye = 0; eye < (stereo ? 2 : 1); eye++) {
        C3D_RenderTarget* topEye = eye ? topRight : top;
        float eyeSign = eye ? 1.0f : -1.0f;  // depth > 0 is behind the screen
#define STEREO_DEPTH(d)                                           \
    do {                                                          \
        C2D_ViewReset();                                          \
        C2D_ViewTranslate(eyeSign * slider * (d), 0.0f);          \
    } while (0)
        C2D_TargetClear(topEye, C2D_Color32(8, 10, 20, 255));
        C2D_SceneBegin(topEye);
        STEREO_DEPTH(STEREO_FAR);
        draw_backdrop(&sRun, 0, 0, TOP_W, TOP_H, 0.0f);
        STEREO_DEPTH(0.0f);
        float camX = cam.x;
        cam.x -= jump_offset() / cam.zoom;
        if (split) clip_top(0, ENEMY_X);
        draw_shields(ship, &cam, combat.shieldFlash[SIDE_PLAYER]);
        draw_ship(ship, &cam);
        draw_weapons(ship, &cam);
        draw_crew(ship, crew, &cam);
        draw_effects(&combat, SIDE_PLAYER, &cam);
        draw_beams(&combat, SIDE_PLAYER, &cam);
        draw_drones(&combat, SIDE_PLAYER, &cam);
        if (split) {
            clip_top(ENEMY_X, TOP_W);
            C2D_DrawRectSolid(ENEMY_X, 0, 0, ENEMY_W, TOP_H, C2D_Color32(12, 15, 28, 150));
            if (shown) {
                draw_ship(shown, &ecam);
                draw_weapons(shown, &ecam);
                draw_crew(shown, shownCrew, &ecam);
                draw_shields(shown, &ecam, foe ? combat.shieldFlash[SIDE_ENEMY] : 0.0f);
            }
            if (foe) draw_effects(&combat, SIDE_ENEMY, &ecam);
            if (foe) draw_beams(&combat, SIDE_ENEMY, &ecam);
            if (foe) draw_drones(&combat, SIDE_ENEMY, &ecam);
            // What our weapons are aimed at, as in FTL, whichever tab is open below.
            if (foe) draw_target_markers(ship, foe, &combat, &ecam, selectedWeapon, ENEMY_X, 50, TOP_W, TOP_H);
            clip_off();
            C2D_DrawRectSolid(ENEMY_X, 0, 0, 1, TOP_H, C2D_Color32(60, 70, 95, 255));
        }
        const Camera* cams[2] = {&cam, split ? &ecam : NULL};
        if (foe) draw_projectiles(&combat, cams, TOP_W + 30);
        cam.x = camX;
        draw_jump_fx();
        STEREO_DEPTH(STEREO_NEAR);
        draw_top_hud(ship, split ? shown : NULL, &combat, paused);
        draw_env_hud(&combat);
        {
            unsigned lf = linearSpaceFree();
            if (lf < sMinLinearFree) sMinLinearFree = lf;
        }
        if (debugOpen || sShowMemory) draw_memory_top(ship, foe);
        if (!debugOpen && !(sInRun && sRun.view.active) && !sStoreOpen && !sShipOpen) draw_skill_panel(crew);
        if (sInRun && sRun.view.active && !debugOpen) draw_event_top(&sRun.view);
        draw_popup(blueprints, bpCount);
        if ((sStoreOpen || sShipOpen) && !debugOpen) draw_info_top();
        C2D_ViewReset();
#undef STEREO_DEPTH
        }

        C2D_TargetClear(bottom, C2D_Color32(8, 10, 20, 255));
        C2D_SceneBegin(bottom);
        if (debugOpen) {
            char sname[64];
            snprintf(sname, sizeof(sname), "%d %s", dbgShip + 1, blueprints[dbgShip].name + 12);
            draw_debug(sname, enemyCount ? text_get(enemyBps[dbgEnemy].classId, enemyBps[dbgEnemy].name) : "-", &combat);
        } else if (sInRun && sRun.view.active) {
            draw_event_choices(&sRun, &sRun.view);
        } else if (sMapOpen) {
            draw_run_map(&sRun);
        } else if (sSectorMapOpen) {
            draw_sector_map(&sRun);
        } else if (sStoreOpen) {
            draw_store(&sRun, ship);
        } else if (sShipOpen) {
            draw_ship_screen(&sRun, ship);
        } else if (tab == TAB_CREW) {
            draw_backdrop(&sRun, 0, 0, BOT_W, BOT_H, 0.5f);
            draw_crew_tab(ship, crew, &map);
            draw_effects(&combat, SIDE_PLAYER, &map);
            draw_beams(&combat, SIDE_PLAYER, &map);
            draw_drones(&combat, SIDE_PLAYER, &map);
            if (dragging) {
                float x0 = dragX0 < dragX1 ? dragX0 : dragX1, y0 = dragY0 < dragY1 ? dragY0 : dragY1;
                float w = fabsf(dragX1 - dragX0), h = fabsf(dragY1 - dragY0);
                u32 edge = SELECT_COL;
                C2D_DrawRectSolid(x0, y0, 0, w, h, C2D_Color32(255, 225, 40, 40));
                C2D_DrawRectSolid(x0, y0, 0, w, 1, edge);
                C2D_DrawRectSolid(x0, y0 + h - 1, 0, w, 1, edge);
                C2D_DrawRectSolid(x0, y0, 0, 1, h, edge);
                C2D_DrawRectSolid(x0 + w - 1, y0, 0, 1, h, edge);
            }
        } else if (tab == TAB_ARMS) {
            draw_backdrop(&sRun, 0, 0, BOT_W, BOT_H, 0.5f);
            draw_arms_tab(ship, foe, foeCrew, &combat, &emap, selectedWeapon);
            if (pickMode) {
                static const char* const hints[] = {"", "Tap a room to send the crew in your teleporter room",
                                                    "Tap a system to attach the hacking drone",
                                                    "Tap a room to mind-control someone in it"};
                C2D_DrawRectSolid(0, CARDS_TOP - 16, 0, BOT_W, 14, C2D_Color32(40, 70, 110, 230));
                draw_text(4, CARDS_TOP - 16, 0.4f, C2D_Color32(235, 235, 235, 255), hints[pickMode]);
            }
            if (beamAiming && selectedWeapon >= 0) {
                // Preview: the beam's full length along the drag.
                float len = ship->weapons[selectedWeapon].def->length * emap.zoom;
                float dx = dragX1 - dragX0, dy = dragY1 - dragY0, d = hypotf(dx, dy);
                if (d < 1) {
                    dx = 1;
                    dy = 0;
                    d = 1;
                }
                if (foe) {
                    float sx0, sy0, sx1, sy1;
                    to_ship(&emap, dragX0, dragY0, &sx0, &sy0);
                    to_ship(&emap, dragX0 + dx, dragY0 + dy, &sx1, &sy1);
                    float ang = d > 1 ? atan2f(sy1 - sy0, sx1 - sx0) : 0.0f;
                    draw_beam_rooms(foe, &emap, sx0, sy0, ang, ship->weapons[selectedWeapon].def->length, 120);
                }
                u32 col = C2D_Color32(255, 220, 60, 230);
                C2D_DrawLine(dragX0, dragY0, col, dragX0 + dx / d * len, dragY0 + dy / d * len, col, 2.5f, 0);
                C2D_DrawCircleSolid(dragX0, dragY0, 0, 3.5f, col);
            }
        } else if (tab == TAB_SYS) {
            draw_sys_tab(ship, crew, &combat);
        } else {
            draw_power_tab(ship);
        }
        if (debugOpen || !((sInRun && sRun.view.active) || sMapOpen || sSectorMapOpen || sStoreOpen || sShipOpen))
            draw_tabs(ship, tab, index, bpCount, paused);
        if (sound_error() && now - startTime < 8000) {
            C2D_DrawRectSolid(0, TAB_H, 0, BOT_W, 14, C2D_Color32(90, 30, 30, 255));
            draw_text(4, TAB_H + 1, 0.4f, C2D_Color32(255, 220, 220, 255), sound_error());
        }
        C3D_FrameEnd(0);
    }

    gpu_idle();
    texture_free(&sPopupIcon);
    ach_icons(false);
    profile_save();
    C2D_TextBufDelete(textBuf);
    texture_free(&sBgTex);
    texture_free(&sPlanetTex);
    font_exit();
    slots_free(&player);
    slots_free(&enemy);
    texture_free(&sExplosion.tex);
    texture_free(&sFireAnim.tex);
    texture_free(&sBreachAnim.tex);
    names_free(&names);
    text_free();
    run_free();
    items_free();
    weapons_free();
    animdb_close(&anims);
    sound_exit();  // before the archive closes
    ftldat_close(&dat);

done:
    sound_exit();
    if (romfs) romfsExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
