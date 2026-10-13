#include "texture.h"

#include <png.h>
#include <stdlib.h>
#include <string.h>

static int next_pow2(int v) {
    int p = 8;  // smallest texture the GPU accepts
    while (p < v) p <<= 1;
    return p;
}

// The 3DS GPU stores textures in 8x8 tiles with Morton (Z-order) pixels inside each tile.
static inline uint32_t tiled_index(uint32_t x, uint32_t y, uint32_t texW) {
    uint32_t tile = ((y >> 3) * (texW >> 3) + (x >> 3)) << 6;
    uint32_t morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
                      ((y & 4) << 3);
    return tile + morton;
}

bool texture_load_png(Texture* t, const uint8_t* png, uint32_t size) { return texture_load_png_fit(t, png, size, 0, 0); }

bool texture_load_png_fit(Texture* t, const uint8_t* png, uint32_t size, int minW, int minH) {
    uint8_t* rgba;
    int w, h;
    memset(t, 0, sizeof(*t));
    if (!texture_decode_png(png, size, minW, minH, &rgba, &w, &h)) return false;
    bool ok = texture_from_rgba(t, rgba, w, h);
    free(rgba);
    return ok;
}

bool texture_decode_png(const uint8_t* png, uint32_t size, int minW, int minH, uint8_t** out, int* outW, int* outH) {

    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, png, size)) return false;
    img.format = PNG_FORMAT_RGBA;

    int w = (int)img.width, h = (int)img.height;
    int f = 1;  // downscale factor
    if (minW > 0 && minH > 0)
        while (w / (f + 1) >= minW && h / (f + 1) >= minH) f++;
    while (w / f > 1024 || h / f > 1024) f++;
    if (f > 1 && (minW <= 0 || minH <= 0)) {
        png_image_free(&img);
        return false;  // too big, and the caller didn't ask for scaling
    }
    uint8_t* rgba = malloc(PNG_IMAGE_SIZE(img));
    if (!rgba || !png_image_finish_read(&img, NULL, rgba, 0, NULL)) {
        free(rgba);
        png_image_free(&img);
        return false;
    }
    if (f > 1) {
        // Average f x f blocks (premultiplied by alpha so edges don't go dark).
        int nw = w / f, nh = h / f;
        uint8_t* small = malloc((size_t)nw * nh * 4);
        if (!small) {
            free(rgba);
            return false;
        }
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                uint32_t acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) {
                        const uint8_t* s = rgba + ((size_t)(y * f + j) * w + (x * f + i)) * 4;
                        acc[0] += s[0] * s[3];
                        acc[1] += s[1] * s[3];
                        acc[2] += s[2] * s[3];
                        acc[3] += s[3];
                    }
                uint8_t* d = small + ((size_t)y * nw + x) * 4;
                for (int c = 0; c < 3; c++) d[c] = acc[3] ? (uint8_t)(acc[c] / acc[3]) : 0;
                d[3] = (uint8_t)(acc[3] / (f * f));
            }
        free(rgba);
        rgba = small;
        w = nw;
        h = nh;
    }
    png_image_free(&img);
    *out = rgba;
    *outW = w;
    *outH = h;
    return true;
}

void (*texture_low_memory)(void);

static unsigned sTexBytes, sTexCount;

void texture_set_smooth(Texture* t) {
    if (t->loaded) C3D_TexSetFilter(t->image.tex, GPU_LINEAR, GPU_LINEAR);
}

void texture_stats(unsigned* bytes, unsigned* count) {
    *bytes = sTexBytes;
    *count = sTexCount;
}

bool texture_from_rgba(Texture* t, const uint8_t* rgba, int w, int h) {
    memset(t, 0, sizeof(*t));

    int texW = next_pow2(w), texH = next_pow2(h);
    C3D_Tex* tex = malloc(sizeof(C3D_Tex));
    Tex3DS_SubTexture* sub = malloc(sizeof(Tex3DS_SubTexture));
    bool ok = tex && sub && C3D_TexInit(tex, texW, texH, GPU_RGBA8);
    if (!ok && tex && sub && texture_low_memory) {  // linear memory is short: make room, try again
        texture_low_memory();
        ok = C3D_TexInit(tex, texW, texH, GPU_RGBA8);
    }
    if (!ok) {
        free(tex);
        free(sub);
        return false;
    }
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_LINEAR);  // crisp when enlarged, smooth when shrunk
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    memset(tex->data, 0, tex->size);

    // GPU_RGBA8 texels are stored as bytes A, B, G, R.
    uint8_t* dst = tex->data;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = rgba + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, src += 4) {
            uint8_t* d = dst + tiled_index(x, y, texW) * 4;
            d[0] = src[3];
            d[1] = src[2];
            d[2] = src[1];
            d[3] = src[0];
        }
    }
    GSPGPU_FlushDataCache(tex->data, tex->size);

    sub->width = (u16)w;
    sub->height = (u16)h;
    sub->left = 0.0f;
    sub->top = 1.0f;
    sub->right = (float)w / texW;
    sub->bottom = 1.0f - (float)h / texH;

    sTexBytes += tex->size;
    sTexCount++;
    t->image.tex = tex;
    t->image.subtex = sub;
    t->width = w;
    t->height = h;
    t->loaded = true;
    return true;
}

bool texture_from_t3x(Texture* t, const void* data, size_t size) {
    memset(t, 0, sizeof(*t));
    C3D_Tex* tex = malloc(sizeof(C3D_Tex));
    Tex3DS_SubTexture* sub = malloc(sizeof(Tex3DS_SubTexture));
    Tex3DS_Texture t3x = tex && sub ? Tex3DS_TextureImport(data, size, tex, NULL, false) : NULL;
    if (!t3x && tex && sub && texture_low_memory) {  // linear memory is short: make room, try again
        texture_low_memory();
        t3x = Tex3DS_TextureImport(data, size, tex, NULL, false);
    }
    if (!t3x) {
        free(tex);
        free(sub);
        return false;
    }
    *sub = *Tex3DS_GetSubTexture(t3x, 0);
    Tex3DS_TextureFree(t3x);
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_LINEAR);  // as texture_from_rgba
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    sTexBytes += tex->size;
    sTexCount++;
    t->image.tex = tex;
    t->image.subtex = sub;
    t->width = sub->width;
    t->height = sub->height;
    t->loaded = true;
    return true;
}

bool texture_png_alpha_bounds(const uint8_t* png, uint32_t size, int rx, int ry, int rw, int rh, int* x0, int* y0,
                              int* x1, int* y1) {
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, png, size)) return false;
    img.format = PNG_FORMAT_RGBA;
    uint8_t* rgba = malloc(PNG_IMAGE_SIZE(img));
    if (!rgba || !png_image_finish_read(&img, NULL, rgba, 0, NULL)) {
        free(rgba);
        png_image_free(&img);
        return false;
    }
    *x0 = *y0 = 1 << 30;
    *x1 = *y1 = -1;
    for (int y = 0; y < rh; y++)
        for (int x = 0; x < rw; x++) {
            int px = rx + x, py = ry + y;
            if (px < 0 || py < 0 || px >= (int)img.width || py >= (int)img.height) continue;
            if (rgba[((size_t)py * img.width + px) * 4 + 3] < 16) continue;
            if (x < *x0) *x0 = x;
            if (y < *y0) *y0 = y;
            if (x > *x1) *x1 = x;
            if (y > *y1) *y1 = y;
        }
    free(rgba);
    return *x1 >= 0;
}

void texture_free(Texture* t) {
    if (!t->loaded) return;
    sTexBytes -= t->image.tex->size;
    sTexCount--;
    C3D_TexDelete(t->image.tex);
    free(t->image.tex);
    free((void*)t->image.subtex);
    memset(t, 0, sizeof(*t));
}
