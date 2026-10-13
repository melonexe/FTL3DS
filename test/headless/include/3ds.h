#pragma once
// Headless PC build (test/headless/Makefile): stand-ins for the parts of libctru, citro3d and citro2d the game
// uses. Nothing is drawn, played or read from buttons: the clock is simulated (one 60 Hz frame per
// C3D_FrameEnd), input comes from the scenario runner, and threads run their work at once.
// Implemented in test/headless/stubs.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef s32 Result;
typedef u32 Handle;

#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)
#define CUR_THREAD_HANDLE 0xFFFF8000
#ifndef U64_MAX
#define U64_MAX UINT64_MAX
#endif

// ---- System
#include <sys/types.h>  // ssize_t
bool aptMainLoop(void);
u32 envGetHeapSize(void);
u32 envGetLinearHeapSize(void);
u32 vramSpaceFree(void);
ssize_t decode_utf8(uint32_t* out, const uint8_t* in);  // bytes read, or -1
u64 osGetTime(void);
float osGet3DSliderState(void);
size_t linearSpaceFree(void);
Result romfsInit(void);
Result romfsExit(void);
void svcSleepThread(s64 ns);
Result svcGetThreadPriority(s32* out, Handle thread);
Result GSPGPU_FlushDataCache(const void* adr, u32 size);

typedef struct PcThread* Thread;
typedef void (*ThreadFunc)(void*);
Thread threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached);
Result threadJoin(Thread t, u64 timeout);
void threadFree(Thread t);

typedef int LightLock;
static inline void LightLock_Init(LightLock* l) { *l = 0; }
static inline void LightLock_Lock(LightLock* l) { (void)l; }
static inline void LightLock_Unlock(LightLock* l) { (void)l; }
typedef int LightEvent;

// ---- Screens
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
void gfxInitDefault(void);
void gfxExit(void);
void gfxSet3D(bool enable);
void gspWaitForVBlank(void);
void* consoleInit(gfxScreen_t screen, void* console);

// ---- Input
enum {
    KEY_A = 1 << 0, KEY_B = 1 << 1, KEY_SELECT = 1 << 2, KEY_START = 1 << 3, KEY_DRIGHT = 1 << 4,
    KEY_DLEFT = 1 << 5, KEY_DUP = 1 << 6, KEY_DDOWN = 1 << 7, KEY_R = 1 << 8, KEY_L = 1 << 9,
    KEY_X = 1 << 10, KEY_Y = 1 << 11, KEY_ZL = 1 << 14, KEY_ZR = 1 << 15, KEY_TOUCH = 1 << 20,
};
typedef struct {
    u16 px, py;
} touchPosition;
void hidScanInput(void);
u32 hidKeysDown(void);
u32 hidKeysHeld(void);
u32 hidKeysUp(void);
void hidTouchRead(touchPosition* pos);

// ---- System font (font.c's fallback before FTL's fonts load)
typedef struct {
    u8 lineFeed;
} FINF_s;
typedef struct {
    u8 left, glyphWidth, charWidth;
} charWidthInfo_s;
FINF_s* fontGetInfo(void* font);
charWidthInfo_s* fontGetCharWidthInfo(void* font, int glyphIndex);
int fontGlyphIndexFromCodePoint(void* font, u32 codePoint);

// ---- GPU (citro3d)
typedef enum { GPU_RGBA8 = 0, GPU_RGB8, GPU_RGBA5551, GPU_RGB565, GPU_RGBA4, GPU_ETC1 = 12, GPU_ETC1A4 = 13 } GPU_TEXCOLOR;
typedef enum { GPU_NEAREST = 0, GPU_LINEAR = 1 } GPU_TEXTURE_FILTER_PARAM;
typedef enum { GPU_CLAMP_TO_EDGE = 0, GPU_CLAMP_TO_BORDER, GPU_REPEAT, GPU_MIRRORED_REPEAT } GPU_TEXTURE_WRAP_PARAM;
typedef enum { GPU_TEXFACE_2D = 0 } GPU_TEXFACE;
typedef enum { GPU_BLEND_ADD = 0 } GPU_BLENDEQUATION;
typedef enum { GPU_ZERO = 0, GPU_ONE, GPU_SRC_ALPHA = 6, GPU_ONE_MINUS_SRC_ALPHA = 7 } GPU_BLENDFACTOR;
typedef enum { GPU_SCISSOR_DISABLE = 0, GPU_SCISSOR_INVERT, GPU_SCISSOR_NORMAL = 3 } GPU_SCISSORMODE;

typedef struct {
    void* data;
    GPU_TEXCOLOR fmt;
    size_t size;
    u16 width, height;
} C3D_Tex;
typedef struct C3D_RenderTarget C3D_RenderTarget;

#define C3D_DEFAULT_CMDBUF_SIZE 0x40000
#define C3D_FRAME_SYNCDRAW 1
bool C3D_Init(size_t cmdBufSize);
void C3D_Fini(void);
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u8 flags);
float C3D_GetDrawingTime(void);
float C3D_GetProcessingTime(void);
bool C3D_TexInit(C3D_Tex* tex, u16 width, u16 height, GPU_TEXCOLOR format);
bool C3D_TexInitVRAM(C3D_Tex* tex, u16 width, u16 height, GPU_TEXCOLOR format);
void C3D_TexDelete(C3D_Tex* tex);
static inline void C3D_TexSetFilter(C3D_Tex* t, GPU_TEXTURE_FILTER_PARAM mag, GPU_TEXTURE_FILTER_PARAM min) {
    (void)t, (void)mag, (void)min;
}
static inline void C3D_TexSetWrap(C3D_Tex* t, GPU_TEXTURE_WRAP_PARAM s, GPU_TEXTURE_WRAP_PARAM w) { (void)t, (void)s, (void)w; }
static inline void C3D_AlphaBlend(GPU_BLENDEQUATION c, GPU_BLENDEQUATION a, GPU_BLENDFACTOR s1, GPU_BLENDFACTOR d1,
                                  GPU_BLENDFACTOR s2, GPU_BLENDFACTOR d2) {
    (void)c, (void)a, (void)s1, (void)d1, (void)s2, (void)d2;
}
static inline void C3D_SetScissor(GPU_SCISSORMODE m, u32 l, u32 t, u32 r, u32 b) { (void)m, (void)l, (void)t, (void)r, (void)b; }
C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, int depthFmt);
void C3D_RenderTargetDelete(C3D_RenderTarget* target);

// ---- tex3ds
typedef struct {
    u16 width, height;
    float left, top, right, bottom;
} Tex3DS_SubTexture;
typedef struct Tex3DS_Texture_s* Tex3DS_Texture;
typedef struct C3D_TexCube C3D_TexCube;
static inline Tex3DS_Texture Tex3DS_TextureImport(const void* in, size_t size, C3D_Tex* tex, C3D_TexCube* cube, bool vram) {
    (void)in, (void)size, (void)tex, (void)cube, (void)vram;
    return NULL;  // no GPU formats here: callers fall back to decoding PNGs
}
static inline const Tex3DS_SubTexture* Tex3DS_GetSubTexture(Tex3DS_Texture t, size_t i) {
    (void)t, (void)i;
    return NULL;
}
static inline void Tex3DS_TextureFree(Tex3DS_Texture t) { (void)t; }

// ---- 2D drawing (citro2d): everything draws nothing
typedef struct {
    C3D_Tex* tex;
    const Tex3DS_SubTexture* subtex;
} C2D_Image;
typedef struct {
    u32 color;
    float blend;
} C2D_Tint;
typedef struct {
    C2D_Tint corners[4];
} C2D_ImageTint;
typedef enum { C2D_TintSolid, C2D_TintMult, C2D_TintLuma } C2D_TintMode;
typedef struct C2D_TextBuf_s* C2D_TextBuf;

#define C2D_DEFAULT_MAX_OBJECTS 4096
static inline u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a) { return r | (g << 8) | (b << 16) | ((u32)a << 24); }
bool C2D_Init(size_t maxObjects);
void C2D_Fini(void);
void C2D_Prepare(void);
void C2D_Flush(void);
C3D_RenderTarget* C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side);
static inline void C2D_TargetClear(C3D_RenderTarget* t, u32 c) { (void)t, (void)c; }
static inline void C2D_SceneBegin(C3D_RenderTarget* t) { (void)t; }
static inline void C2D_ViewReset(void) {}
static inline void C2D_ViewTranslate(float x, float y) { (void)x, (void)y; }
static inline void C2D_SetTintMode(C2D_TintMode m) { (void)m; }
static inline void C2D_PlainImageTint(C2D_ImageTint* t, u32 color, float blend) {
    for (int i = 0; i < 4; i++) t->corners[i] = (C2D_Tint){color, blend};
}
static inline void C2D_AlphaImageTint(C2D_ImageTint* t, float alpha) {
    C2D_PlainImageTint(t, C2D_Color32(0, 0, 0, (u8)(alpha * 255)), 0.0f);
}
static inline C2D_TextBuf C2D_TextBufNew(size_t n) {
    (void)n;
    return (C2D_TextBuf)1;
}
static inline void C2D_TextBufDelete(C2D_TextBuf b) { (void)b; }
static inline void C2D_TextBufClear(C2D_TextBuf b) { (void)b; }
static inline bool C2D_DrawImageAt(C2D_Image img, float x, float y, float d, const C2D_ImageTint* t, float sx, float sy) {
    (void)img, (void)x, (void)y, (void)d, (void)t, (void)sx, (void)sy;
    return true;
}
static inline bool C2D_DrawImageAtRotated(C2D_Image img, float x, float y, float d, float a, const C2D_ImageTint* t,
                                          float sx, float sy) {
    (void)img, (void)x, (void)y, (void)d, (void)a, (void)t, (void)sx, (void)sy;
    return true;
}
static inline bool C2D_DrawRectSolid(float x, float y, float d, float w, float h, u32 c) {
    (void)x, (void)y, (void)d, (void)w, (void)h, (void)c;
    return true;
}
static inline bool C2D_DrawCircleSolid(float x, float y, float d, float r, u32 c) {
    (void)x, (void)y, (void)d, (void)r, (void)c;
    return true;
}
static inline bool C2D_DrawEllipseSolid(float x, float y, float d, float w, float h, u32 c) {
    (void)x, (void)y, (void)d, (void)w, (void)h, (void)c;
    return true;
}
static inline bool C2D_DrawLine(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float t, float d) {
    (void)x0, (void)y0, (void)c0, (void)x1, (void)y1, (void)c1, (void)t, (void)d;
    return true;
}
static inline bool C2D_DrawTriangle(float x0, float y0, u32 c0, float x1, float y1, u32 c1, float x2, float y2, u32 c2,
                                    float d) {
    (void)x0, (void)y0, (void)c0, (void)x1, (void)y1, (void)c1, (void)x2, (void)y2, (void)c2, (void)d;
    return true;
}
