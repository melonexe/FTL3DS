#pragma once
// Reader for FTL's ftl.dat archive (1.6+ "PKG" format). See tools/ftldat.py for the layout.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char* name;
    uint32_t offset;
    uint32_t size;      // bytes stored in the archive
    uint32_t unpacked;  // bytes after inflating
    bool deflated;
} FtlEntry;

typedef struct {
    FILE* f;
    uint32_t count;
    FtlEntry* entries;  // sorted by name
    char* names;
} FtlDat;

bool ftldat_open(FtlDat* dat, const char* path, char* err, size_t errlen);
void ftldat_close(FtlDat* dat);
const FtlEntry* ftldat_find(const FtlDat* dat, const char* name);

// Returns a malloc'd buffer with one extra NUL byte (so text files are C strings),
// or NULL if the file is missing or unreadable. outSize may be NULL.
uint8_t* ftldat_read(FtlDat* dat, const char* name, uint32_t* outSize);
