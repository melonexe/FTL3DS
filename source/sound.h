#pragma once
// Sound effects (data/sounds.xml) and streamed music, played through the 3DS DSP.
// On real hardware the DSP needs sdmc:/3ds/dspfirm.cdc (dumped with DSP1); without it the game
// runs silently.

#include <stdbool.h>

#include "ftldat.h"

// Reads the sound list and starts the DSP. Returns false (and the game stays silent) on failure.
bool sound_init(FtlDat* dat);
void sound_exit(void);
// The options' volumes, 0..1 each (music, sound effects).
void sound_set_volume(float music, float sfx);

// Why sound is off, or NULL if it's working.
const char* sound_error(void);

// Plays a sound by its sounds.xml name, e.g. "powerUpSystem". Unknown names are ignored.
void sound_play(const char* name);
// Starts decoding a sound in the background now, so it's ready when first played.
void sound_prefetch(const char* name);
// Frees every decoded effect that isn't playing (bar the interface sounds and ship explosions):
// called at each jump, so memory holds only what the current beacon needs.
void sound_release_unused(void);
// Frees the memory of every sound that isn't playing (they're decoded again when next played),
// for when textures need the room.
void sound_free_idle(void);
// Decoded sound memory: in linear memory (played at least once) and in the normal heap.
void sound_stats(unsigned* linearBytes, unsigned* heapBytes, int* decoded);

// Starts or stops a looping sound (sounds.xml entries with loop="true", e.g. "repair").
void sound_loop(const char* name, bool on);

// Streams a music track by its sounds.xml name, e.g. "civilian", looping. Another track fades
// out first (as FTL does between playlists); asking for the one already playing does nothing.
void sound_music(const char* track);

// Advances a pending track change (fade out, pause, start). Call once per frame.
void sound_update(float dt);

// True once each time the current track reaches its end (time for the next in the playlist).
bool sound_music_ended(void);

// Crossfades to the track's battle version (or back to explore), in step with the music.
void sound_music_battle(bool battle);

// Picks a random sector track name for sound_music().
const char* sound_random_track(void);
