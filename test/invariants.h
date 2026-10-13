#pragma once
// Invariant checks (test builds only): rules the game state must always obey, checked every frame
// of every scenario run, in Azahar and headless. See test/invariants.c.

#include "crew.h"
#include "run.h"
#include "ship.h"

// Checks our ship and crew, the enemy's if there is one, and the run (NULL outside a run). A rule
// broken for INV_HOLD_FRAMES frames in a row (or once, for NaNs) is logged as
// "ERROR invariant: ...", once per rule and ship per run.
void test_invariants(float now, const Ship* ship, const CrewSet* crew, const Ship* foe, const CrewSet* foeCrew,
                     const Run* run);
