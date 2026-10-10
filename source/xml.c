#include "xml.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char* xml_find_tag(const char* p, const char* tag, const char** end) {
    size_t len = strlen(tag);
    while ((p = strchr(p, '<')) != NULL) {
        if (strncmp(p, "<!--", 4) == 0) {
            const char* c = strstr(p + 4, "-->");
            if (!c) return NULL;
            p = c + 3;
            continue;
        }
        const char* after = p + 1 + len;
        if (strncmp(p + 1, tag, len) == 0 && (isspace((unsigned char)*after) || *after == '/' || *after == '>')) {
            *end = strchr(after, '>');
            return *end ? p : NULL;
        }
        p++;
    }
    return NULL;
}

const char* xml_find_tag_before(const char* p, const char* limit, const char* tag, const char** end) {
    const char* t = xml_find_tag(p, tag, end);
    return (t && (!limit || t < limit)) ? t : NULL;
}

bool xml_attr(const char* tag, const char* end, const char* name, char* out, size_t outlen) {
    size_t len = strlen(name);
    for (const char* p = tag; p + len + 2 < end; p++) {
        if (!isspace((unsigned char)p[0]) || strncmp(p + 1, name, len) != 0) continue;
        const char* q = p + 1 + len;
        while (isspace((unsigned char)*q)) q++;
        if (*q != '=') continue;
        q++;
        while (isspace((unsigned char)*q)) q++;
        if (*q != '"') continue;
        q++;
        const char* close = memchr(q, '"', (size_t)(end - q));
        if (!close) return false;
        size_t n = (size_t)(close - q);
        if (n >= outlen) n = outlen - 1;
        memcpy(out, q, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

bool xml_attr_int(const char* tag, const char* end, const char* name, int* out) {
    char buf[16];
    if (!xml_attr(tag, end, name, buf, sizeof(buf))) return false;
    *out = atoi(buf);
    return true;
}

bool xml_attr_float(const char* tag, const char* end, const char* name, float* out) {
    char buf[16];
    if (!xml_attr(tag, end, name, buf, sizeof(buf))) return false;
    *out = (float)atof(buf);
    return true;
}

bool xml_text(const char* tagEnd, char* out, size_t outlen) {
    if (*tagEnd != '>' || tagEnd[-1] == '/') return false;
    const char* p = tagEnd + 1;
    while (isspace((unsigned char)*p)) p++;
    const char* close = strchr(p, '<');
    if (!close) return false;
    while (close > p && isspace((unsigned char)close[-1])) close--;
    size_t n = (size_t)(close - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}
