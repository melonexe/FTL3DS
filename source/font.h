#pragma once
// FTL's bitmap fonts (fonts/*.font, "SIL" format per xftl's SILFontLoader), drawn at 1:1 pixels so
// text stays sharp on the 3DS screens.

#include <citro2d.h>
#include <stdbool.h>

#include "ftldat.h"

bool font_init(FtlDat* dat);
void font_exit(void);

// Sizes are given as the old system-font scales (0.34 .. 0.6) and mapped to the nearest FTL font.
float font_width(float scale, const char* text);    // widest line, in pixels
float font_char_width(float scale, uint32_t cp);    // advance of one character
float font_line_height(float scale);
float font_cap_height(float scale);  // height of capital letters, for centring in boxes
// Draws text with its top-left at (x, y); '\n' starts a new line. squeeze < 1 narrows it.
void font_draw(float x, float y, float scale, u32 color, const char* text, float squeeze);
