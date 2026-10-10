#pragma once
// Minimal XML tag/attribute scanning. FTL's XML is simple and sometimes not well-formed,
// so we scan tags directly instead of using a full parser.

#include <stdbool.h>
#include <stddef.h>

// Finds the next "<tag" (followed by whitespace, '/' or '>'), skipping comments.
// Returns a pointer to '<' and sets *end to the closing '>', or NULL.
const char* xml_find_tag(const char* p, const char* tag, const char** end);

// Like xml_find_tag, but stops (returns NULL) at `limit`.
const char* xml_find_tag_before(const char* p, const char* limit, const char* tag, const char** end);

// Copies attribute `name` from the tag spanning [tag, end) into out.
bool xml_attr(const char* tag, const char* end, const char* name, char* out, size_t outlen);
bool xml_attr_int(const char* tag, const char* end, const char* name, int* out);
bool xml_attr_float(const char* tag, const char* end, const char* name, float* out);

// Copies the text between the end of an opening tag and the next '<' into out, trimmed.
bool xml_text(const char* tagEnd, char* out, size_t outlen);
