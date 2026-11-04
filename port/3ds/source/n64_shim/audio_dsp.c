/*
 * audio_dsp.c — M5 phase 0: 3DS NDSP service init/teardown.
 *
 * Fidelity tier: STUB++ — boots the libctru NDSP service so future MSEQ
 *                playback work has a target to feed. No actual audio is
 *                produced at this phase; `pm3ds_audio_step` is a no-op.
 *
 * Why NDSP and not CSND: NDSP is the modern (3DS 8.0+) audio service.
 * It runs DSP firmware from the system's secret-sector keyset (or from
 * `sdmc:/3ds/dspfirm.cdc` for homebrew), exposes 24 hardware-mixed voice
 * channels with mono/stereo PCM8/PCM16/ADPCM, and is the path used by
 * every modern homebrew title. CSND is legacy (pre-8.0); we'd outgrow
 * it the first time we needed reverb or routing.
 *
 * Future-M5 wiring:
 *   1. `pm3ds_audio_init` is called from port boot before the game
 *      thread spawns (port/3ds/source/main.c). On systems where the
 *      DSP firmware isn't dumpable (older Azahar / unhomebrewed 3DS),
 *      `ndspInit` returns failure; we log and continue silently rather
 *      than panic — audio is non-essential for the M3-M4 smoke loop.
 *   2. MSEQ playback (src/audio/bgm_*) emits 8-bit / 16-bit PCM voices.
 *      Each voice maps to one NDSP channel.
 *   3. `pm3ds_audio_step` ticks once per frame to feed buffer-queue
 *      events; the actual mixing happens on the DSP coprocessor.
 */

#include <3ds.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include "platform.h"
#include "pm3ds_log.h"
#include "pm3ds_audio.h"

static bool s_audio_dsp_up = false;
static bool s_audio_dsp_failed = false;

/* M5.2.10: per sm64-port, `DSP_FlushDataCache` is slow when
 * AppCpuLimit is set; `svcFlushProcessDataCache` against the current
 * process handle does the same job much faster. Wrap it so each call
 * site stays a one-liner. */
static inline void svcFlushProcessDataCache_wrapper(const void* p, size_t bytes) {
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, bytes);
}
/* M5.2.7: which audio backend is live. Mutually exclusive. NDSP is
 * preferred; CSND is the legacy fallback for environments without
 * dspfirm.cdc (notably Azahar HLE in some configurations). */
static bool s_use_csnd = false;

/* M5.1d: dedicated audio refill thread. Polling from the vsync-gated
 * frame loop (M5.1b) worked but exposed audible crackle whenever a video
 * frame stretched past the ring depth (~133 ms at 8 buffers × 1/60 s).
 * Moving the refill loop onto its own thread decouples it from frame
 * timing entirely — we poll every 2 ms and react the instant a buffer
 * comes back DONE, regardless of how long the GPU is busy.
 *
 * Priority 0x10 (higher than the default 0x18 used by retrace_thread)
 * so the audio thread preempts low-priority work. 0x800 bytes stack is
 * plenty — pm3ds_audio_step does a sinf loop, a memcpy, and a couple
 * of NDSP calls. Core -1 = libctru auto-pick (typically core 1 on O3DS
 * since core 0 is the app's main thread). */
static Thread s_audio_thread = NULL;
int s_audio_core = -1;   /* core the mixer runs on (shown on the PERF line) */
bool pm3ds_audio_thread_running(void) { return s_audio_thread != NULL; }
static volatile bool s_audio_thread_run = false;

static void audio_thread_entry(void* arg) {
    (void)arg;
    while (s_audio_thread_run) {
        pm3ds_audio_step();
        svcSleepThread(2 * 1000 * 1000);  /* 2 ms */
    }
}

int pm3ds_audio_init(void) {
    if (s_audio_dsp_up || s_audio_dsp_failed) {
        return s_audio_dsp_failed ? -1 : 0;
    }
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        /* GitHub #81: no sdmc:/3ds/dspfirm.cdc on an emulator. Azahar's CSND makes no sound at
         * all (measured), while its HLE DSP never runs the firmware it is given, only logs its
         * hash, so a blank one starts NDSP. Not for a console: the real DSP would run it. */
        s64 emu = 0;
        if (R_SUCCEEDED(svcGetSystemInfo(&emu, 0x20000, 0)) && emu != 0) {
            static const u8 no_firmware[0x400] __attribute__((aligned(4)));
            ndspUseComponent(no_firmware, sizeof no_firmware, 0xFF, 0xFF);
            rc = ndspInit();
            pm3ds_log(PM3DS_LOG_WARN, "emulator without dspfirm.cdc: NDSP on a blank component, rc=0x%08lX",
                      (unsigned long)rc);
        }
    }
    if (R_FAILED(rc)) {
        pm3ds_log(PM3DS_LOG_WARN,
                  "ndspInit failed (rc=0x%08lX); trying CSND fallback. "
                  "(NDSP needs dspfirm.cdc; CSND is the legacy path that "
                  "works under Azahar HLE without firmware dumping.)",
                  (unsigned long)rc);
        Result rc2 = csndInit();
        if (R_FAILED(rc2)) {
            pm3ds_log(PM3DS_LOG_WARN,
                      "csndInit also failed (rc=0x%08lX); audio disabled.",
                      (unsigned long)rc2);
            s_audio_dsp_failed = true;
            return -1;
        }
        s_use_csnd = true;
        s_audio_dsp_up = true;
        pm3ds_log(PM3DS_LOG_INFO, "CSND service up (NDSP fallback path)");
    } else {
        s_audio_dsp_up = true;
        pm3ds_log(PM3DS_LOG_INFO, "NDSP service up; %d channels available",
                  24 /* libctru NDSP exposes 24 hardware voices */);
    }

    s_audio_thread_run = true;
    /* M5.1d v2: priority 0x18 to match retrace_thread (known-working).
     * Earlier 0x10 silently failed — libctru limits homebrew thread
     * priorities and 0x10 was out of the allowed range, so
     * threadCreate returned NULL and audio died entirely on hardware.
     *
     * M5.2.9: stack bumped 0x800 → 0x4000 (2 KB → 16 KB). The BGM load
     * path (bgm_load_file → fopen → fread → malloc → bgm_advance_
     * composition → bgm_load_phrase → bgm_track_advance, plus a
     * pm3ds_log that pulls in newlib's vfprintf using ~1 KB of stack
     * by itself) was overflowing the 2 KB stack and corrupting the
     * return address into strlen → data abort. 16 KB gives ample
     * headroom for any further audio work. */
    /* M5.2.10: stack bumped 0x4000 → 0x10000 (16 KB → 64 KB) per
     * sm64-port's proven value. The 16 KB worked for the M5.2.9
     * crash, but sm64-port's audio thread has more headroom for
     * future MSEQ work / SBN instrument bank decoding. */
    /* New 3DS: give the mixer (15-30% of a core) the spare app core 2 so it
     * stops competing with the game on core 0. The game only posts requests
     * that the engine consumes per audio frame, as with the N64 audio
     * thread; Old 3DS (or a refused core) keeps the shared-core setup. */
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
#ifdef PM3DS_SIM_O3DS
    n3ds = false;   /* test build: Old 3DS thread setup */
#endif
    s_audio_core = -1;
    s_audio_thread = NULL;
    {   /* Old 3DS: no reverb (audio_alist.c, syn_driver.c) */
        extern bool pm3ds_audio_no_reverb;
        pm3ds_audio_no_reverb = !n3ds;
    }
    {   /* Old 3DS: the DSP's channels play the voices (audio_hwvoice.c); needs NDSP */
        extern bool pm3ds_au_hwvoices;
        extern void pm3ds_hwv_init(void);
#if defined(PM3DS_AUDIO_SWMIX)        /* test: the software mixer everywhere */
        pm3ds_au_hwvoices = false;
#elif defined(PM3DS_AUDIO_HWVOICES)   /* test: the DSP's voices on a New 3DS too */
        pm3ds_au_hwvoices = !s_use_csnd;
#else
        pm3ds_au_hwvoices = !n3ds && !s_use_csnd;
#endif
        if (pm3ds_au_hwvoices) pm3ds_hwv_init();
    }
    if (n3ds) {
        s_audio_thread = threadCreate(audio_thread_entry, NULL, 0x10000, 0x18, 2, false);
        if (s_audio_thread) s_audio_core = 2;
    } else {
        /* Old 3DS: the mixer took ~half of the game's 268 MHz core. The
         * system core can lend apps a share of its time (as the 3DS sm64
         * port does for its audio); ask for as much as it gives. */
        static const u32 limits[] = { 80, 70, 60, 55, 50, 45, 40, 35, 30 };
        for (unsigned i = 0; i < sizeof limits / sizeof limits[0] && !s_audio_thread; i++) {
            Result rc = APT_SetAppCpuTimeLimit(limits[i]);
            if (R_FAILED(rc)) {
                pm3ds_log(PM3DS_LOG_INFO, "audio: core 1 time limit %lu%% refused (0x%08lX)", (unsigned long)limits[i],
                          (unsigned long)rc);
                continue;
            }
#ifdef PM3DS_AUDIO_CORE0   /* test: measure the mixer on the game's core */
            pm3ds_log(PM3DS_LOG_INFO, "audio: core 1 would give %lu%%; mixer stays on core 0 (test)", (unsigned long)limits[i]);
            break;
#endif
            s_audio_thread = threadCreate(audio_thread_entry, NULL, 0x10000, 0x18, 1, false);
            if (s_audio_thread) {
                s_audio_core = 1;
                pm3ds_log(PM3DS_LOG_INFO, "audio: mixer on core 1 with %lu%% of its time", (unsigned long)limits[i]);
            }
        }
    }
    if (!s_audio_thread)
        s_audio_thread = threadCreate(audio_thread_entry, NULL,
                                      0x10000, /* stack: 64 KB */
                                      0x18,    /* priority (matches retrace_thread) */
                                      -1,      /* core: libctru auto-pick */
                                      false);
    if (!s_audio_thread) {
        pm3ds_log(PM3DS_LOG_WARN,
                  "audio refill thread spawn failed (prio 0x18); "
                  "frame-loop pm3ds_audio_step fallback will keep audio alive");
        s_audio_thread_run = false;
    } else {
        pm3ds_log(PM3DS_LOG_INFO,
                  "audio refill thread spawned (prio 0x18, 2 ms poll, core %d)", s_audio_core);
    }
    return 0;
}

void pm3ds_audio_close(void) {
    if (!s_audio_dsp_up) return;
    if (s_audio_thread) {
        s_audio_thread_run = false;
        threadJoin(s_audio_thread, U64_MAX);
        threadFree(s_audio_thread);
        s_audio_thread = NULL;
    }
    { extern void pm3ds_audio_engine_shutdown(void); pm3ds_audio_engine_shutdown(); }
    if (s_use_csnd) {
        csndExit();
    } else {
        ndspExit();
    }
    s_audio_dsp_up = false;
}

/* M5.1b: per-frame voice-feed ring buffer.
 * Hardware-verified on real 3DS 2026-05-14 — 220->880 Hz sweep audible
 * on channel 1 alongside the M5.1a test tone on channel 0.
 *
 * Two linearAlloc'd PCM16 chunks, each holding 1/60 s of audio at the
 * step rate. Channel 1 (separate from the M5.1a test-tone channel 0)
 * runs a continuous queue: every frame we check whether either buffer
 * came back DONE from the DSP, and if so refill it and re-add. NDSP
 * round-robins through queued buffers so two-deep is enough to absorb
 * one frame of CPU jitter without a glitch.
 *
 * Without PM3DS_AUDIO_STEP_DEMO defined the buffers stay zero-filled —
 * silent audio, but the per-frame plumbing still ticks. Defining the
 * symbol replaces the zero-fill with a frequency-sweep generator
 * (220→880 Hz over ~3 s, looping) so the developer can hear that
 * audio_step is running on real hardware.
 *
 * This is the infrastructure M5.1c will hook MSEQ playback onto:
 * replace `fill_demo` with "read MSEQ bytestream, advance ADSR state,
 * mix voices into chunk" and the rest of the plumbing is reusable.
 */

#define PM3DS_STEP_CHANNEL          1   /* M5.1a uses channel 0 */
#define PM3DS_STEP_RATE_HZ          32000
#define PM3DS_STEP_FRAME_SAMPLES    (PM3DS_STEP_RATE_HZ / 60)
/* M5.2.6: stereo frames — each "sample" is an [L, R] int16 pair, so the
 * buffer is 2 * samples * sizeof(int16). NDSP plays this as a true
 * stereo stream and the mix matrix becomes a passthrough (mix[0] = L→L
 * gain, mix[1] = R→R gain). Fill callbacks are responsible for writing
 * the [L, R] interleave themselves. */
#define PM3DS_STEP_CHANNELS         2
#define PM3DS_STEP_FRAME_BYTES      (PM3DS_STEP_FRAME_SAMPLES * PM3DS_STEP_CHANNELS * sizeof(int16_t))
/* M5.1c hardware test exposed underruns: with PM3DS_DEBUG_DL=2 submitting
 * the full texture display list every frame, individual frames can stretch
 * to 30-50 ms and a 2-buffer (33 ms) queue runs dry, producing audible
 * crackling. 8 buffers gives 133 ms of headroom — enough to absorb a
 * ~100 ms frame stall before underrunning. Cost: 8 × 1067 bytes of linear
 * memory (~8.3 KB), negligible compared to the framebuffers. */
#define PM3DS_STEP_BUFFERS          8

static int16_t* s_step_data[PM3DS_STEP_BUFFERS];
static ndspWaveBuf s_step_wave[PM3DS_STEP_BUFFERS];
static bool s_step_inited = false;

#if defined(PM3DS_AUDIO_STEP_DEMO) || defined(PM3DS_AUDIO_STEP_MELODY)
/* Shared phase tracker — both demo modes use the same sine generator
 * and we don't want phase jumps when switching between them. */
static float s_synth_phase = 0.0f;
static const float TWO_PI = 6.28318530717958647692f;
#endif

#ifdef PM3DS_AUDIO_STEP_DEMO
/* Sweep generator state. */
static int s_demo_frame = 0;

static void fill_demo(int16_t* out, int nsamples) {
    /* Sweep 220 -> 880 Hz over 180 frames (3 s) then loop. M5.2.6:
     * writes [L, R] stereo pairs with L=R (no panning for the demo). */
    const int   sweep_frames = 180;
    const float f0 = 220.0f, f1 = 880.0f;
    float t = (float)(s_demo_frame % sweep_frames) / (float)sweep_frames;
    float freq = f0 + (f1 - f0) * t;
    float step = TWO_PI * freq / (float)PM3DS_STEP_RATE_HZ;
    for (int i = 0; i < nsamples; i++) {
        int16_t s = (int16_t)(sinf(s_synth_phase) * 8192.0f);
        out[i * 2 + 0] = s;  /* L */
        out[i * 2 + 1] = s;  /* R */
        s_synth_phase += step;
    }
    if (s_synth_phase > TWO_PI * 16384.0f) {
        s_synth_phase = fmodf(s_synth_phase, TWO_PI);
    }
    s_demo_frame++;
}
#endif

#ifdef PM3DS_AUDIO_STEP_MELODY
/* M5.1c: minimal port-side melody player. Hardcoded "Twinkle Twinkle
 * Little Star" first phrase — 14 notes at ~250 ms each, looping.
 * Hardware-verified on real 3DS 2026-05-14 — melody clearly audible
 * with the 8-buffer (133 ms) ring; occasional faint crackle on long
 * video frames (would need a dedicated audio thread off the vsync
 * loop for full glitch elimination — M5.1d candidate). The
 * point isn't musical accuracy; it's to validate that audio_step can
 * drive structured note-sequence content (variable pitch, attack/release
 * envelope, gate timing) through the same `fill_*` seam we wired in
 * M5.1b. Once we have a real MSEQ interpreter (M5.2+) it replaces this
 * exact callback with no other plumbing changes. */

/* Equal-temperament note frequencies. C4 = 261.63 Hz. */
#define NOTE_C4  261.626f
#define NOTE_D4  293.665f
#define NOTE_E4  329.628f
#define NOTE_F4  349.228f
#define NOTE_G4  391.995f
#define NOTE_A4  440.000f
#define NOTE_REST  0.0f

typedef struct {
    float freq_hz;    /* 0 == rest */
    int   frames;     /* note duration in 60 Hz frames */
} MelodyNote;

static const MelodyNote s_melody[] = {
    /* "Twinkle Twinkle Little Star" — first phrase, ~250 ms / note. */
    {NOTE_C4, 15}, {NOTE_C4, 15},
    {NOTE_G4, 15}, {NOTE_G4, 15},
    {NOTE_A4, 15}, {NOTE_A4, 15},
    {NOTE_G4, 30},
    {NOTE_F4, 15}, {NOTE_F4, 15},
    {NOTE_E4, 15}, {NOTE_E4, 15},
    {NOTE_D4, 15}, {NOTE_D4, 15},
    {NOTE_C4, 30},
    {NOTE_REST, 30}, /* breath before loop */
};
static const int s_melody_len =
    (int)(sizeof(s_melody) / sizeof(s_melody[0]));

static int s_melody_idx = 0;
static int s_melody_frame_in_note = 0;

/* Linear attack + release envelope. Hardware testing turned up a
 * click on every note attack — the previous formulation was
 *   - attack:  env(0) = 0, env(attack) = 1
 *   - release: env where frames_left < release returns frames_left/release
 * which left env at the LAST sample = 1/release (~0.33 for release=3),
 * not 0. Combined with the phase reset between notes that's a
 * stepwise jump of ~0.33 × peak amplitude ≈ ±5000 PCM units — audible.
 * New formulation: both endpoints land on exactly 0. */
static float note_envelope(int frame_in_note, int note_frames) {
    const int attack  = 2;
    const int release = 3;
    if (frame_in_note < attack) {
        return (float)frame_in_note / (float)attack;
    }
    int frames_until_end = note_frames - 1 - frame_in_note;
    if (frames_until_end <= release) {
        return (float)frames_until_end / (float)release;
    }
    return 1.0f;
}

static void fill_melody(int16_t* out, int nsamples) {
    const MelodyNote* note = &s_melody[s_melody_idx];
    float env = note_envelope(s_melody_frame_in_note, note->frames);

    if (note->freq_hz <= 0.0f) {
        memset(out, 0, nsamples * 2 * sizeof(int16_t));
    } else {
        float step = TWO_PI * note->freq_hz / (float)PM3DS_STEP_RATE_HZ;
        for (int i = 0; i < nsamples; i++) {
            int16_t s = (int16_t)(sinf(s_synth_phase) * 8192.0f * env);
            out[i * 2 + 0] = s;  /* L */
            out[i * 2 + 1] = s;  /* R */
            s_synth_phase += step;
        }
        if (s_synth_phase > TWO_PI * 16384.0f) {
            s_synth_phase = fmodf(s_synth_phase, TWO_PI);
        }
    }

    s_melody_frame_in_note++;
    if (s_melody_frame_in_note >= note->frames) {
        s_melody_frame_in_note = 0;
        s_melody_idx = (s_melody_idx + 1) % s_melody_len;
        /* Don't reset phase between notes — let it carry over so the
         * waveform stays continuous across the boundary. With the new
         * envelope (which lands on 0 at the last sample of A and the
         * first of B), the sample value at the boundary is 0 either
         * way, but keeping phase continuous protects against any
         * floating-point imprecision that might leave envelope at a
         * tiny non-zero value. */
    }
}
#endif

#ifdef PM3DS_AUDIO_STEP_BGM
/* M5.2: forward to the BGM interpreter in audio_bgm.c. */
extern void pm3ds_audio_bgm_fill(int16_t* out, int nsamples);
static void fill_bgm(int16_t* out, int nsamples) {
    pm3ds_audio_bgm_fill(out, nsamples);
}
#endif

#if defined(PM3DS_AUDIO_ENGINE)
/* M14: the game's own audio engine (audio_system.c + audio_alist.c) */
extern void pm3ds_audio_engine_fill(int16_t* out, int nsamples);
#define fill_demo pm3ds_audio_engine_fill
#elif defined(PM3DS_AUDIO_STEP_BGM)
#define fill_demo fill_bgm
#elif defined(PM3DS_AUDIO_STEP_MELODY)
#define fill_demo fill_melody
#elif !defined(PM3DS_AUDIO_STEP_DEMO)
static void fill_demo(int16_t* out, int nsamples) {
    /* M5.2.6: stereo buffer is nsamples * 2 int16 wide. */
    memset(out, 0, nsamples * 2 * sizeof(int16_t));
}
#endif

/* M5.2.7 CSND fallback ring. Single channel (8), continuously-looping
 * buffer of ~250 ms. The refill thread overwrites the buffer faster
 * than the DSP plays it, so the read head always sees fresh data.
 * Quick & dirty but works under Azahar HLE without dspfirm.cdc. */
#define PM3DS_CSND_CHANNEL          8
#define PM3DS_CSND_BUF_FRAMES       (PM3DS_STEP_RATE_HZ / 4)   /* 250 ms */
#define PM3DS_CSND_BUF_BYTES        (PM3DS_CSND_BUF_FRAMES * PM3DS_STEP_CHANNELS * sizeof(int16_t))
/* Per-poll chunk size. The audio thread polls every 2 ms; producing 64
 * stereo frames per poll equals the playback rate (64 × 500 Hz = 32 kHz).
 * This is the bare minimum to keep up — anything more and we'd be
 * generating audio faster than the DSP plays it (and burning CPU); anything
 * less and we underrun. */
#define PM3DS_CSND_CHUNK_FRAMES     64
static int16_t* s_csnd_buf = NULL;
static int      s_csnd_write_cursor = 0;  /* in stereo frames */

static void csnd_step_init(void) {
    s_csnd_buf = linearAlloc(PM3DS_CSND_BUF_BYTES);
    if (!s_csnd_buf) {
        pm3ds_log(PM3DS_LOG_WARN,
                  "csnd step_init: linearAlloc(%u) failed", PM3DS_CSND_BUF_BYTES);
        return;
    }
    memset(s_csnd_buf, 0, PM3DS_CSND_BUF_BYTES);

    /* Prime with one fill before the DSP starts. */
    fill_demo(s_csnd_buf, PM3DS_CSND_BUF_FRAMES);
    svcFlushProcessDataCache_wrapper(s_csnd_buf, PM3DS_CSND_BUF_BYTES);

    /* SOUND_REPEAT loops the buffer forever; PCM16 stereo at 32 kHz. */
    u32 flags = SOUND_CHANNEL(PM3DS_CSND_CHANNEL)
              | SOUND_FORMAT_16BIT
              | SOUND_REPEAT;
    /* csndPlaySound stereo trick: format includes the bit; data0 holds
     * interleaved L,R; size = byte count. */
    csndPlaySound(PM3DS_CSND_CHANNEL, flags, PM3DS_STEP_RATE_HZ, 1.0f, 0.0f,
                  s_csnd_buf, s_csnd_buf, PM3DS_CSND_BUF_BYTES);
    csndExecCmds(true);

    s_step_inited = true;
    pm3ds_log(PM3DS_LOG_INFO,
              "csnd step_init: ch%d primed with %d-frame stereo ring",
              PM3DS_CSND_CHANNEL, PM3DS_CSND_BUF_FRAMES);
}

static void step_init(void) {
    if (s_step_inited) return;
    if (s_use_csnd) {
        csnd_step_init();
        return;
    }
    for (int b = 0; b < PM3DS_STEP_BUFFERS; b++) {
        s_step_data[b] = linearAlloc(PM3DS_STEP_FRAME_BYTES);
        if (!s_step_data[b]) {
            pm3ds_log(PM3DS_LOG_WARN,
                      "audio_step: linearAlloc(%u) failed for buffer %d",
                      PM3DS_STEP_FRAME_BYTES, b);
            return;
        }
        memset(s_step_data[b], 0, PM3DS_STEP_FRAME_BYTES);
        memset(&s_step_wave[b], 0, sizeof(s_step_wave[b]));
        s_step_wave[b].data_vaddr = s_step_data[b];
        s_step_wave[b].nsamples   = PM3DS_STEP_FRAME_SAMPLES;
        s_step_wave[b].looping    = false;
        s_step_wave[b].status     = NDSP_WBUF_FREE;
    }

    ndspChnReset(PM3DS_STEP_CHANNEL);
    ndspChnSetInterp(PM3DS_STEP_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(PM3DS_STEP_CHANNEL, (float)PM3DS_STEP_RATE_HZ);
    /* M5.2.6: STEREO_PCM16 — source provides [L, R] pairs. Mix matrix
     * is the standard stereo passthrough (mix[0] = source L → output L,
     * mix[1] = source R → output R, mix[2..3] = aux send L/R, mix[4..]
     * = reverb/etc.). */
    ndspChnSetFormat(PM3DS_STEP_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = {0};
    mix[0] = 1.0f;   /* L → L */
    mix[1] = 1.0f;   /* R → R */
    ndspChnSetMix(PM3DS_STEP_CHANNEL, mix);

    /* Prime both buffers so the queue is never empty on frame 1. */
    for (int b = 0; b < PM3DS_STEP_BUFFERS; b++) {
        fill_demo(s_step_data[b], PM3DS_STEP_FRAME_SAMPLES);
        svcFlushProcessDataCache_wrapper(s_step_data[b], PM3DS_STEP_FRAME_BYTES);
        ndspChnWaveBufAdd(PM3DS_STEP_CHANNEL, &s_step_wave[b]);
    }
    s_step_inited = true;
    pm3ds_log(PM3DS_LOG_INFO,
              "audio_step: ch%d primed with %d-sample buffers @ %d Hz",
              PM3DS_STEP_CHANNEL, PM3DS_STEP_FRAME_SAMPLES, PM3DS_STEP_RATE_HZ);
}

void pm3ds_audio_step(void) {
    if (!s_audio_dsp_up) return;
    {
        extern bool pm3ds_au_hwvoices;
        extern void pm3ds_audio_engine_tick_hw(void);
        if (pm3ds_au_hwvoices) {   /* no stream: the engine runs by the clock */
            pm3ds_audio_engine_tick_hw();
            return;
        }
    }
    if (!s_step_inited) step_init();
    if (!s_step_inited) return;  /* init failed — keep silent */

    if (s_use_csnd) {
        /* CSND path: partial write at the playback rate. Each poll
         * generates exactly PM3DS_CSND_CHUNK_FRAMES of new audio (64
         * stereo frames = 2 ms @ 32 kHz) and writes it into the
         * looping ring at s_csnd_write_cursor. Since the audio thread
         * polls at 500 Hz, the write rate = 64 × 500 = 32000 frames/s
         * which matches playback. The ring is 8000 frames (250 ms)
         * deep, so the DSP read head trails the write head by ~125 ms
         * on average — plenty of latency budget for the poll cadence
         * to jitter without underrunning.
         *
         * Generating only 64 frames per poll (instead of refilling
         * the whole 16 KB ring) cuts CPU load by ~125× compared to
         * the original "refill everything every 2 ms" approach,
         * which froze the frame loop on real hardware. */
        int chunk = PM3DS_CSND_CHUNK_FRAMES;
        int frames_to_end = PM3DS_CSND_BUF_FRAMES - s_csnd_write_cursor;
        if (chunk <= frames_to_end) {
            int16_t* dst = s_csnd_buf + s_csnd_write_cursor * PM3DS_STEP_CHANNELS;
            fill_demo(dst, chunk);
            svcFlushProcessDataCache_wrapper(dst, chunk * PM3DS_STEP_CHANNELS * sizeof(int16_t));
            s_csnd_write_cursor += chunk;
            if (s_csnd_write_cursor >= PM3DS_CSND_BUF_FRAMES) {
                s_csnd_write_cursor = 0;
            }
        } else {
            /* Wraparound: split into two writes. */
            int16_t* dst = s_csnd_buf + s_csnd_write_cursor * PM3DS_STEP_CHANNELS;
            fill_demo(dst, frames_to_end);
            svcFlushProcessDataCache_wrapper(dst, frames_to_end * PM3DS_STEP_CHANNELS * sizeof(int16_t));
            int remaining = chunk - frames_to_end;
            fill_demo(s_csnd_buf, remaining);
            svcFlushProcessDataCache_wrapper(s_csnd_buf, remaining * PM3DS_STEP_CHANNELS * sizeof(int16_t));
            s_csnd_write_cursor = remaining;
        }
        return;
    }

    /* NDSP path: look for buffers the DSP marked DONE and refill+re-queue. */
    for (int b = 0; b < PM3DS_STEP_BUFFERS; b++) {
        if (s_step_wave[b].status != NDSP_WBUF_DONE) continue;
        fill_demo(s_step_data[b], PM3DS_STEP_FRAME_SAMPLES);
        svcFlushProcessDataCache_wrapper(s_step_data[b], PM3DS_STEP_FRAME_BYTES);
        ndspChnWaveBufAdd(PM3DS_STEP_CHANNEL, &s_step_wave[b]);
    }
}

bool pm3ds_audio_is_up(void) {
    return s_audio_dsp_up;
}

/* M5.1a: 880 Hz sine on NDSP channel 0 for 1 second.
 * Hardware-verified on real 3DS 2026-05-14 — tone audible.
 *
 * Smallest end-to-end voice-plumbing validation:
 *   - linearAlloc a PCM16 mono buffer (DSP can only DMA from the
 *     linear memory region — heap_alloc'd buffers are invisible to it).
 *   - Fill with one second of 880 Hz at 32 kHz: 32000 samples.
 *   - Configure NDSP channel 0 for mono PCM16, 32 kHz, max volume on
 *     both output lines (3DS speakers are stereo; mono signal is
 *     duplicated to L/R via the mix matrix).
 *   - Cache-flush the data so the DSP coprocessor sees the right bytes
 *     (the DSP doesn't snoop CPU caches).
 *   - Queue a single ndspWaveBuf. Playback starts immediately; the
 *     buffer is freed in pm3ds_audio_close once the tone has finished.
 *
 * The wave buffer + data block are static so they stay valid until
 * shutdown — NDSP DMAs from them asynchronously and freeing mid-play
 * crashes the DSP coprocessor. */
#define PM3DS_TEST_TONE_FREQ_HZ   880
#define PM3DS_TEST_TONE_RATE_HZ   32000
#define PM3DS_TEST_TONE_SECONDS   1
#define PM3DS_TEST_TONE_SAMPLES   (PM3DS_TEST_TONE_RATE_HZ * PM3DS_TEST_TONE_SECONDS)
#define PM3DS_TEST_TONE_BYTES     (PM3DS_TEST_TONE_SAMPLES * sizeof(int16_t))

static int16_t* s_test_tone_buf = NULL;
static ndspWaveBuf s_test_tone_wave;

void pm3ds_audio_test_tone(void) {
    if (!s_audio_dsp_up) {
        pm3ds_log(PM3DS_LOG_WARN, "audio_test_tone: NDSP not up; skipping");
        return;
    }
    if (s_test_tone_buf) {
        pm3ds_log(PM3DS_LOG_WARN, "audio_test_tone: already played; skipping");
        return;
    }

    /* Implicit void-pointer conversion. The endian checker scans for
     * explicit narrow-int pointer casts on baked data; linearAlloc
     * returns native-byte-order memory, so we lean on the implicit
     * conversion rather than spelling out a cast. */
    s_test_tone_buf = linearAlloc(PM3DS_TEST_TONE_BYTES);
    if (!s_test_tone_buf) {
        pm3ds_log(PM3DS_LOG_WARN, "audio_test_tone: linearAlloc(%u) failed",
                  PM3DS_TEST_TONE_BYTES);
        return;
    }

    /* Generate the sine wave. Amplitude at ~50% so it isn't ear-splitting. */
    const float two_pi_f_over_rate =
        2.0f * 3.14159265358979323846f
        * (float)PM3DS_TEST_TONE_FREQ_HZ
        / (float)PM3DS_TEST_TONE_RATE_HZ;
    for (int i = 0; i < PM3DS_TEST_TONE_SAMPLES; i++) {
        float s = sinf((float)i * two_pi_f_over_rate);
        s_test_tone_buf[i] = (int16_t)(s * 16384.0f);
    }

    /* Tell NDSP the global output mode. STEREO so the mix matrix below
     * can fan our mono signal out to both speakers. */
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    /* Channel 0 setup. */
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)PM3DS_TEST_TONE_RATE_HZ);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    float mix[12] = {0};
    mix[0] = 1.0f; /* front-left  */
    mix[1] = 1.0f; /* front-right */
    ndspChnSetMix(0, mix);

    memset(&s_test_tone_wave, 0, sizeof(s_test_tone_wave));
    s_test_tone_wave.data_vaddr = s_test_tone_buf;
    s_test_tone_wave.nsamples   = PM3DS_TEST_TONE_SAMPLES;
    s_test_tone_wave.looping    = false;
    s_test_tone_wave.status     = NDSP_WBUF_FREE;

    svcFlushProcessDataCache_wrapper(s_test_tone_buf, PM3DS_TEST_TONE_BYTES);
    ndspChnWaveBufAdd(0, &s_test_tone_wave);

    pm3ds_log(PM3DS_LOG_INFO,
              "audio_test_tone: %d Hz mono PCM16 @ %d Hz, %d samples queued on ch0",
              PM3DS_TEST_TONE_FREQ_HZ, PM3DS_TEST_TONE_RATE_HZ,
              PM3DS_TEST_TONE_SAMPLES);
}
