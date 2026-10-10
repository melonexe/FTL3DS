#include "xmldom.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Bump allocator in 64 KB blocks.
typedef struct Block {
    struct Block* prev;
    size_t used;
    char data[65536 - 2 * sizeof(size_t)];
} Block;

static void* arena_alloc(XDoc* doc, size_t n) {
    n = (n + 7) & ~(size_t)7;
    Block* b = doc->arena;
    if (!b || b->used + n > sizeof(b->data)) {
        Block* nb = malloc(sizeof(Block));
        if (!nb) return NULL;
        nb->prev = b;
        nb->used = 0;
        doc->arena = b = nb;
    }
    void* p = b->data + b->used;
    b->used += n;
    return p;
}

// Decodes &amp; &lt; &gt; &quot; &apos; in place.
static void decode_entities(char* s) {
    char* w = s;
    for (char* r = s; *r;) {
        if (*r == '&') {
            static const struct { const char* e; char c; } E[] = {
                {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool done = false;
            for (size_t i = 0; i < sizeof(E) / sizeof(E[0]) && !done; i++) {
                size_t n = strlen(E[i].e);
                if (!strncmp(r, E[i].e, n)) {
                    *w++ = E[i].c;
                    r += n;
                    done = true;
                }
            }
            if (done) continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static char* trim(char* s, char* e) {
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

bool xdoc_parse(XDoc* doc, char* buf) {
    memset(doc, 0, sizeof(*doc));
    doc->buf = buf;
    doc->root = arena_alloc(doc, sizeof(XNode));
    if (!doc->root) return false;
    memset(doc->root, 0, sizeof(XNode));
    doc->root->name = "#root";
    doc->root->text = "";

    XNode* stack[64];
    XNode* last[64];  // last child added at each depth
    int depth = 0;
    stack[0] = doc->root;
    last[0] = NULL;
    char* p = buf;
    char* textStart = NULL;

    while (*p) {
        if (*p != '<') {
            if (!textStart) textStart = p;
            p++;
            continue;
        }
        // Text before this tag belongs to the open element (first run only).
        if (textStart && depth > 0 && stack[depth]->text[0] == '\0') {
            char saved = *p;
            char* t = trim(textStart, p);
            if (*t) {
                size_t n = strlen(t);
                char* copy = arena_alloc(doc, n + 1);
                memcpy(copy, t, n + 1);
                decode_entities(copy);
                stack[depth]->text = copy;
            }
            *p = saved;
        }
        textStart = NULL;
        if (!strncmp(p, "<!--", 4)) {
            char* e = strstr(p + 4, "-->");
            p = e ? e + 3 : p + strlen(p);
            continue;
        }
        if (p[1] == '?' || p[1] == '!') {
            char* e = strchr(p, '>');
            p = e ? e + 1 : p + strlen(p);
            continue;
        }
        if (p[1] == '/') {  // closing tag
            char* e = strchr(p, '>');
            if (depth > 0) depth--;
            p = e ? e + 1 : p + strlen(p);
            continue;
        }
        // Opening tag: name, attributes, maybe self-closing.
        char* q = p + 1;
        char* nameStart = q;
        while (*q && !isspace((unsigned char)*q) && *q != '>' && *q != '/') q++;
        XNode* n = arena_alloc(doc, sizeof(XNode));
        if (!n) return false;
        memset(n, 0, sizeof(*n));
        n->text = "";
        size_t nl = (size_t)(q - nameStart);
        char* name = arena_alloc(doc, nl + 1);
        memcpy(name, nameStart, nl);
        name[nl] = '\0';
        n->name = name;

        const char* attrs[64];
        int ac = 0;
        bool selfClose = false;
        while (*q && *q != '>') {
            while (isspace((unsigned char)*q)) q++;
            if (*q == '/') {
                selfClose = true;
                q++;
                continue;
            }
            if (*q == '>' || !*q) break;
            char* an = q;
            while (*q && *q != '=' && !isspace((unsigned char)*q) && *q != '>') q++;
            char* anEnd = q;
            while (isspace((unsigned char)*q)) q++;
            if (*q != '=') continue;
            q++;
            while (isspace((unsigned char)*q)) q++;
            char quote = *q;
            if (quote != '"' && quote != '\'') continue;
            char* av = ++q;
            while (*q && *q != quote) q++;
            if (ac < 62) {
                size_t l1 = (size_t)(anEnd - an), l2 = (size_t)(q - av);
                char* a = arena_alloc(doc, l1 + 1);
                char* v = arena_alloc(doc, l2 + 1);
                memcpy(a, an, l1);
                a[l1] = '\0';
                memcpy(v, av, l2);
                v[l2] = '\0';
                decode_entities(v);
                attrs[ac++] = a;
                attrs[ac++] = v;
            }
            if (*q) q++;
        }
        attrs[ac++] = NULL;
        n->attrs = arena_alloc(doc, sizeof(char*) * ac);
        memcpy(n->attrs, attrs, sizeof(char*) * ac);

        XNode* parent = stack[depth];
        if (last[depth])
            last[depth]->next = n;
        else
            parent->child = n;
        last[depth] = n;
        p = *q ? q + 1 : q;
        if (!selfClose && depth < 63) {
            stack[++depth] = n;
            last[depth] = NULL;
        }
    }
    return true;
}

void xdoc_free(XDoc* doc) {
    Block* b = doc->arena;
    while (b) {
        Block* prev = b->prev;
        free(b);
        b = prev;
    }
    free(doc->buf);
    memset(doc, 0, sizeof(*doc));
}

const char* xattr(const XNode* n, const char* name) {
    for (int i = 0; n->attrs && n->attrs[i]; i += 2)
        if (!strcmp(n->attrs[i], name)) return n->attrs[i + 1];
    return NULL;
}

int xattr_int(const XNode* n, const char* name, int fallback) {
    const char* v = xattr(n, name);
    return v ? atoi(v) : fallback;
}

XNode* xchild(const XNode* n, const char* name) {
    for (XNode* c = n->child; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}
