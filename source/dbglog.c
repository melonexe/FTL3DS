#include "dbglog.h"

#include <stdarg.h>
#include <stdio.h>

static char sLines[DBGLOG_LINES][DBGLOG_LEN];
static int sNext, sCount;
static float sNow;

void dbglog_clock(float seconds) { sNow = seconds; }

void dbglog(const char* fmt, ...) {
    char* out = sLines[sNext];
    int s = (int)sNow;
    int n = snprintf(out, DBGLOG_LEN, "%d:%02d ", s / 60, s % 60);
    if (n < 0 || n >= DBGLOG_LEN) n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + n, DBGLOG_LEN - n, fmt, ap);
    va_end(ap);
    sNext = (sNext + 1) % DBGLOG_LINES;
    if (sCount < DBGLOG_LINES) sCount++;
}

int dbglog_count(void) { return sCount; }

const char* dbglog_line(int i) {
    if (i < 0 || i >= sCount) return "";
    return sLines[(sNext - 1 - i + 2 * DBGLOG_LINES) % DBGLOG_LINES];
}
