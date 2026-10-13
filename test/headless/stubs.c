// Headless PC build: the libctru / citro3d / citro2d stand-ins declared in include/3ds.h.
//
// Time is simulated: osGetTime() advances one 60 Hz frame at each C3D_FrameEnd, so a run is the
// same on any machine and as fast as the CPU allows. FTL3DS_MAX_SECONDS (default 600) of simulated
// time ends a run that never finishes (aptMainLoop returns false), and the runner reports it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds.h"

static double sNowMs = 1e12;  // like the 3DS: milliseconds since 1900, far from zero
static long sFrames, sMaxFrames = -1;

bool aptMainLoop(void) {
    if (sMaxFrames < 0) {
        const char* s = getenv("FTL3DS_MAX_SECONDS");
        sMaxFrames = (long)((s ? atof(s) : 600.0) * 60.0);
    }
    if (sFrames >= sMaxFrames) {
        fprintf(stderr, "headless: stopped after %ld simulated seconds\n", sMaxFrames / 60);
        return false;
    }
    return true;
}

u64 osGetTime(void) { return (u64)sNowMs; }
u32 envGetHeapSize(void) { return 64u << 20; }
u32 envGetLinearHeapSize(void) { return 32u << 20; }
u32 vramSpaceFree(void) { return 6u << 20; }

ssize_t decode_utf8(uint32_t* out, const uint8_t* in) {
    if (in[0] < 0x80) {
        *out = in[0];
        return 1;
    }
    int n = in[0] >= 0xF0 ? 4 : in[0] >= 0xE0 ? 3 : in[0] >= 0xC0 ? 2 : 0;
    if (!n) return -1;
    uint32_t c = in[0] & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((in[i] & 0xC0) != 0x80) return -1;
        c = (c << 6) | (in[i] & 0x3F);
    }
    *out = c;
    return n;
}
float osGet3DSliderState(void) { return 0.0f; }
size_t linearSpaceFree(void) { return 32u << 20; }  // a New 3DS's headroom, roughly
Result romfsInit(void) { return 0; }
Result romfsExit(void) { return 0; }
void svcSleepThread(s64 ns) { sNowMs += ns / 1e6; }
Result svcGetThreadPriority(s32* out, Handle thread) {
    (void)thread;
    *out = 0x30;
    return 0;
}
Result GSPGPU_FlushDataCache(const void* adr, u32 size) {
    (void)adr, (void)size;
    return 0;
}

// No threads: callers run the work themselves when threadCreate fails (image jobs do).
Thread threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached) {
    (void)entry, (void)arg, (void)stackSize, (void)prio, (void)core, (void)detached;
    return NULL;
}
Result threadJoin(Thread t, u64 timeout) {
    (void)t, (void)timeout;
    return 0;
}
void threadFree(Thread t) { (void)t; }

void gfxInitDefault(void) {}
void gfxExit(void) {}
void gfxSet3D(bool enable) { (void)enable; }
void gspWaitForVBlank(void) {
    sNowMs += 1000.0 / 60.0;
    sFrames++;
}
void* consoleInit(gfxScreen_t screen, void* console) {
    (void)screen;
    return console;
}

// No buttons: scenarios press them through the game's test hooks.
void hidScanInput(void) {}
u32 hidKeysDown(void) { return 0; }
u32 hidKeysHeld(void) { return 0; }
u32 hidKeysUp(void) { return 0; }
void hidTouchRead(touchPosition* pos) { pos->px = pos->py = 0; }

FINF_s* fontGetInfo(void* font) {
    (void)font;
    static FINF_s info = {30};
    return &info;
}
charWidthInfo_s* fontGetCharWidthInfo(void* font, int glyphIndex) {
    (void)font, (void)glyphIndex;
    static charWidthInfo_s w = {0, 12, 13};
    return &w;
}
int fontGlyphIndexFromCodePoint(void* font, u32 codePoint) {
    (void)font;
    return (int)codePoint;
}

bool C3D_Init(size_t cmdBufSize) {
    (void)cmdBufSize;
    return true;
}
void C3D_Fini(void) {}
bool C3D_FrameBegin(u8 flags) {
    (void)flags;
    return true;
}
void C3D_FrameEnd(u8 flags) {
    (void)flags;
    gspWaitForVBlank();  // one frame of simulated time
}
float C3D_GetDrawingTime(void) { return 0.0f; }
float C3D_GetProcessingTime(void) { return 0.0f; }

// Textures hold real memory (the game writes pixels into them), so texture code runs unchanged.
bool C3D_TexInit(C3D_Tex* tex, u16 width, u16 height, GPU_TEXCOLOR format) {
    size_t px = (size_t)width * height;
    size_t size = format == GPU_RGBA8 ? px * 4 : format == GPU_RGB8 ? px * 3 : format == GPU_ETC1 ? px / 2
                  : format == GPU_ETC1A4                                                 ? px
                                                                                         : px * 2;
    memset(tex, 0, sizeof(*tex));
    tex->data = calloc(1, size ? size : 1);
    if (!tex->data) return false;
    tex->fmt = format;
    tex->size = size;
    tex->width = width;
    tex->height = height;
    return true;
}
bool C3D_TexInitVRAM(C3D_Tex* tex, u16 width, u16 height, GPU_TEXCOLOR format) { return C3D_TexInit(tex, width, height, format); }
void C3D_TexDelete(C3D_Tex* tex) {
    free(tex->data);
    tex->data = NULL;
}

struct C3D_RenderTarget {
    int unused;
};
static C3D_RenderTarget sTargets[8];
static int sTargetCount;
static C3D_RenderTarget* new_target(void) { return &sTargets[sTargetCount++ % 8]; }
C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, int depthFmt) {
    (void)tex, (void)face, (void)level, (void)depthFmt;
    return new_target();
}
void C3D_RenderTargetDelete(C3D_RenderTarget* target) { (void)target; }

bool C2D_Init(size_t maxObjects) {
    (void)maxObjects;
    return true;
}
void C2D_Fini(void) {}
void C2D_Prepare(void) {}
void C2D_Flush(void) {}
C3D_RenderTarget* C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side) {
    (void)screen, (void)side;
    return new_target();
}
