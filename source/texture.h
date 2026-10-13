#pragma once
// Decodes PNGs from ftl.dat into GPU textures for citro2d.

#include <citro2d.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    C2D_Image image;  // image.tex / image.subtex are owned by this struct
    int width, height;
    bool loaded;
} Texture;

bool texture_load_png(Texture* t, const uint8_t* png, uint32_t size);
// Like texture_load_png, but box-filters the image down by a whole factor so it's no bigger than
// it needs to be to cover minW x minH (and fits the GPU's 1024 limit), e.g. 1280x720 backgrounds.
bool texture_load_png_fit(Texture* t, const uint8_t* png, uint32_t size, int minW, int minH);

// The two halves of texture_load_png_fit: decoding (plain heap memory only, so it can run on
// another thread; *out is malloc'd) and making the GPU texture (main thread).
bool texture_decode_png(const uint8_t* png, uint32_t size, int minW, int minH, uint8_t** out, int* outW, int* outH);
bool texture_from_rgba(Texture* t, const uint8_t* rgba, int w, int h);
// A texture from a .t3x file in memory (tex3ds output, e.g. ETC1 from a personal CIA's backdrop
// pack): copied straight to the GPU, no decoding. Main thread.
bool texture_from_t3x(Texture* t, const void* data, size_t size);

// Smooth (linear) filtering when enlarged too, for soft images drawn bigger than they're stored.
void texture_set_smooth(Texture* t);

// Live textures and the linear memory they hold (for the debug memory readout).
void texture_stats(unsigned* bytes, unsigned* count);
static inline unsigned texture_bytes(const Texture* t) { return t->loaded ? t->image.tex->size : 0; }

// Called when there's no GPU memory for a texture, to free some (e.g. idle sounds) before one more
// try. Main thread only.
extern void (*texture_low_memory)(void);
void texture_free(Texture* t);

// Finds the bounding box of the non-transparent pixels inside the rectangle (rx, ry, rw, rh) of a
// PNG. Bounds are relative to the rectangle, inclusive. Returns false if it's empty or unreadable.
bool texture_png_alpha_bounds(const uint8_t* png, uint32_t size, int rx, int ry, int rw, int rh, int* x0, int* y0,
                              int* x1, int* y1);
