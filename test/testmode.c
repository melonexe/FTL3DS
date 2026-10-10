// Scenario tests (test build only): reads sdmc:/3ds/ftl3ds/test.txt, hands its timed commands to
// the game loop when they're due, and writes sdmc:/3ds/ftl3ds/test.log. See source/testhooks.h.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testhooks.h"

#define TEST_FILE "sdmc:/3ds/ftl3ds/test.txt"
#define LOG_FILE "sdmc:/3ds/ftl3ds/test.log"
#define MAX_CMDS 256

typedef struct {
    float at;
    char text[96];
} Cmd;

static Cmd sCmds[MAX_CMDS];
static int sCmdCount, sNext;

static int env_index(const char* v) {
    static const char* const envs[] = {"none", "nebula", "storm", "asteroid", "sun", "pulsar", "pds"};
    for (int k = 0; k < 7; k++)
        if (!strcmp(v, envs[k])) return k;
    return 0;
}

static void setup_line(TestSetup* t, const char* key, const char* val) {
    if (!strcmp(key, "ship")) snprintf(t->ship, sizeof(t->ship), "%s", val);
    else if (!strcmp(key, "enemy")) snprintf(t->enemy, sizeof(t->enemy), "%s", val);
    else if (!strcmp(key, "event")) snprintf(t->event, sizeof(t->event), "%s", val);
    else if (!strcmp(key, "crew")) t->crew = atoi(val);
    else if (!strcmp(key, "tab")) snprintf(t->tab, sizeof(t->tab), "%.15s", val);
    else if (!strcmp(key, "seed")) t->seed = atoi(val);
    else if (!strcmp(key, "weapons")) t->weaponsOn = !strcmp(val, "on");
    else if (!strcmp(key, "paused")) t->paused = atoi(val);
    else if (!strcmp(key, "enemyhull")) t->enemyHull = atoi(val);
    else if (!strcmp(key, "enemyfire")) t->enemyFire = atoi(val);
    else if (!strcmp(key, "board")) t->board = atoi(val);
    else if (!strcmp(key, "skills")) t->skills = atoi(val) + 1;
    else if (!strcmp(key, "memory")) t->memory = atoi(val);
    else if (!strcmp(key, "boss")) t->boss = atoi(val);
    else if (!strcmp(key, "laststand")) t->lastStand = atoi(val);
    else if (!strcmp(key, "title")) t->title = atoi(val);
    else if (!strcmp(key, "env")) t->env = env_index(val);
}

static int cmd_order(const void* a, const void* b) {
    float d = ((const Cmd*)a)->at - ((const Cmd*)b)->at;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

bool test_load(TestSetup* t) {
    memset(t, 0, sizeof(*t));
    t->seed = -1;
    t->enemyHull = -1;
    t->enemyFire = -1;
    sCmdCount = sNext = 0;
    FILE* f = fopen(TEST_FILE, "r");
    if (!f) return false;
    FILE* log = fopen(LOG_FILE, "w");  // a fresh log for each run
    if (log) fclose(log);
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n#")] = '\0';  // '#' starts a comment
        char key[32] = "", val[96] = "";
        if (sscanf(line, "%31s %95[^\n]", key, val) < 1 || !key[0]) continue;
        if (!strcmp(key, "at")) {
            float when;
            int used = 0;
            if (sscanf(val, "%f %n", &when, &used) >= 1 && used > 0 && sCmdCount < MAX_CMDS) {
                sCmds[sCmdCount].at = when;
                snprintf(sCmds[sCmdCount].text, sizeof(sCmds[0].text), "%s", val + used);
                sCmdCount++;
            }
            continue;
        }
        char first[48] = "";
        sscanf(val, "%47s", first);
        setup_line(t, key, first);
    }
    fclose(f);
    // Stable by time: commands at the same moment keep their order in the file.
    for (int i = 1; i < sCmdCount; i++)
        for (int j = i; j > 0 && cmd_order(&sCmds[j - 1], &sCmds[j]) > 0; j--) {
            Cmd tmp = sCmds[j];
            sCmds[j] = sCmds[j - 1];
            sCmds[j - 1] = tmp;
        }
    return true;
}

const char* test_next_command(float now) {
    if (sNext >= sCmdCount || sCmds[sNext].at > now) return NULL;
    return sCmds[sNext++].text;
}

void test_log(float now, const char* fmt, ...) {
    FILE* f = fopen(LOG_FILE, "a");
    if (!f) return;
    fprintf(f, "[%7.2f] ", now);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
