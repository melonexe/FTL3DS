#include "font.h"

#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "texture.h"

typedef struct {
    uint32_t ch;
    int x, y, w, h, ascent;
    float prekern, postkern;
} Glyph;

typedef struct {
    Texture tex;
    Glyph* glyphs;  // sorted by ch
    int count;
    int lineSpacing, lineTop;
    Tex3DS_SubTexture sub;  // reused per glyph
} BitFont;

enum { F_SMALL, F_BODY, F_TITLE, F_COUNT };
// lineTop: how far the top of a line sits above the baseline (xftl font-data-override.xml).
static const struct {
    const char* file;
    int lineTop;
} FILES[F_COUNT] = {{"fonts/JustinFont8.font", 8}, {"fonts/JustinFont10.font", 10}, {"fonts/JustinFont12Bold.font", 0}};
static BitFont sFonts[F_COUNT];
static bool sReady;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int be16(const uint8_t* p) { return (int16_t)(p[0] << 8 | p[1]); }
static int ube16(const uint8_t* p) { return p[0] << 8 | p[1]; }

static int cmp_glyph(const void* a, const void* b) {
    uint32_t x = ((const Glyph*)a)->ch, y = ((const Glyph*)b)->ch;
    return x < y ? -1 : x > y;
}

static bool load(BitFont* f, FtlDat* dat, const char* path, int lineTop) {
    uint32_t size;
    uint8_t* d = ftldat_read(dat, path, &size);
    if (!d || size < 24 || memcmp(d, "FONT", 4) || d[4] != 1) {
        free(d);
        return false;
    }
    f->lineSpacing = d[5];
    f->lineTop = lineTop > 0 ? lineTop : d[6];
    uint32_t infoOff = be32(d + 8);
    int count = ube16(d + 12), infoSize = ube16(d + 14);
    uint32_t texOff = be32(d + 16);
    f->glyphs = malloc(sizeof(Glyph) * count);
    if (!f->glyphs || infoOff + (uint32_t)count * infoSize > size) {
        free(d);
        return false;
    }
    for (int i = 0; i < count; i++) {
        const uint8_t* p = d + infoOff + i * infoSize;
        Glyph* g = &f->glyphs[i];
        g->ch = be32(p);
        g->x = be16(p + 4);
        g->y = be16(p + 6);
        g->w = p[8];
        g->h = p[9];
        g->ascent = (int8_t)p[10];
        g->prekern = be16(p + 12) / 256.0f;
        g->postkern = be16(p + 14) / 256.0f;
    }
    f->count = count;
    qsort(f->glyphs, count, sizeof(Glyph), cmp_glyph);
    // "TEX\n" v2, format 64 (8-bit alpha), no mipmaps.
    const uint8_t* t = d + texOff;
    if (texOff + 32 > size || memcmp(t, "TEX\n", 4) || t[4] != 2 || t[5] != 64) {
        free(d);
        return false;
    }
    int w = ube16(t + 8), h = ube16(t + 10);
    uint32_t pixOff = be32(t + 16);
    if (texOff + pixOff + (uint32_t)w * h > size) {
        free(d);
        return false;
    }
    uint8_t* rgba = malloc((size_t)w * h * 4);
    if (!rgba) {
        free(d);
        return false;
    }
    const uint8_t* a = t + pixOff;
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = a[i];
    }
    bool ok = texture_from_rgba(&f->tex, rgba, w, h);
    free(rgba);
    free(d);
    if (ok) C3D_TexSetFilter(f->tex.image.tex, GPU_NEAREST, GPU_NEAREST);
    return ok;
}

bool font_init(FtlDat* dat) {
    sReady = true;
    for (int i = 0; i < F_COUNT; i++) sReady &= load(&sFonts[i], dat, FILES[i].file, FILES[i].lineTop);
    return sReady;
}

void font_exit(void) {
    for (int i = 0; i < F_COUNT; i++) {
        texture_free(&sFonts[i].tex);
        free(sFonts[i].glyphs);
    }
    memset(sFonts, 0, sizeof(sFonts));
    sReady = false;
}

static const BitFont* pick(float scale) { return &sFonts[scale >= 0.5f ? F_TITLE : scale >= 0.42f ? F_BODY : F_SMALL]; }

static const Glyph* glyph(const BitFont* f, uint32_t cp) {
    Glyph key = {.ch = cp};
    const Glyph* g = bsearch(&key, f->glyphs, f->count, sizeof(Glyph), cmp_glyph);
    if (!g && cp != '?') g = glyph(f, '?');
    return g;
}

static float advance(const Glyph* g) { return g ? roundf(g->prekern) + g->w + roundf(g->postkern) : 0.0f; }

float font_char_width(float scale, uint32_t cp) {
    if (!sReady) return fontGetCharWidthInfo(NULL, fontGlyphIndexFromCodePoint(NULL, cp))->charWidth * scale;
    const BitFont* f = pick(scale);
    return cp == ' ' && !glyph(f, ' ') ? 3.0f : advance(glyph(f, cp));
}

float font_line_height(float scale) { return sReady ? (float)pick(scale)->lineSpacing : fontGetInfo(NULL)->lineFeed * scale; }

float font_cap_height(float scale) { return sReady ? (float)pick(scale)->lineTop : 12.0f * scale; }

float font_width(float scale, const char* text) {
    float w = 0.0f, best = 0.0f;
    for (const char* p = text; *p;) {
        uint32_t cp;
        ssize_t n = decode_utf8(&cp, (const uint8_t*)p);
        if (n <= 0) {
            n = 1;
            cp = '?';
        }
        p += n;
        if (cp == '\n') {
            if (w > best) best = w;
            w = 0.0f;
            continue;
        }
        w += font_char_width(scale, cp);
    }
    return w > best ? w : best;
}

void font_draw(float x, float y, float scale, u32 color, const char* text, float squeeze) {
    if (!sReady) return;
    BitFont* f = (BitFont*)pick(scale);
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, color, 1.0f);
    float tw = f->tex.image.tex->width, th = f->tex.image.tex->height;
    float px = roundf(x), base = roundf(y) + f->lineTop;
    for (const char* p = text; *p;) {
        uint32_t cp;
        ssize_t n = decode_utf8(&cp, (const uint8_t*)p);
        if (n <= 0) {
            n = 1;
            cp = '?';
        }
        p += n;
        if (cp == '\n') {
            px = roundf(x);
            base += f->lineSpacing;
            continue;
        }
        const Glyph* g = glyph(f, cp);
        if (!g) continue;
        float pre = roundf(g->prekern) * squeeze;
        if (g->w && g->h) {
            f->sub.width = g->w;
            f->sub.height = g->h;
            f->sub.left = g->x / tw;
            f->sub.right = (g->x + g->w) / tw;
            f->sub.top = 1.0f - g->y / th;
            f->sub.bottom = 1.0f - (g->y + g->h) / th;
            C2D_Image img = {f->tex.image.tex, &f->sub};
            C2D_DrawImageAt(img, roundf(px + pre), base - g->ascent, 0.0f, &tint, squeeze, 1.0f);
        }
        px += advance(g) * squeeze;
    }
}
