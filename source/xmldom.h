#pragma once
// A small XML tree parser for FTL's event files (which nest events inside choices inside events).
// The document keeps its buffer; names, attribute values and text point into it.

#include <stdbool.h>

typedef struct XNode XNode;
struct XNode {
    const char* name;
    const char** attrs;  // name, value, name, value, ..., NULL
    const char* text;    // text directly inside the element (trimmed), or ""
    XNode* child;        // first child element
    XNode* next;         // next sibling
};

typedef struct {
    char* buf;
    XNode* root;    // the document element's first child list lives in root->child
    void* arena;    // allocations, freed together
} XDoc;

// Parses (and takes ownership of) a malloc'd, NUL-terminated buffer. Returns false on failure.
bool xdoc_parse(XDoc* doc, char* buf);
void xdoc_free(XDoc* doc);

const char* xattr(const XNode* n, const char* name);  // NULL if absent
int xattr_int(const XNode* n, const char* name, int fallback);
XNode* xchild(const XNode* n, const char* name);      // first child with that name, or NULL
