#pragma once
// A beacon's environment (events' <environment type=...>; wiki: Environmental Hazards).

enum { ENV_NONE, ENV_NEBULA, ENV_STORM, ENV_ASTEROID, ENV_SUN, ENV_PULSAR, ENV_PDS };

// Difficulty, chosen in the hangar.
enum { DIFF_EASY, DIFF_NORMAL, DIFF_HARD };

// Who an anti-ship battery (PDS) fires at: bits by side (1 = player, 2 = enemy).
#define ENV_TARGET_PLAYER 1
#define ENV_TARGET_ENEMY 2
