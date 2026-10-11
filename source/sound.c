#include "sound.h"

#include <3ds.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tremor/ivorbisfile.h>

#include "xml.h"

#define MAX_SFX 192
#define MAX_TRACKS 24
#define FIRST_SFX_CHANNEL 2  // 0 and 1 are music (explore and battle)
#define CHANNELS 16
#define LOOP_CHANNELS 5  // the last channels are kept for looping sounds
#define MUSIC_BUFS 3
#define MUSIC_FRAMES 4096  // per buffer, about 93 ms at 44.1 kHz
#define MUSIC_VOLUME 0.75f
// The 3DS speakers are quiet and FTL's audio is mastered with lots of headroom (music averages
// about -21 dBFS, half the effects peak below 0.42), so both are played louder than recorded.
// Effects: sounds.xml's volume times SFX_GAIN, but never more than takes that sound's own peak to
// SFX_PEAK (measured when it's decoded), so no effect clips on its own.
#define SFX_GAIN 3.0f
#define SFX_PEAK 0.97f
// Music: every decoded sample is boosted by MUSIC_BOOST, and the rare peaks that would pass
// MUSIC_KNEE of full scale are rounded off smoothly (a soft limiter) instead of clipping.
#define MUSIC_BOOST 1.8f
#define MUSIC_KNEE 0.7f
// Music timings from xftl (doc/music): explore <-> battle crossfade over 2 s, the explore version at
// 80% of the battle version's volume; changing playlists fades out over 0.75 s and starts the new
// one 2 s after the fade began.
#define MUSIC_FADE 2.0f
#define EXPLORE_VOLUME 0.8f
#define SWITCH_FADE 0.75f
#define SWITCH_START 2.0f

typedef struct {
    char name[32];
    char file[64];  // inside ftl.dat
    float volume;   // 0..1
    bool loop;
    s16* pcm;       // linear memory, interleaved
    u32 frames;
    int channels, rate;
    // Decoding happens on the loader thread; `ready` is set once pcm is complete, `failed` if it
    // couldn't be decoded (a play asked for meanwhile waits for one or the other).
    volatile bool ready, failed, queued, playPending;
    bool linear;  // pcm moved to linear memory (the DSP reads it from there); main thread only
    bool priority;  // must not go missing (ship explosions): preloaded first, never cut off
    u64 pendingAt;  // when playPending was set (ms): a play left waiting too long is dropped
    float peak;     // loudest sample, 0..1 of full scale (for SFX_PEAK)
} Sfx;

typedef struct {
    char name[24];
    char explore[48], combat[48];
} Track;

static FtlDat* sDat;
static Sfx sSfx[MAX_SFX];
static int sSfxCount;
static Track sTracks[MAX_TRACKS];
static int sTrackCount;
static bool sReady;
static const char* sError = "Sound not started";
static ndspWaveBuf sSfxBuf[CHANNELS];
static struct {
    const Sfx* sfx;
    bool on;
} sLoops[LOOP_CHANNELS];  // looping sounds on the last channels
static Sfx* sChanSfx[CHANNELS];  // what each channel was last given
static int sNextChannel = FIRST_SFX_CHANNEL;

static bool is_priority_sound(const char* name) {
    return !strcmp(name, "explosion1") || !strcmp(name, "explosion2");
}

// Interface sounds: small, used all the time, kept decoded.
static const char* const UI_SOUNDS[] = {"powerUpSystem", "powerDownSystem", "powerUpFail", "hoverBeep", "jumpLeave",
                                        "jumpArrive", "upgradeSystem", "buy", "shieldsUp"};

static bool is_ui_sound(const char* name) {
    for (size_t i = 0; i < sizeof(UI_SOUNDS) / sizeof(UI_SOUNDS[0]); i++)
        if (!strcmp(name, UI_SOUNDS[i])) return true;
    return false;
}

// Sounds kept decoded for good (interface, ship explosions): outside the heap budget.
static bool kept_sound(const Sfx* s) { return s->priority || is_ui_sound(s->name); }

// ---- Vorbis from memory --------------------------------------------------------

typedef struct {
    const uint8_t* data;
    size_t size, pos;
} MemFile;

static size_t mem_read(void* dst, size_t size, size_t n, void* src) {
    MemFile* m = src;
    size_t want = size * n, left = m->size - m->pos;
    if (want > left) want = left;
    memcpy(dst, m->data + m->pos, want);
    m->pos += want;
    return size ? want / size : 0;
}

static int mem_seek(void* src, ogg_int64_t off, int whence) {
    MemFile* m = src;
    ogg_int64_t p = whence == SEEK_SET ? off : whence == SEEK_CUR ? (ogg_int64_t)m->pos + off : (ogg_int64_t)m->size + off;
    if (p < 0 || p > (ogg_int64_t)m->size) return -1;
    m->pos = (size_t)p;
    return 0;
}

static int mem_close(void* src) { return 0; }
static long mem_tell(void* src) { return (long)((MemFile*)src)->pos; }
static const ov_callbacks MEM_CALLBACKS = {mem_read, mem_seek, mem_close, mem_tell};

// ---- Decoding ------------------------------------------------------------------

static bool decode_wav(Sfx* s, const uint8_t* d, uint32_t size) {
    if (size < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
    int fmt = 0, bits = 0;
    for (uint32_t p = 12; p + 8 <= size;) {
        uint32_t len = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
        const uint8_t* c = d + p + 8;
        if (len > size - p - 8) len = size - p - 8;
        if (!memcmp(d + p, "fmt ", 4) && len >= 16) {
            fmt = c[0] | c[1] << 8;
            s->channels = c[2] | c[3] << 8;
            s->rate = c[4] | c[5] << 8 | c[6] << 16 | c[7] << 24;
            bits = c[14] | c[15] << 8;
        } else if (!memcmp(d + p, "data", 4)) {
            if (fmt != 1 || bits != 16 || s->channels < 1 || s->channels > 2) return false;
            s->frames = len / (2 * s->channels);
            s->pcm = malloc(s->frames * s->channels * 2);
            if (!s->pcm) return false;
            memcpy(s->pcm, c, s->frames * s->channels * 2);
            return true;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

static bool decode_ogg(Sfx* s, const uint8_t* d, uint32_t size) {
    MemFile m = {d, size, 0};
    OggVorbis_File vf;
    if (ov_open_callbacks(&m, &vf, NULL, 0, MEM_CALLBACKS) < 0) return false;
    vorbis_info* vi = ov_info(&vf, -1);
    ogg_int64_t total = ov_pcm_total(&vf, -1);
    bool ok = false;
    if (vi && vi->channels >= 1 && vi->channels <= 2 && total > 0) {
        s->channels = vi->channels;
        s->rate = (int)vi->rate;
        s->pcm = malloc((size_t)total * s->channels * 2);
        if (s->pcm) {
            size_t want = (size_t)total * s->channels * 2, got = 0;
            int section;
            long n;
            while (got < want && (n = ov_read(&vf, (char*)s->pcm + got, (int)(want - got), &section)) > 0) got += n;
            s->frames = got / (2 * s->channels);
            ok = s->frames > 0;
        }
    }
    ov_clear(&vf);
    return ok;
}

// ---- Background loading -------------------------------------------------------
// Decoding an effect takes long enough to stall a frame, so it happens on a lower-priority
// thread: queued when first played (it plays once ready), and common combat sounds are preloaded
// in the background while there's linear memory to spare.

#define LOAD_QUEUE 64
#define PRELOAD_BUDGET (4 * 1024 * 1024)  // bytes of decoded audio preloaded ahead of use
#define LINEAR_RESERVE (16 * 1024 * 1024)  // keep this much linear memory for textures

static bool sfx_load(Sfx* s);
static Thread sLoader;
static LightEvent sLoadWake;
static volatile bool sLoaderQuit;
static Sfx* volatile sQueue[LOAD_QUEUE];
static volatile int sQueueHead, sQueueTail;  // main thread writes the tail, the loader the head
static size_t sPreloaded;
// Decoded effects wait in the normal heap until first played (then they move to linear memory).
// Decoding ahead (prefetch, preload) stops at this much, so a long run can't fill the heap and
// leave no room for a music track (each is read whole, a few MB).
#define HEAP_AHEAD_BUDGET (3 * 1024 * 1024)
static volatile int sHeapPcm;  // bytes of decoded effects in the heap (not counting the kept ones)

static void sfx_request(Sfx* s) {
    if (s->ready || s->queued || s->failed) return;
    int next = (sQueueTail + 1) % LOAD_QUEUE;
    if (next == sQueueHead) return;  // full; asked again on the next play
    s->queued = true;
    sQueue[sQueueTail] = s;
    __sync_synchronize();
    sQueueTail = next;
    LightEvent_Signal(&sLoadWake);
}

static bool worth_preloading(const Sfx* s) {
    static const char* const keys[] = {"explosion", "jump", "hit", "shield", "laser", "missile", "ion", "beam",
                                       "death", "door", "power", "beep", "bomb", "flak", "drone", "teleport"};
    char lower[32];
    size_t n = strlen(s->name);
    for (size_t i = 0; i <= n && i < sizeof(lower); i++) lower[i] = (char)tolower((unsigned char)s->name[i]);
    lower[sizeof(lower) - 1] = '\0';
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strstr(lower, keys[i])) return true;
    return false;
}

static void loader_thread(void* arg) {
    int preload = 0;
    while (!sLoaderQuit) {
        if (sQueueHead != sQueueTail) {
            Sfx* s = sQueue[sQueueHead];
            sQueueHead = (sQueueHead + 1) % LOAD_QUEUE;
            // A sound asked for ahead of time waits while the heap holds enough already.
            bool ahead = !s->playPending;
            if (!s->ready && !s->failed && !(ahead && sHeapPcm > HEAP_AHEAD_BUDGET) && sfx_load(s)) {
                if (!kept_sound(s)) __sync_fetch_and_add(&sHeapPcm, (int)(s->frames * s->channels * 2));
                __sync_synchronize();
                s->ready = true;
            }
            if (!s->ready) s->playPending = false;  // couldn't decode it now: that play is dropped
            s->queued = false;
            continue;
        }
        // Idle: the must-play sounds first, then likely sounds in file order.
        Sfx* must = NULL;
        for (int i = 0; i < sSfxCount && !must; i++)
            if (sSfx[i].priority && !sSfx[i].ready && !sSfx[i].failed && !sSfx[i].queued) must = &sSfx[i];
        if (must) {
            if (sfx_load(must)) {
                if (!kept_sound(must)) __sync_fetch_and_add(&sHeapPcm, (int)(must->frames * must->channels * 2));
                __sync_synchronize();
                must->ready = true;
            } else {
                LightEvent_WaitTimeout(&sLoadWake, 500 * 1000000LL);  // short of memory: try again later
            }
            continue;
        }
        while (preload < sSfxCount && (sSfx[preload].ready || sSfx[preload].failed || !worth_preloading(&sSfx[preload])))
            preload++;
        if (preload < sSfxCount && sPreloaded < PRELOAD_BUDGET && linearSpaceFree() > LINEAR_RESERVE && sHeapPcm < HEAP_AHEAD_BUDGET) {
            Sfx* s = &sSfx[preload++];
            if (sfx_load(s)) {
                sPreloaded += s->frames * s->channels * 2;
                if (!kept_sound(s)) __sync_fetch_and_add(&sHeapPcm, (int)(s->frames * s->channels * 2));
                __sync_synchronize();
                s->ready = true;
            }
            continue;
        }
        LightEvent_WaitTimeout(&sLoadWake, 200 * 1000000LL);
    }
}

// The 3DS's DSP mixes at 32728 Hz (it resamples every voice to that), and FTL's effects are almost all
// 44.1 kHz stereo. Stored as decoded they'd take a third more memory than the output can use, so
// they're resampled to the DSP's rate here (linear interpolation, the same the DSP was doing on
// playback), and those whose two channels are the same (about a third) are kept as mono.
#define DSP_RATE 32728
#define MONO_DIFF 0.1f  // left/right RMS difference, relative to the signal, below which it's mono

static void shrink_pcm(Sfx* s) {
    int ch = s->channels;
    u32 frames = s->frames;
    s16* p = s->pcm;
    if (!p || frames < 2) return;
    // Mono when the channels match.
    if (ch == 2) {
        double diff = 0.0, sig = 0.0;
        for (u32 i = 0; i < frames; i++) {
            double l = p[2 * i], r = p[2 * i + 1], m = (l + r) * 0.5;
            diff += (l - r) * (l - r);
            sig += m * m;
        }
        if (diff <= MONO_DIFF * MONO_DIFF * sig) {
            for (u32 i = 0; i < frames; i++) p[i] = (s16)((p[2 * i] + p[2 * i + 1]) / 2);
            ch = 1;
        }
    }
    // Down to the DSP's rate (in place: each output frame reads at or past where it writes).
    u32 out = frames;
    if (s->rate > DSP_RATE) {
        double step = (double)s->rate / DSP_RATE;
        out = (u32)((frames - 1) / step) + 1;
        for (u32 j = 0; j < out; j++) {
            double pos = j * step;
            u32 i0 = (u32)pos;
            u32 i1 = i0 + 1 < frames ? i0 + 1 : i0;
            float f = (float)(pos - i0);
            for (int c = 0; c < ch; c++) p[j * ch + c] = (s16)(p[i0 * ch + c] * (1.0f - f) + p[i1 * ch + c] * f);
        }
        s->rate = DSP_RATE;
    }
    s->channels = ch;
    s->frames = out;
    int peak = 0;
    for (u32 i = 0; i < out * ch; i++) {
        int a = p[i] < 0 ? -p[i] : p[i];
        if (a > peak) peak = a;
    }
    s->peak = peak / 32768.0f;
    s16* small = realloc(p, (size_t)out * ch * 2);
    if (small) s->pcm = small;
}

static bool sfx_load(Sfx* s) {
    uint32_t size;
    uint8_t* d = ftldat_read(sDat, s->file, &size);
    if (!d) {
        // A file that isn't there is gone for good; one that couldn't be read for lack of
        // memory is tried again when next played.
        if (!ftldat_find(sDat, s->file)) s->failed = true;
        return false;
    }
    size_t n = strlen(s->file);
    bool ok = n > 4 && !strcmp(s->file + n - 4, ".wav") ? decode_wav(s, d, size) : decode_ogg(s, d, size);
    free(d);
    if (!ok) {
        // Out of memory (decoders leave pcm NULL when malloc fails): retried on the next play.
        bool oom = !s->pcm;
        free(s->pcm);
        s->pcm = NULL;
        if (!oom) s->failed = true;
        return false;
    }
    shrink_pcm(s);
    return true;
}

// ---- sounds.xml ----------------------------------------------------------------

static void parse_sounds(const char* xml) {
    const char* musicEnd = strstr(xml, "</music>");
    // Music tracks.
    const char* p = xml;
    const char* end;
    const char* t;
    while (musicEnd && sTrackCount < MAX_TRACKS && (t = xml_find_tag_before(p, musicEnd, "track", &end))) {
        p = end + 1;
        const char* close = strstr(p, "</track>");
        Track* tr = &sTracks[sTrackCount];
        const char* e;
        // <combat> is optional: the title and Last Stand have only one version.
        tr->combat[0] = '\0';
        if ((t = xml_find_tag_before(p, close, "combat", &e))) xml_text(e, tr->combat, sizeof(tr->combat));
        if ((t = xml_find_tag_before(p, close, "name", &e)) && xml_text(e, tr->name, sizeof(tr->name)) &&
            (t = xml_find_tag_before(p, close, "explore", &e)) && xml_text(e, tr->explore, sizeof(tr->explore)))
            sTrackCount++;
    }

    // Sound effects: every other element whose text is a .wav/.ogg path.
    p = musicEnd ? musicEnd : xml;
    while ((p = strchr(p, '<')) != NULL && sSfxCount < MAX_SFX) {
        if (!strncmp(p, "<!--", 4)) {
            const char* c = strstr(p, "-->");
            if (!c) break;
            p = c + 3;
            continue;
        }
        end = strchr(p, '>');
        if (!end) break;
        size_t n = strcspn(p + 1, " \t\r\n/>");
        Sfx* s = &sSfx[sSfxCount];
        char file[48];
        if (p[1] != '/' && p[1] != '?' && n > 0 && n < sizeof(s->name) && xml_text(end, file, sizeof(file))) {
            size_t fl = strlen(file);
            if (fl > 4 && (!strcmp(file + fl - 4, ".wav") || !strcmp(file + fl - 4, ".ogg"))) {
                memset(s, 0, sizeof(*s));
                memcpy(s->name, p + 1, n);
                snprintf(s->file, sizeof(s->file), "audio/waves/%s", file);
                int vol = 10;
                char loop[8] = "";
                xml_attr_int(p, end, "volume", &vol);
                xml_attr(p, end, "loop", loop, sizeof(loop));
                s->volume = vol / 10.0f;
                s->priority = is_priority_sound(s->name);
                s->loop = !strcmp(loop, "true");
                sSfxCount++;
            }
        }
        p = end + 1;
    }
}

// ---- Music streaming -----------------------------------------------------------
//
// Like FTL, each track's explore and battle versions run in step and the game crossfades between
// them: the incoming version starts at the outgoing one's playback position.

typedef struct {
    uint8_t* ogg;  // whole compressed file
    MemFile mem;
    OggVorbis_File vf;
    bool open, playing;  // file loaded / feeding the DSP
    s16* pcm;            // MUSIC_BUFS * MUSIC_FRAMES stereo frames, linear memory
    ndspWaveBuf bufs[MUSIC_BUFS];
    int channels, rate, chn;
    float gain;
} Stream;

static struct {
    Thread thread;
    LightEvent wake;
    LightLock lock;
    volatile bool quit;
    Stream s[2];      // explore, battle
    volatile int want;  // which one should be audible
    u64 lastTick;
} sMusic;

static void dsp_callback(void* arg) { LightEvent_Signal(&sMusic.wake); }

static volatile float sMaster = 1.0f;  // fades out when switching tracks
static volatile bool sEnded;           // the explore version reached its end (next playlist track)

static float sMusicVol = 1.0f, sSfxVol = 1.0f;  // the options' volume settings, 0..1

static void stream_mix(Stream* st) {
    float v = MUSIC_VOLUME * sMusicVol * st->gain * sMaster * (st->chn == 0 ? EXPLORE_VOLUME : 1.0f);
    float mix[12] = {v, v};
    ndspChnSetMix(st->chn, mix);
}

// MUSIC_BOOST with a soft knee, as a table from every 16-bit sample to its output: linear up to
// MUSIC_KNEE, then easing towards full scale (y = knee + (1 - knee) * u / (1 + u)) so loud passages
// round off instead of clipping. Built once (128 KB).
static s16* sMusicCurve;

static void music_curve_init(void) {
    if (sMusicCurve) return;
    sMusicCurve = malloc(65536 * sizeof(s16));
    if (!sMusicCurve) return;
    for (int i = 0; i < 65536; i++) {
        float x = (i - 32768) / 32768.0f * MUSIC_BOOST, a = x < 0.0f ? -x : x;
        if (a > MUSIC_KNEE) {
            float u = (a - MUSIC_KNEE) / (1.0f - MUSIC_KNEE);
            a = MUSIC_KNEE + (1.0f - MUSIC_KNEE) * u / (1.0f + u);
        }
        int y = (int)((x < 0.0f ? -a : a) * 32767.0f);
        sMusicCurve[i] = (s16)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
}

// Fills one buffer, looping the track at its end.
static bool stream_fill(Stream* st, ndspWaveBuf* b) {
    size_t want = MUSIC_FRAMES * st->channels * 2, got = 0;
    int section, eofs = 0;
    while (got < want) {
        long n = ov_read(&st->vf, (char*)b->data_pcm16 + got, (int)(want - got), &section);
        if (n > 0) {
            got += n;
        } else if (n == 0 && ++eofs < 2) {
            ov_pcm_seek(&st->vf, 0);
            if (st->chn == 0) sEnded = true;
        } else {
            break;
        }
    }
    if (got == 0) return false;
    b->nsamples = got / (2 * st->channels);
    if (sMusicCurve)
        for (size_t i = 0; i < got / 2; i++) b->data_pcm16[i] = sMusicCurve[(u16)(b->data_pcm16[i] + 32768)];
    DSP_FlushDataCache(b->data_pcm16, got);
    ndspChnWaveBufAdd(st->chn, b);
    return true;
}

// The sample `ref` is playing right now: decoded position minus what's still queued.
static s64 stream_heard(const Stream* ref) {
    s64 pos = ov_pcm_tell((OggVorbis_File*)&ref->vf);
    for (int i = 0; i < MUSIC_BUFS; i++) {
        const ndspWaveBuf* b = &ref->bufs[i];
        if (b->status == NDSP_WBUF_QUEUED || b->status == NDSP_WBUF_PLAYING) pos -= b->nsamples;
    }
    pos += ndspChnGetSamplePos(ref->chn);
    return pos < 0 ? 0 : pos;
}

// Starts feeding a stream, in step with `ref` if that one is playing.
static void stream_start(Stream* st, const Stream* ref) {
    s64 pos = 0;
    if (ref && ref->playing) {
        pos = stream_heard(ref) * st->rate / (ref->rate ? ref->rate : st->rate);
        s64 total = ov_pcm_total(&st->vf, -1);
        if (total > 0) pos %= total;
    }
    ov_pcm_seek(&st->vf, pos);
    ndspChnReset(st->chn);
    ndspChnSetInterp(st->chn, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(st->chn, (float)st->rate);
    ndspChnSetFormat(st->chn, st->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    stream_mix(st);
    memset(st->bufs, 0, sizeof(st->bufs));
    for (int i = 0; i < MUSIC_BUFS; i++) st->bufs[i].data_pcm16 = st->pcm + i * MUSIC_FRAMES * 2;
    st->playing = true;
}

static void stream_halt(Stream* st) {
    if (!st->playing) return;
    ndspChnWaveBufClear(st->chn);
    st->playing = false;
}

static void music_thread(void* arg) {
    while (!sMusic.quit) {
        LightLock_Lock(&sMusic.lock);
        u64 now = osGetTime();
        float dt = (now - sMusic.lastTick) / 1000.0f;
        sMusic.lastTick = now;
        int want = sMusic.want;
        if (!sMusic.s[want].open) want = 0;  // no battle version: stay on explore
        Stream* in = &sMusic.s[want];
        Stream* out = &sMusic.s[!want];
        if (in->open && !in->playing) stream_start(in, out);
        // Crossfade.
        float step = dt / MUSIC_FADE;
        in->gain = in->gain + step > 1.0f ? 1.0f : in->gain + step;
        out->gain = out->gain - step < 0.0f ? 0.0f : out->gain - step;
        for (int k = 0; k < 2; k++) {
            Stream* st = &sMusic.s[k];
            if (!st->playing) continue;
            stream_mix(st);
            if (st == out && st->gain <= 0.0f) {
                stream_halt(st);
                continue;
            }
            for (int i = 0; i < MUSIC_BUFS; i++) {
                ndspWaveBuf* b = &st->bufs[i];
                if ((b->status == NDSP_WBUF_FREE || b->status == NDSP_WBUF_DONE) && !stream_fill(st, b)) stream_halt(st);
            }
        }
        LightLock_Unlock(&sMusic.lock);
        LightEvent_WaitTimeout(&sMusic.wake, 40 * 1000000LL);  // wake for fades too
    }
}

static void stream_close(Stream* st) {
    stream_halt(st);
    if (st->open) ov_clear(&st->vf);
    free(st->ogg);
    st->ogg = NULL;
    st->open = false;
    st->gain = 0.0f;
}

static void music_stop(void) {
    if (sMusic.thread) {
        sMusic.quit = true;
        LightEvent_Signal(&sMusic.wake);
        threadJoin(sMusic.thread, U64_MAX);
        threadFree(sMusic.thread);
        sMusic.thread = NULL;
    }
    for (int k = 0; k < 2; k++) stream_close(&sMusic.s[k]);
}

static bool stream_open(Stream* st, const char* file) {
    if (!file[0]) return false;
    char path[80];
    snprintf(path, sizeof(path), "audio/music/%s", file);
    uint32_t size;
    st->ogg = ftldat_read(sDat, path, &size);
    if (!st->ogg) return false;
    st->mem.size = size;
    st->mem.data = st->ogg;
    st->mem.pos = 0;
    if (ov_open_callbacks(&st->mem, &st->vf, NULL, 0, MEM_CALLBACKS) < 0) {
        free(st->ogg);
        st->ogg = NULL;
        return false;
    }
    vorbis_info* vi = ov_info(&st->vf, -1);
    st->channels = vi->channels == 1 ? 1 : 2;
    st->rate = (int)vi->rate;
    st->open = true;
    return true;
}

static char sTrackName[32];

static char sPending[32];
static float sPendingTime = -1.0f;

static void music_start(const char* track);
static void free_heap_sounds(void);
static u64 sMusicRetryAt;  // after a failed start: when to try again (ms, osGetTime)

void sound_music(const char* track) {
    if (!sReady) return;
    if (!track) track = "";
    if (sPendingTime >= 0.0f ? !strcmp(track, sPending) : !strcmp(track, sTrackName) && sMusic.thread) return;
    if (!sMusic.thread && sPendingTime < 0.0f && !strcmp(track, sTrackName) && osGetTime() < sMusicRetryAt) return;
    if (!sMusic.thread) {  // nothing playing: start now
        music_start(track);
        return;
    }
    snprintf(sPending, sizeof(sPending), "%s", track);
    sPendingTime = 0.0f;
}

void sound_update(float dt) {
    // Effects that were asked for before they were decoded.
    for (int i = 0; i < sSfxCount; i++)
        if (sSfx[i].playPending && (sSfx[i].ready || sSfx[i].failed)) {
            sSfx[i].playPending = false;
            // Decoded in time to still go with what it's for (a hit, a door): play it, else drop it.
            if (sSfx[i].ready && osGetTime() - sSfx[i].pendingAt < 1500) sound_play(sSfx[i].name);
        }
    if (sPendingTime < 0.0f) return;
    sPendingTime += dt;
    float m = 1.0f - sPendingTime / SWITCH_FADE;
    sMaster = m > 0.0f ? m : 0.0f;
    if (sPendingTime >= SWITCH_START) {
        sPendingTime = -1.0f;
        music_start(sPending);
        sMaster = 1.0f;
    }
}

bool sound_music_ended(void) {
    bool e = sEnded;
    sEnded = false;
    return e;
}

static void music_start(const char* track) {
    music_stop();
    sEnded = false;
    snprintf(sTrackName, sizeof(sTrackName), "%s", track ? track : "");
    const Track* tr = NULL;
    for (int i = 0; track && i < sTrackCount && !tr; i++)
        if (!strcmp(sTracks[i].name, track)) tr = &sTracks[i];
    if (!tr) return;
    if (!stream_open(&sMusic.s[0], tr->explore)) {
        free_heap_sounds();  // likely no room for the track: make some and try once more
        if (!stream_open(&sMusic.s[0], tr->explore)) {
            sMusicRetryAt = osGetTime() + 3000;
            return;
        }
    }
    if (!stream_open(&sMusic.s[1], tr->combat) && tr->combat[0]) {
        free_heap_sounds();
        stream_open(&sMusic.s[1], tr->combat);
    }
    // The version that's wanted starts at full volume; the other fades in when asked for.
    int want = sMusic.want && sMusic.s[1].open ? 1 : 0;
    sMusic.s[want].gain = 1.0f;

    // Decode ahead of the main thread so loading never starves the DSP.
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sMusic.quit = false;
    sMusic.lastTick = osGetTime();
    sMusic.thread = threadCreate(music_thread, NULL, 64 * 1024, prio > 0x18 ? prio - 1 : prio, -1, false);
    if (!sMusic.thread) {
        music_stop();
        sMusicRetryAt = osGetTime() + 3000;
    }
}

void sound_music_battle(bool battle) {
    if (sMusic.want == (int)battle) return;
    sMusic.want = battle;
    LightEvent_Signal(&sMusic.wake);
}

const char* sound_random_track(void) {
    // Sector tracks only (each has a battle version), not the title or Last Stand.
    int n = 0;
    for (int i = 0; i < sTrackCount; i++) n += sTracks[i].combat[0] != '\0';
    for (int i = 0, pick = n ? rand() % n : 0; i < sTrackCount; i++)
        if (sTracks[i].combat[0] && pick-- == 0) return sTracks[i].name;
    return "";
}

// ---- Public --------------------------------------------------------------------

bool sound_init(FtlDat* dat) {
    sDat = dat;
    for (int i = 0; i < 2; i++) {
        char* xml = (char*)ftldat_read(dat, i ? "data/dlcSounds.xml" : "data/sounds.xml", NULL);
        if (xml) parse_sounds(xml);
        free(xml);
    }
    if (R_FAILED(ndspInit())) {
        sError = "No sound: sdmc:/3ds/dspfirm.cdc is missing";
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    for (int k = 0; k < 2; k++) {
        sMusic.s[k].pcm = linearAlloc(MUSIC_BUFS * MUSIC_FRAMES * 2 * sizeof(s16));
        sMusic.s[k].chn = k;
    }
    music_curve_init();
    LightEvent_Init(&sMusic.wake, RESET_ONESHOT);
    LightLock_Init(&sMusic.lock);
    ndspSetCallback(dsp_callback, NULL);
    sReady = true;
    sError = NULL;

    LightEvent_Init(&sLoadWake, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sLoaderQuit = false;
    // Below the main thread's priority, so it only runs while the main thread waits for vblank.
    sLoader = threadCreate(loader_thread, NULL, 32 * 1024, prio < 0x3F ? prio + 1 : prio, -1, false);
    // Loaded first: UI feedback and the sounds of the first jump and the ship screen.
    for (size_t i = 0; i < sizeof(UI_SOUNDS) / sizeof(UI_SOUNDS[0]); i++)
        for (int j = 0; j < sSfxCount; j++)
            if (!strcmp(sSfx[j].name, UI_SOUNDS[i])) sfx_request(&sSfx[j]);
    return true;
}

void sound_set_volume(float music, float sfx) {
    sMusicVol = music < 0.0f ? 0.0f : music > 1.0f ? 1.0f : music;
    sSfxVol = sfx < 0.0f ? 0.0f : sfx > 1.0f ? 1.0f : sfx;
    if (!sReady) return;
    for (int k = 0; k < 2; k++)
        if (sMusic.s[k].open) stream_mix(&sMusic.s[k]);
}

void sound_exit(void) {
    if (!sReady) return;
    music_stop();
    if (sLoader) {
        sLoaderQuit = true;
        LightEvent_Signal(&sLoadWake);
        threadJoin(sLoader, U64_MAX);
        threadFree(sLoader);
        sLoader = NULL;
    }
    ndspSetCallback(NULL, NULL);
    for (int c = 0; c < CHANNELS; c++) ndspChnWaveBufClear(c);
    ndspExit();
    for (int i = 0; i < sSfxCount; i++)
        if (sSfx[i].pcm) (sSfx[i].linear ? linearFree : free)(sSfx[i].pcm);
    for (int k = 0; k < 2; k++) linearFree(sMusic.s[k].pcm);
    free(sMusicCurve);
    sMusicCurve = NULL;
    sReady = false;
}

const char* sound_error(void) { return sError; }

static bool start_on_channel(Sfx* s, int ch, bool loop);

void sound_prefetch(const char* name) {
    if (!sReady || !name || !name[0]) return;
    for (int i = 0; i < sSfxCount; i++)
        if (!strcmp(sSfx[i].name, name)) {
            sfx_request(&sSfx[i]);
            return;
        }
}

void sound_play(const char* name) {
    if (!sReady) return;
    Sfx* s = NULL;
    for (int i = 0; i < sSfxCount && !s; i++)
        if (!strcmp(sSfx[i].name, name)) s = &sSfx[i];
    if (!s) return;
    if (!s->ready) {  // decoded in the background; it plays as soon as it's ready
        sfx_request(s);
        if (!s->playPending) s->pendingAt = osGetTime();
        s->playPending = true;
        return;
    }

    // Round-robin over the effect channels, preferring idle ones; when all are busy the next one
    // is cut off, but never one playing a must-play sound.
    int ch = -1;
    const int shared = CHANNELS - LOOP_CHANNELS - FIRST_SFX_CHANNEL;
    for (int i = 0; i < shared && ch < 0; i++) {
        int c = FIRST_SFX_CHANNEL + (sNextChannel - FIRST_SFX_CHANNEL + i) % shared;
        if (!ndspChnIsPlaying(c)) ch = c;
    }
    for (int i = 0; i < shared && ch < 0; i++) {
        int c = FIRST_SFX_CHANNEL + (sNextChannel - FIRST_SFX_CHANNEL + i) % shared;
        if (!(sChanSfx[c] && sChanSfx[c]->priority)) ch = c;
    }
    if (ch < 0) ch = sNextChannel;
    sNextChannel = FIRST_SFX_CHANNEL + (ch - FIRST_SFX_CHANNEL + 1) % shared;
    start_on_channel(s, ch, false);
}


void sound_loop(const char* name, bool on) {
    if (!sReady) return;
    int slot = -1, idle = -1;
    for (int i = 0; i < LOOP_CHANNELS; i++) {
        if (sLoops[i].on && !strcmp(sLoops[i].sfx->name, name)) slot = i;
        if (!sLoops[i].on && idle < 0) idle = i;
    }
    int ch = CHANNELS - LOOP_CHANNELS;
    if (!on) {
        if (slot >= 0) {
            ndspChnWaveBufClear(ch + slot);
            sLoops[slot].on = false;
        }
        return;
    }
    if (slot >= 0 || idle < 0) return;  // already playing, or no channel left
    Sfx* s = NULL;
    for (int i = 0; i < sSfxCount && !s; i++)
        if (!strcmp(sSfx[i].name, name)) s = &sSfx[i];
    if (!s) return;
    if (!s->ready) {  // starts on a later frame, once decoded
        sfx_request(s);
        return;
    }
    sLoops[idle].sfx = s;
    sLoops[idle].on = start_on_channel(s, ch + idle, true);  // retried next frame if it couldn't start
}

// Frees the linear memory of the largest decoded sound that isn't playing (it's decoded again when
// next wanted). False if there's nothing to free.
static bool evict_one(const Sfx* keep) {
    Sfx* best = NULL;
    for (int i = 0; i < sSfxCount; i++) {
        Sfx* s = &sSfx[i];
        if (s == keep || !s->linear || !s->ready || s->queued || s->priority) continue;
        bool busy = false;
        for (int c = 0; c < CHANNELS && !busy; c++) busy = sChanSfx[c] == s && ndspChnIsPlaying(c);
        if (busy) continue;
        if (!best || s->frames * s->channels > best->frames * best->channels) best = s;
    }
    if (!best) return false;
    for (int c = 0; c < CHANNELS; c++)
        if (sChanSfx[c] == best) sChanSfx[c] = NULL;
    best->ready = false;
    __sync_synchronize();
    linearFree(best->pcm);
    best->pcm = NULL;
    best->linear = false;
    return true;
}

void sound_stats(unsigned* linearBytes, unsigned* heapBytes, int* decoded) {
    *linearBytes = *heapBytes = 0;
    *decoded = 0;
    for (int i = 0; i < sSfxCount; i++) {
        const Sfx* s = &sSfx[i];
        if (!s->ready || !s->pcm) continue;
        unsigned b = s->frames * s->channels * 2;
        if (s->linear)
            *linearBytes += b;
        else
            *heapBytes += b;
        (*decoded)++;
    }
}

// Frees effects decoded ahead but not yet played (they decode again when wanted).
static void free_heap_sounds(void) {
    for (int i = 0; i < sSfxCount; i++) {
        Sfx* s = &sSfx[i];
        if (!s->ready || s->linear || s->playPending || kept_sound(s) || !s->pcm) continue;
        s->ready = false;
        __sync_synchronize();
        __sync_fetch_and_sub(&sHeapPcm, (int)(s->frames * s->channels * 2));
        free(s->pcm);
        s->pcm = NULL;
    }
}

void sound_release_unused(void) {
    if (!sReady) return;
    for (int i = 0; i < sSfxCount; i++) {
        Sfx* s = &sSfx[i];
        if (!s->ready || !s->pcm || s->priority || s->playPending || is_ui_sound(s->name)) continue;
        bool busy = false;
        for (int c = 0; c < CHANNELS && !busy; c++) busy = sChanSfx[c] == s && ndspChnIsPlaying(c);
        for (int k = 0; k < LOOP_CHANNELS && !busy; k++) busy = sLoops[k].on && sLoops[k].sfx == s;
        if (busy) continue;
        for (int c = 0; c < CHANNELS; c++)
            if (sChanSfx[c] == s) sChanSfx[c] = NULL;
        s->ready = false;
        __sync_synchronize();
        if (s->linear) {
            linearFree(s->pcm);
        } else {
            __sync_fetch_and_sub(&sHeapPcm, (int)(s->frames * s->channels * 2));
            free(s->pcm);
        }
        s->pcm = NULL;
        s->linear = false;
    }
}

void sound_free_idle(void) {
    if (!sReady) return;
    while (evict_one(NULL)) {
    }
}

static bool start_on_channel(Sfx* s, int ch, bool loop) {
    if (!s->linear) {
        // Decoded on the loader thread into the normal heap; the DSP needs linear memory. When
        // that's short, idle sounds make room rather than this one going silent.
        size_t bytes = (size_t)s->frames * s->channels * 2;
        s16* lin = linearAlloc(bytes);
        while (!lin && evict_one(s)) lin = linearAlloc(bytes);
        if (!lin) return false;
        memcpy(lin, s->pcm, bytes);
        free(s->pcm);
        if (!kept_sound(s)) __sync_fetch_and_sub(&sHeapPcm, (int)bytes);
        s->pcm = lin;
        s->linear = true;
        DSP_FlushDataCache(lin, bytes);
    }
    ndspChnWaveBufClear(ch);
    ndspChnReset(ch);
    ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ch, (float)s->rate);
    ndspChnSetFormat(ch, s->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float g = s->volume * SFX_GAIN * sSfxVol;
    float most = s->peak > 0.01f ? SFX_PEAK / s->peak : 1.0f;  // this sound's own clipping point
    if (g > most) g = most;
    float mix[12] = {g, g};
    ndspChnSetMix(ch, mix);
    sChanSfx[ch] = s;
    ndspWaveBuf* b = &sSfxBuf[ch];
    memset(b, 0, sizeof(*b));
    b->data_pcm16 = s->pcm;
    b->nsamples = s->frames;
    b->looping = loop;
    ndspChnWaveBufAdd(ch, b);
    return true;
}
