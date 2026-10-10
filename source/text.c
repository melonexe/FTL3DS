#include "text.h"

#include <stdlib.h>
#include <string.h>

#include "xml.h"

typedef struct {
    const char* id;
    const char* text;
} TextEntry;

#define TEXT_FILES 5
static char* sFiles[TEXT_FILES];
static TextEntry* sEntries;
static int sCount;

static int cmp_entry(const void* a, const void* b) {
    return strcmp(((const TextEntry*)a)->id, ((const TextEntry*)b)->id);
}

// &quot; and friends, in place.
static void decode_entities(char* s) {
    static const struct { const char* e; char c; } E[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    char* w = s;
    for (char* r = s; *r;) {
        size_t i = 0;
        if (*r == '&')
            for (; i < sizeof(E) / sizeof(E[0]); i++)
                if (!strncmp(r, E[i].e, strlen(E[i].e))) break;
        if (*r == '&' && i < sizeof(E) / sizeof(E[0])) {
            *w++ = E[i].c;
            r += strlen(E[i].e);
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

// Scans one file, NUL-terminating ids and texts in place. With out == NULL it only counts.
static int scan(char* xml, TextEntry* out) {
    int n = 0;
    char* p = xml;
    const char* end;
    const char* tag;
    while ((tag = xml_find_tag(p, "text", &end)) != NULL) {
        char* e = (char*)end;
        p = e + 1;
        if (e[-1] == '/') continue;
        // id: name="..." inside the tag.
        char* name = strstr((char*)tag, "name=\"");
        if (!name || name > e) continue;
        name += 6;
        char* nameEnd = strchr(name, '"');
        char* close = strstr(p, "</text>");
        if (!nameEnd || nameEnd > e || !close) continue;
        if (out) {
            *nameEnd = '\0';
            *close = '\0';
            out[n].id = name;
            out[n].text = e + 1;
            decode_entities(e + 1);
        }
        n++;
        p = close + 7;
    }
    return n;
}

bool text_load(FtlDat* dat) {
    static const char* const files[TEXT_FILES] = {"data/text_blueprints.xml", "data/text_misc.xml",
                                                  "data/text_events.xml", "data/text_sectorname.xml",
                                                  "data/text_achievements.xml"};
    int total = 0;
    for (int i = 0; i < TEXT_FILES; i++) {
        sFiles[i] = (char*)ftldat_read(dat, files[i], NULL);
        if (sFiles[i]) total += scan(sFiles[i], NULL);
    }
    sEntries = malloc(sizeof(TextEntry) * (total ? total : 1));
    if (!sEntries) return false;
    sCount = 0;
    for (int i = 0; i < TEXT_FILES; i++)
        if (sFiles[i]) sCount += scan(sFiles[i], sEntries + sCount);
    qsort(sEntries, sCount, sizeof(TextEntry), cmp_entry);
    return sCount > 0;
}

void text_free(void) {
    free(sEntries);
    for (int i = 0; i < TEXT_FILES; i++) {
        free(sFiles[i]);
        sFiles[i] = NULL;
    }
    sEntries = NULL;
    sCount = 0;
}

const char* text_get(const char* id, const char* fallback) {
    if (!id) return fallback;
    TextEntry key = {id, NULL};
    const TextEntry* e = sCount ? bsearch(&key, sEntries, sCount, sizeof(TextEntry), cmp_entry) : NULL;
    return e ? e->text : fallback;
}
