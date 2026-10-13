// The debug console's log (OPTIONS > DEBUG CONSOLE, then SELECT to its LOG page): the last few
// things worth knowing about, such as art that wouldn't load or a ship that failed, kept in a ring.
#pragma once

#define DBGLOG_LINES 14
#define DBGLOG_LEN 64

void dbglog_clock(float seconds);  // the time stamped on lines logged from now on
void dbglog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int dbglog_count(void);          // lines kept, up to DBGLOG_LINES
const char* dbglog_line(int i);  // 0 = the newest
