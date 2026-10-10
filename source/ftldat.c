#include "ftldat.h"

#include <3ds.h>

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define FLAG_DEFLATED 0x01000000u

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int cmp_entry(const void* a, const void* b) {
    return strcmp(((const FtlEntry*)a)->name, ((const FtlEntry*)b)->name);
}

static int cmp_key(const void* key, const void* e) {
    return strcmp((const char*)key, ((const FtlEntry*)e)->name);
}

// Reads come from the main thread and the sound loader, so the file position is guarded.
static LightLock sLock;
static bool sLockReady;

bool ftldat_open(FtlDat* dat, const char* path, char* err, size_t errlen) {
    if (!sLockReady) {
        LightLock_Init(&sLock);
        sLockReady = true;
    }
    memset(dat, 0, sizeof(*dat));
    uint8_t* raw = NULL;

    dat->f = fopen(path, "rb");
    if (!dat->f) {
        snprintf(err, errlen, "Can't open %s", path);
        return false;
    }
    setvbuf(dat->f, NULL, _IOFBF, 64 * 1024);

    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof(hdr), dat->f) != sizeof(hdr) || memcmp(hdr, "PKG\n", 4) != 0) {
        snprintf(err, errlen, "Not an FTL 1.6+ ftl.dat");
        goto fail;
    }
    uint16_t headerSize = be16(hdr + 4);
    uint16_t entrySize = be16(hdr + 6);
    uint32_t count = be32(hdr + 8);
    uint32_t pathSize = be32(hdr + 12);
    if (entrySize < 20 || count == 0 || count > 100000) {
        snprintf(err, errlen, "Corrupt ftl.dat header");
        goto fail;
    }

    raw = malloc((size_t)entrySize * count);
    dat->names = malloc(pathSize + 1);
    dat->entries = calloc(count, sizeof(FtlEntry));
    if (!raw || !dat->names || !dat->entries) {
        snprintf(err, errlen, "Out of memory reading index");
        goto fail;
    }
    fseek(dat->f, headerSize, SEEK_SET);
    if (fread(raw, entrySize, count, dat->f) != count ||
        fread(dat->names, 1, pathSize, dat->f) != pathSize) {
        snprintf(err, errlen, "Truncated ftl.dat index");
        goto fail;
    }
    dat->names[pathSize] = '\0';

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = raw + (size_t)i * entrySize;
        uint32_t flagsOff = be32(e + 4);
        uint32_t nameOff = flagsOff & 0x00FFFFFFu;
        if (nameOff >= pathSize) {
            snprintf(err, errlen, "Corrupt entry %lu", (unsigned long)i);
            goto fail;
        }
        dat->entries[i].name = dat->names + nameOff;
        dat->entries[i].deflated = (flagsOff & FLAG_DEFLATED) != 0;
        dat->entries[i].offset = be32(e + 8);
        dat->entries[i].size = be32(e + 12);
        dat->entries[i].unpacked = be32(e + 16);
    }
    dat->count = count;
    free(raw);
    qsort(dat->entries, count, sizeof(FtlEntry), cmp_entry);
    return true;

fail:
    free(raw);
    ftldat_close(dat);
    return false;
}

void ftldat_close(FtlDat* dat) {
    if (dat->f) fclose(dat->f);
    free(dat->entries);
    free(dat->names);
    memset(dat, 0, sizeof(*dat));
}

const FtlEntry* ftldat_find(const FtlDat* dat, const char* name) {
    return bsearch(name, dat->entries, dat->count, sizeof(FtlEntry), cmp_key);
}

uint8_t* ftldat_read(FtlDat* dat, const char* name, uint32_t* outSize) {
    const FtlEntry* e = ftldat_find(dat, name);
    if (!e) return NULL;

    uint8_t* packed = malloc(e->size ? e->size : 1);
    uint8_t* out = malloc((size_t)e->unpacked + 1);
    if (!packed || !out) goto fail;

    LightLock_Lock(&sLock);
    fseek(dat->f, e->offset, SEEK_SET);
    bool read = fread(packed, 1, e->size, dat->f) == e->size;
    LightLock_Unlock(&sLock);
    if (!read) goto fail;

    if (e->deflated) {
        uLongf len = e->unpacked;
        if (uncompress(out, &len, packed, e->size) != Z_OK || len != e->unpacked) goto fail;
    } else {
        memcpy(out, packed, e->size);
    }
    free(packed);

    out[e->unpacked] = '\0';
    if (outSize) *outSize = e->unpacked;
    return out;

fail:
    free(packed);
    free(out);
    return NULL;
}
