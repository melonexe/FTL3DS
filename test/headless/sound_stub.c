// Headless PC build: no sound. Every call in sound.h does nothing.

#include "sound.h"

bool sound_init(FtlDat* dat) {
    (void)dat;
    return true;
}
void sound_exit(void) {}
void sound_set_volume(float music, float sfx) { (void)music, (void)sfx; }
const char* sound_error(void) { return NULL; }
void sound_play(const char* name) { (void)name; }
void sound_prefetch(const char* name) { (void)name; }
void sound_release_unused(void) {}
void sound_free_idle(void) {}
void sound_stats(unsigned* linearBytes, unsigned* heapBytes, int* decoded) {
    *linearBytes = *heapBytes = 0;
    *decoded = 0;
}
void sound_loop(const char* name, bool on) { (void)name, (void)on; }
void sound_music(const char* track) { (void)track; }
void sound_update(float dt) { (void)dt; }
bool sound_music_ended(void) { return false; }
void sound_music_battle(bool battle) { (void)battle; }
const char* sound_random_track(void) { return ""; }
