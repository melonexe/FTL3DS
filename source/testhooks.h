#pragma once
// Scenario testing hooks. Only the test build (make TEST=1, test/ sources, FTL_TEST defined) can
// read sdmc:/3ds/ftl3ds/test.txt; in the normal build these are stubs that compile away.
//
// A scenario file holds setup lines, read once at start:
//   ship PLAYER_SHIP_STEALTH     player ship (blueprint name or 1-based number)
//   enemy REBEL_FAT              enemy ship (autoBlueprints name); "enemy none" for no enemy
//   event DISTRESS_BEACON        in a run: show this event at the start
//   crew 8                       top the crew up to this many, random races
//   tab arms                     crew | arms (or fight) | sys | power
//   seed 1234                    random seed, for repeatable fights
//   weapons on                   power every player weapon that fits
//   paused 1                     start paused
//   enemyhull 3                  enemy hull points (also the run's first fight)
//   enemyfire 0                  enemy weapons start off
//   board 2                      that many crew start on the teleporter and board the enemy
//   skills 1                     every crew skill at that level, one point short of the next
//   memory 1                     keep the memory readout on the top screen
//   env asteroid                 one-off fights at a hazard: nebula, storm, asteroid, sun, pulsar, pds
//   boss 2                       a one-off fight with that stage of the rebel flagship
//   laststand 1                  start the run in the final sector
//   title 1                      open on the title screen and hangar (skipped in tests otherwise)
// and timed commands, run inside the game at that many seconds after start:
//   at 5 do <action> [args]      act on the game (see test/test_exec.inc for the list)
//   at 6 shot <name>             log "SHOT <name>" and hold the game still while the PC runner
//                                takes a screenshot
//   at 7 log state               log a line of game state
//   at 8 expect <value> <op> <n> log PASS or FAIL (op: == != < <= > >=)
//   at 9 done                    log DONE and quit
// Every frame the state is also checked against invariants (test/invariants.c): a rule broken for
// half a second logs "ERROR invariant: ...".
// Everything is logged to sdmc:/3ds/ftl3ds/test.log. tools/test/run_scenario.ps1 runs a scenario
// in Azahar, takes the screenshots and prints the log.

#include <stdbool.h>

typedef struct {
    char ship[48], enemy[48], tab[16], event[48];
    int seed, weaponsOn, paused, enemyHull, enemyFire, board, crew, skills, memory, env, boss, lastStand, title;
} TestSetup;

#ifdef FTL_TEST
// Reads the scenario's setup (and queues its timed commands). False without a test.txt.
bool test_load(TestSetup* t);
// The next command due by `now` (seconds since start), or NULL. Call until NULL each frame.
const char* test_next_command(float now);
// Appends a line to the test log, with the time.
void test_log(float now, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#else
static inline bool test_load(TestSetup* t) {
    (void)t;
    return false;
}
#endif
