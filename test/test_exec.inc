// Scenario commands (test build only), included into main()'s game loop so they can reach the
// game's state directly. Runs every command that's due this frame. See source/testhooks.h.
//
//   do pause | resume                 pause or unpause the fight
//   do tab crew|fight|sys|power       switch the bottom-screen tab
//   do power <weapon#>                power that weapon (1-based) if it isn't
//   do target <weapon#> <room#|sys>   aim a weapon at a room (number) or a system (shields...)
//   do autofire 0|1
//   do destroy | killcrew             the enemy blows up / its crew all die
//   do hull N | enemyhull N | scrap N | fuel N | missiles N | parts N
//   do jump <beacon#>|near            jump (with the animation) to that beacon, or any neighbour
//   do wait                           wait a turn on the map
//   do choose N                       pick event choice N (1-based)
//   do closeevents                    choice 1 on every page until the event dialog closes
//   do nextsector [row]               jump to the next sector
//   do flagship                       move the flagship to our beacon (the Last Stand)
//   do boss N                         start a fight with flagship stage N
//   do env <name>                     set this beacon's environment
//   do surge                          the flagship's next power surge in a moment
//   do select N                       select crew member N (1-based) only
//   do key A|B|X|Y|L|R|UP|DOWN|LEFT|RIGHT|SELECT   press a button next frame
//   do tap X Y                        touch the bottom screen at (X, Y) next frame
//   do aug NAME                       fit an augment (the run's list, or for this one-off fight)
//   do fire N | breach N              start fires / breaches in N random rooms of our ship
//   do map                            open the beacon map
//   do enemydamage                    a bar of damage on every enemy system
//   do evade N                        the next N shots at our ship miss (a real dodge: piloting and
//                                     engines learn as if the ship had dodged them)
//   do lockdown                       our first Crystal uses the lockdown where they stand
//   do drone NAME                     fit that drone (drone system and reactor raised to run it), power it
//   shot <name>                       log SHOT <name> and hold still for the PC runner's screenshot
//   log state                         a line of game state
//   expect <value> <op> <number>      PASS or FAIL; values as in "log state", plus skill_<name>
//                                     (best crew level: pilot, engines, shields, weapons, repair,
//                                     combat), menu (0 none, 1 main, 2 hangar, 3 options, 4 pause)
//                                     and saved (a save file exists), fires, breaches,
//                                     jumpable (beacons we can jump to), jumpfar (... not next door),
//                                     aug_<NAME> (count fitted), ach_<ID> (0, or 1 + difficulty earned),
//                                     lockdowns (our rooms locked down), enemyion (enemy systems ionised),
//                                     enemyevasion, enemydamage (bars of damage on enemy systems),
//                                     enemyhulllost (hull the enemy has lost this fight, repairs or not),
//                                     manned_<system> (1 if someone is at its station),
//                                     skillmax_<name> (best level reached this run, even by crew since lost),
//                                     dodges (shots that have missed our ship)
//   done                              log DONE and quit
// Every frame, test/invariants.c also checks the rules the game state must always obey and logs
// "ERROR invariant: ..." if one stays broken.
{
    double tv(const char* n) {  // a game value by name (GCC nested function)
        const Ship* ps = PLAYER_SHIP;
        const Ship* es = ENEMY_SHIP;
        unsigned texB, texN;
        texture_stats(&texB, &texN);
        if (!strcmp(n, "hull")) return ps->hullHp;
        if (!strcmp(n, "maxhull")) return ps->maxHull;
        if (!strcmp(n, "shields")) return ps->shieldLayers;
        if (!strcmp(n, "zoltan")) return ps->superShield;
        if (!strcmp(n, "evasion")) return combat_evasion(ps);
        if (!strcmp(n, "crew")) return crew_native_count(PLAYER_CREW);
        if (!strcmp(n, "enemy")) return haveEnemy;
        if (!strcmp(n, "enemyhull")) return es ? es->hullHp : -1;
        if (!strcmp(n, "enemypirate")) {  // a pirate variant (MANTIS_FIGHTER_P...), or its AE version (..._P_DLC)
            const char* nm = es ? es->bp->name : "";
            size_t l = strlen(nm);
            return (l > 2 && !strcmp(nm + l - 2, "_P")) || strstr(nm, "_P_DLC") != NULL;
        }
        if (!strcmp(n, "enemymaxhull")) return es ? es->maxHull : -1;
        if (!strcmp(n, "enemyshields")) return es ? es->shieldLayers : -1;
        if (!strcmp(n, "enemyzoltan")) return es ? es->superShield : -1;
        if (!strcmp(n, "enemyevasion")) return es ? combat_evasion(es) : -1;
        if (!strcmp(n, "enemydamage")) {  // bars of damage across the enemy's systems
            int dmg = 0;
            for (int k = 0; es && k < es->systemCount; k++) dmg += es->systems[k].damage;
            return es ? dmg : -1;
        }
        if (!strcmp(n, "enemycrew")) return haveEnemy ? crew_native_count(ENEMY_CREW) : -1;
        if (!strcmp(n, "artillery")) return es ? es->artilleryCount : -1;
        if (!strcmp(n, "result")) return combat.result;  // 0 fighting, 1 won, 2 lost
        if (!strcmp(n, "bossstage")) return sBossStage;
        if (!strcmp(n, "env")) return sEnvNow;
        if (!strcmp(n, "ftl")) return sFtlCharge;
        if (!strcmp(n, "canjump")) return sCanJump;  // the JUMP button works
        if (!strcmp(n, "dodges")) return gTestDodges;  // shots that have missed our ship
        if (!strcmp(n, "enemyhulllost")) return sTestFoeHullLost;  // hull the enemy has lost this fight, repairs or not
        if (!strcmp(n, "mapopen")) return sMapOpen;
        if (!strcmp(n, "paused")) return paused;
        if (!strcmp(n, "linearfree")) return linearSpaceFree() / 1048576.0;
        if (!strcmp(n, "soundmb") || !strcmp(n, "soundheapmb") || !strcmp(n, "sounds")) {  // decoded effects
            unsigned lin, heap;
            int count;
            sound_stats(&lin, &heap, &count);
            return !strcmp(n, "sounds") ? count : !strcmp(n, "soundmb") ? lin / 1048576.0 : heap / 1048576.0;
        }
        if (!strcmp(n, "textures")) return texB / 1048576.0;
        if (!strcmp(n, "run")) return sInRun;
        if (!strncmp(n, "skillmax_", 9)) {  // the best level reached this run in that skill, even by crew since lost
            static const char* const sk[SKILL_COUNT] = {"pilot", "engines", "shields", "weapons", "repair", "combat"};
            for (int k = 0; k < SKILL_COUNT; k++)
                if (!strcmp(n + 9, sk[k])) return sTestSkillMax[k];
            return -1;
        }
        if (!strncmp(n, "skill_", 6)) {  // the best level any of our crew has in that skill
            static const char* const sk[SKILL_COUNT] = {"pilot", "engines", "shields", "weapons", "repair", "combat"};
            int best = 0;
            for (int k = 0; k < SKILL_COUNT; k++)
                if (!strcmp(n + 6, sk[k]))
                    for (int i = 0; i < PLAYER_CREW->count; i++) {
                        int l = crew_skill_level(PLAYER_CREW, &PLAYER_CREW->crew[i], k);
                        if (l > best) best = l;
                    }
            return best;
        }
        if (!strcmp(n, "menu")) return sMenu;
        if (!strcmp(n, "sandbox")) return sRun.sandbox;
        if (!strcmp(n, "jumpms")) return sJumpChangeMs;
        if (!strcmp(n, "shiploadms")) return sShipLoadMs;
        if (!strcmp(n, "weapons")) return PLAYER_SHIP->weaponCount;
        if (!strcmp(n, "visitor")) return sVisitor;
        if (!strcmp(n, "fps")) return 1.0f / sTestFrameTime;
        if (!strcmp(n, "cpums")) return C3D_GetProcessingTime();  // last frame: building it, then the GPU drawing it
        if (!strcmp(n, "gpums")) return C3D_GetDrawingTime();
        if (!strcmp(n, "preloaded")) return sPreloadFor != NULL;  // an enemy ship built during the last jump
        if (!strcmp(n, "fires")) return life_ship_fires(ps);
        if (!strcmp(n, "intruders")) return crew_foreign_count(PLAYER_CREW);
        if (!strcmp(n, "opendoors") || !strcmp(n, "openairlocks")) {  // doors the player has opened
            bool airlocks = n[4] == 'a';
            int k = 0;
            for (int i = 0; i < ps->doorCount; i++)
                if (ps->doorState[i].forced && (ps->doors[i].roomA < 0 || ps->doors[i].roomB < 0) == airlocks) k++;
            return k;
        }
        if (!strncmp(n, "manned_", 7)) {  // someone at that system's station: 1, no: 0, no such system: -1
            int si = ship_find_system(ps, system_from_name(n + 7));
            return si >= 0 ? ps->systems[si].manned : -1;
        }
        if (!strncmp(n, "cap_", 4)) {  // a system's event limit (<status>): -1 none, -2 no such system
            int si = ship_find_system(ps, system_from_name(n + 4));
            return si >= 0 ? ps->systems[si].eventCap : -2;
        }
        if (!strcmp(n, "lockdowns")) {
            int l = 0;
            for (int r = 0; r < ps->roomCount; r++) l += ps->lockdown[r] > 0.0f;
            return l;
        }
        if (!strcmp(n, "enemyion")) {
            int l = 0;
            for (int k = 0; es && k < es->systemCount; k++) l += es->systems[k].ionTimer > 0.0f;
            return es ? l : -1;
        }
        if (!strcmp(n, "breaches")) return life_ship_breaches(ps);
        if (!strncmp(n, "ach_", 4)) return profile_ach_level(profile_ach_find(n + 4));  // 0 not earned
        if (!strncmp(n, "aug_", 4)) {
            for (int k = 0; k < AUG_COUNT; k++)
                if (!strcmp(n + 4, AUG_NAMES[k])) return ps->aug[k];
            return -1;
        }
        if (!strcmp(n, "jumpfar")) {  // jumpable beacons that aren't neighbours (Adv. FTL Navigation)
            int j = 0;
            for (int b = 0; sInRun && b < sRun.beaconCount; b++) {
                bool nb = false;
                for (int k = 0; k < sRun.beacons[sRun.current].nbCount; k++) nb |= sRun.beacons[sRun.current].nb[k] == b;
                j += !nb && run_can_jump(&sRun, b);
            }
            return j;
        }
        if (!strcmp(n, "jumpable")) {
            int j = 0;
            for (int b = 0; sInRun && b < sRun.beaconCount; b++) j += run_can_jump(&sRun, b);
            return j;
        }
        if (!strcmp(n, "saved")) return sSaveExists;
        if (!sInRun) return -1;
        if (!strcmp(n, "scrap")) return sRun.scrap;
        if (!strcmp(n, "fuel")) return sRun.fuel;
        if (!strcmp(n, "missiles")) return ps->missiles;
        if (!strcmp(n, "parts")) return ps->droneParts;
        if (!strcmp(n, "sector")) return sRun.sector + 1;
        if (!strcmp(n, "beacon")) return sRun.current;
        if (!strcmp(n, "event")) return sRun.view.active;
        if (!strcmp(n, "stage")) return sRun.flagStage;
        if (!strcmp(n, "flagbeacon")) return sRun.flagBeacon;
        if (!strcmp(n, "flagnext")) return sRun.flagNext;
        if (!strcmp(n, "base")) return sRun.baseBeacon;
        if (!strcmp(n, "baseturns")) return sRun.baseTurns;
        if (!strcmp(n, "laststand")) return sRun.lastStand;
        if (!strcmp(n, "over")) return sRun.over;
        if (!strcmp(n, "won")) return sRun.won;
        if (!strcmp(n, "baselost")) return sRun.baseLost;
        return -9999;
    }

    sTestClock += dt;
    if (sTestHold > 0.0f) sTestHold -= dt;
    for (const char* cmd; sTestHold <= 0.0f && (cmd = test_next_command(sTestClock)) != NULL;) {
        char verb[16] = "", a1[48] = "", a2[48] = "", a3[48] = "";
        sscanf(cmd, "%15s %47s %47s %47s", verb, a1, a2, a3);
        test_log(sTestClock, "> %s", cmd);
        if (!strcmp(verb, "done")) {
            test_log(sTestClock, "DONE");
            sTestQuit = true;
        } else if (!strcmp(verb, "shot")) {
            test_log(sTestClock, "SHOT %s", a1);
            sTestHold = 1.5f;  // the PC runner captures the window meanwhile
        } else if (!strcmp(verb, "log")) {
            static const char* const keys[] = {"hull", "shields", "zoltan", "evasion", "crew", "enemyhull", "enemyshields",
                                               "enemyzoltan", "enemycrew", "artillery", "result", "bossstage", "env", "ftl",
                                               "scrap", "fuel", "sector", "beacon", "event", "stage", "flagbeacon",
                                               "flagnext", "base", "baseturns", "over", "won", "baselost", "linearfree",
                                               "textures", "soundmb", "soundheapmb", "sounds", "preloaded", "enemy", "fps", "cpums", "gpums", "jumpms", "shiploadms"};
            char line[768] = "STATE";
            for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
                char part[32];
                snprintf(part, sizeof(part), " %s=%g", keys[k], tv(keys[k]));
                strncat(line, part, sizeof(line) - strlen(line) - 1);
            }
            test_log(sTestClock, "%s", line);
        } else if (!strcmp(verb, "expect")) {
            double v = tv(a1), want = atof(a3);
            bool ok = !strcmp(a2, "==") ? v == want : !strcmp(a2, "!=") ? v != want : !strcmp(a2, "<") ? v < want
                    : !strcmp(a2, "<=") ? v <= want : !strcmp(a2, ">") ? v > want : !strcmp(a2, ">=") ? v >= want : false;
            test_log(sTestClock, "%s %s %s %s (is %g)", ok ? "PASS" : "FAIL", a1, a2, a3, v);
        } else if (!strcmp(verb, "do")) {
            Ship* ps = PLAYER_SHIP;
            Ship* es = ENEMY_SHIP;
            int n1 = atoi(a2);
            if (!strcmp(a1, "pause")) paused = true;
            else if (!strcmp(a1, "resume")) paused = false;
            else if (!strcmp(a1, "tab")) {
                static const char* const tabNames[TAB_COUNT] = {"crew", "fight", "sys", "power"};
                for (int i = 0; i < TAB_COUNT; i++)
                    if (!strcmp(a2, tabNames[i]) || (i == TAB_ARMS && !strcmp(a2, "arms"))) tab = i;
            } else if (!strcmp(a1, "power") && n1 >= 1 && n1 <= ps->weaponCount) {
                if (!ps->weapons[n1 - 1].powered) combat_toggle_weapon(ps, n1 - 1);
            } else if (!strcmp(a1, "target") && es && n1 >= 1 && n1 <= ps->weaponCount) {
                int room = -1;
                if (a3[0] >= '0' && a3[0] <= '9') room = atoi(a3);
                else {
                    int si = ship_find_system(es, system_from_name(a3));
                    room = si >= 0 ? es->systems[si].room : -1;
                }
                if (room >= 0 && room < es->roomCount) {
                    if (!ps->weapons[n1 - 1].powered) combat_toggle_weapon(ps, n1 - 1);
                    combat_set_target(&combat, n1 - 1, room);
                } else {
                    test_log(sTestClock, "ERROR no room %s", a3);
                }
            } else if (!strcmp(a1, "autofire")) combat.autofire = n1 != 0;
            else if (!strcmp(a1, "destroy") || !strcmp(a1, "killcrew")) {
                if (!es) test_log(sTestClock, "ERROR no enemy to %s", a1);
                else if (!strcmp(a1, "destroy")) es->hullHp = 0;
                else
                    for (int i = 0; i < ENEMY_CREW->count; i++)
                        if (!ENEMY_CREW->crew[i].foreign) ENEMY_CREW->crew[i].health = -1000.0f;  // well below 0: a medbay tick can't save them
            } else if (!strcmp(a1, "closeevents") && sInRun) {
                // Choice 1 on every page until the dialog closes (at most 12 pages).
                for (int k = 0; k < 12 && sRun.view.active; k++) run_choose(&sRun, RCTX, 0);
            } else if (!strcmp(a1, "hull")) ps->hullHp = n1;
            else if (!strcmp(a1, "enemyhull") && es) es->hullHp = n1;
            else if (!strcmp(a1, "missiles")) ps->missiles = n1;
            else if (!strcmp(a1, "parts")) ps->droneParts = n1;
            else if (!strcmp(a1, "scrap") && sInRun) sRun.scrap = n1;
            else if (!strcmp(a1, "fuel") && sInRun) sRun.fuel = n1;
            else if (!strcmp(a1, "jump") && sInRun) {
                int b = -1;
                if (!strcmp(a2, "near")) {
                    const Beacon* cb = &sRun.beacons[sRun.current];
                    // An unvisited neighbour if there is one (so runs move on), else any.
                    for (int k = 0; k < cb->nbCount && b < 0; k++)
                        if (run_can_jump(&sRun, cb->nb[k]) && !sRun.beacons[cb->nb[k]].visited) b = cb->nb[k];
                    for (int k = 0; k < cb->nbCount && b < 0; k++)
                        if (run_can_jump(&sRun, cb->nb[k])) b = cb->nb[k];
                } else {
                    b = n1;
                }
                if (b >= 0 && run_can_jump(&sRun, b)) {
                    sMapOpen = false;
                    sJumpTarget = b;
                    sJumpTime = 0.0f;
                    sound_play("jumpLeave");  // as tapping the beacon does
                } else {
                    test_log(sTestClock, "ERROR can't jump to %s", a2);
                }
            } else if (!strcmp(a1, "wait") && sInRun) run_wait(&sRun, RCTX);
            else if (!strcmp(a1, "choose") && sInRun && sRun.view.active && n1 >= 1 && n1 <= sRun.view.choiceCount)
                run_choose(&sRun, RCTX, n1 - 1);
            else if (!strcmp(a1, "nextsector") && sInRun) {
                sNextRow = n1;
                sJumpTarget = -1;
                sJumpTime = 0.0f;
                sound_play("jumpLeave");
            } else if (!strcmp(a1, "flagship") && sInRun && sRun.lastStand) {
                sRun.flagBeacon = sRun.current;
                sRun.bossFight = true;
            } else if (!strcmp(a1, "boss") && n1 >= 1 && n1 <= 3) {
                START_BOSS(n1);
            } else if (!strcmp(a1, "env")) {
                static const char* const envs[] = {"none", "nebula", "storm", "asteroid", "sun", "pulsar", "pds"};
                for (int k = 0; k < 7; k++)
                    if (!strcmp(a2, envs[k])) {
                        if (sInRun) {
                            sRun.env = k;
                            sRun.envTarget = ENV_TARGET_PLAYER;
                        } else {
                            sTestEnv = k;
                        }
                    }
            } else if (!strcmp(a1, "surge")) combat.surgeNext = 0.2f;
            else if (!strcmp(a1, "aug")) {
                if (sInRun && sRun.augCount < MAX_AUGS)
                    snprintf(sRun.augs[sRun.augCount++], sizeof(sRun.augs[0]), "%s", a2);
                else if (!sInRun && sExtraAugCount < 3)
                    snprintf(sExtraAugs[sExtraAugCount++], sizeof(sExtraAugs[0]), "%s", a2);
                else
                    test_log(sTestClock, "ERROR no room for augment %s", a2);
            } else if ((!strcmp(a1, "fire") || !strcmp(a1, "breach")) && ps->roomCount > 0) {
                for (int k = 0; k < (n1 > 0 ? n1 : 1); k++) {
                    int room = rand() % ps->roomCount;
                    if (a1[0] == 'f') life_ignite(ps, room);
                    else life_breach(ps, room);
                }
            } else if (!strcmp(a1, "enemydamage") && es) {  // a bar of damage on every enemy system
                for (int k = 0; k < es->systemCount; k++)
                    if (es->systems[k].damage < es->systems[k].def->level) es->systems[k].damage++;
            } else if (!strcmp(a1, "break")) {  // one of our systems fully damaged: do break pilot
                int si = ship_find_system(ps, system_from_name(a2));
                if (si >= 0) ps->systems[si].damage = ps->systems[si].def->level;
            } else if (!strcmp(a1, "map") && sInRun) sMapOpen = true;
            else if (!strcmp(a1, "evade")) gTestEvade = n1;  // the next N shots at our ship miss (the game's dodge, XP and all)
            else if (!strcmp(a1, "opendoors")) life_set_all_doors(ps, true);  // the open all button
            else if (!strcmp(a1, "closedoors")) life_set_all_doors(ps, false);
            else if (!strcmp(a1, "store") && sInRun) {  // this beacon's store (stocked if new) on tab n1
                run_store_enter(&sRun, RCTX);
                sStoreOpen = true;
                sStoreTab = n1;
                sStoreSel = -1;
            } else if (!strcmp(a1, "shipmenu") && sInRun) {  // the SHIP screen: 0 upgrades, 1 equipment
                sStoreOpen = false;
                sShipOpen = true;
                sShipTab = n1;
            }
            else if (!strcmp(a1, "lockdown")) {
                bool done = false;
                for (int i = 0; i < PLAYER_CREW->count && !done; i++)
                    if (!strcmp(PLAYER_CREW->races[PLAYER_CREW->crew[i].race].name, "crystal"))
                        done = combat_crystal_lockdown(&combat, SIDE_PLAYER, i);
                if (!done) test_log(sTestClock, "ERROR no crystal could lock down");
            } else if (!strcmp(a1, "drone")) {
                int ds = ship_find_system(ps, SYS_DRONES);
                const DroneDef* dd = drone_find(a2);
                if (ds < 0 || !dd || !combat_add_drone(ps, &dat, &anims, a2)) {
                    test_log(sTestClock, "ERROR can't fit drone %s", a2);
                } else {
                    SystemDef* def = (SystemDef*)ps->systems[ds].def;
                    if (def->level < dd->power) def->level = dd->power;
                    ps->reactor += dd->power;
                    ps->droneParts += 2;
                    if (!combat_toggle_drone(&combat, SIDE_PLAYER, ps->droneCount - 1))
                        test_log(sTestClock, "ERROR drone %s won't power", a2);
                }
            }
            else if (!strcmp(a1, "select") && n1 >= 1 && n1 <= PLAYER_CREW->count) PLAYER_CREW->selected = 1u << (n1 - 1);
            else if (!strcmp(a1, "key")) {
                static const char* const names[] = {"A", "B", "X", "Y", "L", "R", "UP", "DOWN", "LEFT", "RIGHT", "SELECT"};
                static const u32 bits[] = {KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT, KEY_SELECT};
                for (int k = 0; k < 11; k++)
                    if (!strcmp(a2, names[k])) sTestKeys |= bits[k];
            } else if (!strcmp(a1, "tap")) {
                sTestTap = true;
                sTestTapX = (u16)atoi(a2);
                sTestTapY = (u16)atoi(a3);
            } else {
                test_log(sTestClock, "ERROR unknown: %s", cmd);
            }
        } else {
            test_log(sTestClock, "ERROR unknown: %s", cmd);
        }
    }
}
