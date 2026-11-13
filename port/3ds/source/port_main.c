/*
 * main.c — 3DS entry point.
 *
 * Boot order:
 *   1. 3DS platform: gfx, console, romfs, citro3d, log, heap, panic, clock.
 *   2. N64 SDK shim: install routing tables, spawn VI retrace ticker,
 *      map baserom into the emulated RDRAM region.
 *   3. Game entry: jump into the decomp's entry_point() (defined by
 *      src/boot/entry_point.s — but on ARM that asm is excluded, so the
 *      effective entry is whatever C function the decomp ld script would
 *      have called next). We resolve this at link time via a symbol
 *      defined in the decomp source (`main_func` in src/main.c, if it
 *      exists). If absent at link time, fall back to logging and idle.
 *   4. Frame loop: pump messages, present, repeat.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <3ds.h>
#include "pm3ds_settings.h"
extern const void *pm3ds_gfx_drain_dl_list(const void **out, int max, int *count);
#include <citro3d.h>

#include "platform.h"
#include "pm3ds_heap.h"
#include "pm3ds_brand.h"
#include "pm3ds_log.h"
#include "pm3ds_panic.h"
#include "pm3ds_clock.h"
#include "pm3ds_audio.h"

/* Implemented in n64_shim/. */
extern void  pm3ds_gfx_present_init(void);
extern void  pm3ds_gfx_present_frame(void);
extern void  pm3ds_gfx_present_shutdown(void);
extern void *pm3ds_vi_consume_pending(void);
extern void  pm3ds_event_post(int event);
extern void  osContStartReadData(void *mq);

#define OS_EVENT_VI 7

/* M6.1: src/main.c (the decomp's) now compiles into the build, providing
 * `boot_main` as a real (non-weak) symbol. The port-local main.c file was
 * renamed to port_main.c so its basename no longer collides with src/main.c.
 *
 * boot_main does N64-specific init (osViSetMode, nuGfxInit, audio system
 * setup) that runs entirely through our libultra/nusys shims. Many called
 * functions are still weak stubs in m42_rom_stubs.c — those return zero,
 * which lets boot_main reach gfxRetrace_Callback's step_game_loop path
 * without immediately crashing.
 *
 * Gated behind PM3DS_BOOT_GAME so we can ship a build that just runs the
 * 3DS smoke loop (M5 audio still works) or a build that hands off to the
 * decomp's boot_main. Set with EXTRA_CFLAGS=-DPM3DS_BOOT_GAME=1. */
extern void boot_main(void *arg);

static volatile int g_retrace_thread_run = 1;
static Thread g_retrace_thread;

static void retrace_thread(void *arg) {
    (void)arg;
    while (g_retrace_thread_run) {
        gspWaitForVBlank();
        pm3ds_event_post(OS_EVENT_VI);
    }
}

extern volatile const char *pm3ds_bc_main;

static void shim_libultra_init(void) {
    /* The shim layer is stateless beyond the per-subsystem statics. */
    /* Spawn the VI retrace ticker, pinned to the system core. */
    g_retrace_thread = threadCreate(retrace_thread, NULL, 0x800, 0x18, -1, false);
}

/* false: the game can't start (no usable ROM) and the player chose to exit */
static bool platform_init(void) {
    /* New 3DS: 804 MHz + L2 cache (no-op on Old 3DS). The CIA exheader asks
     * for it too, but the 3dsx path only gets it from this call. */
#ifdef PM3DS_SIM_O3DS
    osSetSpeedupEnable(false);   /* test build: Old 3DS CPU speed on a New 3DS */
#else
    osSetSpeedupEnable(true);
#endif
    gfxInitDefault();
    gfxSet3D(false);
    /* the bottom screen is drawn with citro3d (platform/bottom_screen.c); the
     * text console is only brought up for fatal errors */
    romfsInit();

    /* GPU command buffer for one frame. The default 256 KB overflowed in busy
     * scenes with the 3D slider up (every list is drawn once per eye), and
     * citro3d answers an overflow with svcBreak. gfx_present.c also stops
     * drawing before it fills up (CMDBUF_STOP). */
    C3D_Init(PM3DS_CMDBUF_BYTES);
    { extern void pm3ds_c3d_fast_init(size_t); pm3ds_c3d_fast_init(PM3DS_CMDBUF_BYTES); }
    C3D_FrameRate(60.0f);   /* count every vblank: gfx_present.c paces the game steps */

    pm3ds_log_init();
    { extern void pm3ds_sd_migrate(void); pm3ds_sd_migrate(); }     /* before any SD write */
    { extern void pm3ds_netlog_init(void); pm3ds_netlog_init(); }   /* dev builds: log to the PC */
    pm3ds_clock_init();
    pm3ds_panic_install();

    /* (no pm3ds_heap arena any more: the game allocates from its own heaps
     * (src/heaps2.c, heaps3.c), and those 8 MB of linear memory are what the
     * texture cache lives on in the Old 3DS's 80 MB mode) */

    /* M5.0: bring the NDSP audio service up. Non-fatal on failure —
     * audio is silenced and the rest of the platform continues. */
    pm3ds_bc_main = "audio init";
    pm3ds_audio_init();

#ifdef PM3DS_AUDIO_TEST_TONE
    /* M5.1a: queue an 880 Hz / 1 s sine tone so we can verify on real
     * hardware that the voice plumbing is wired correctly. Skipped on
     * Azahar (NDSP init fails there), so the screen log is the only
     * channel to confirm the queue went out. */
    pm3ds_audio_test_tone();
#endif

    /* the renderer first: the bottom screen shows the ROM load's progress in
     * the console font (its menu art is game data, rebuilt after the load) */
    pm3ds_bc_main = "renderer init";
    pm3ds_gfx_present_init();
    { extern void pm3ds_bottom_screen_init(void); pm3ds_bottom_screen_init(); }

    /* the player's ROM from the SD card (platform/pm3ds_rom.c); release
     * builds also get their game data back from it here */
    pm3ds_bc_main = "rom load";
    { extern bool pm3ds_rom_init(void); if (!pm3ds_rom_init()) return false; }

    /* only now: the update check's sockets and TLS buffers, taken while the ROM
     * loaded, left no block for the 10 MB audio bank, which then played from the
     * SD card with the bass off (GitHub #70, half the starts in a player's log) */
    { extern void pm3ds_update_check_start(void); pm3ds_update_check_start(); }

    { extern void pm3ds_convert_n64_mtx_arrays(void); pm3ds_convert_n64_mtx_arrays(); }
    return true;
}

int main(void) {
#ifdef PM3DS_RO_CHECK
    { extern void pm3ds_ro_check_init(void); pm3ds_ro_check_init(); }
#endif
    if (!platform_init()) {
        { extern void pm3ds_audio_close(void); pm3ds_audio_close(); }
        pm3ds_gfx_present_shutdown();
        C3D_Fini();
        gfxExit();
        return 0;
    }
    pm3ds_bc_main = "shim init";
    shim_libultra_init();

    pm3ds_log(PM3DS_LOG_INFO, "%s v%s; platform up; entering decomp main", PM3DS_APP_NAME, PM3DS_VERSION_STR);

#ifdef PM3DS_DEBUG_DL
    /* Submit the canned DL once per frame so the translator has work
     * to do every iteration. The DebugOSTask layout MUST match the
     * OSTask struct in source/n64_shim/gfx_dl.c byte-for-byte so the
     * drain path reads data_ptr from the right offset. With ARM
     * 8-byte u64 alignment, data_ptr sits at offset 88, not 48 —
     * an earlier version of this struct underestimated the padding
     * and produced a NULL-or-garbage DL pointer that walk_dl exited
     * on immediately, hence visible blue + gfx_tasks ticking.
     *
     * If you change gfx_dl.c's OSTask, change this here too. */
    extern const struct { uint32_t w0, w1; } pm3ds_debug_dl[];
    typedef struct DebugOSTask {
        uint32_t type;            uint32_t flags;
        uint64_t ucode_boot;      uint32_t ucode_boot_size;
        uint64_t ucode;           uint32_t ucode_size;
        uint64_t ucode_data;      uint32_t ucode_data_size;
        uint64_t dram_stack;      uint32_t dram_stack_size;
        uint64_t output_buff;     uint64_t output_buff_size;
        uint64_t data_ptr;        uint32_t data_size;
        uint32_t yield_data_ptr;  uint32_t yield_data_size;
    } DebugOSTask;
    extern void osSpTaskStart(void *task);
    static DebugOSTask debug_task;
    memset(&debug_task, 0, sizeof(debug_task));
    debug_task.data_ptr = (uintptr_t)pm3ds_debug_dl;
    pm3ds_log(PM3DS_LOG_INFO, "PM3DS_DEBUG_DL active (submitting every frame)");
#endif

#ifdef PM3DS_BOOT_GAME
    /* boot_main starts by checking osTvType: the PAL build PANIC()s unless it
     * is OS_TV_PAL, the US build unless it is NTSC or MPAL. PANIC compiles to
     * an infinite `b .` loop on this port (is_debug.c is excluded).
     * osTvType is a real BSS slot from src/os/ that the decomp's
     * src/os/initialize.c::__osInitialize_common would set; we don't compile
     * it, so set it to the version's TV type before the thread spawns. */
    extern int osTvType;
#if VERSION_PAL
    osTvType = 1; /* OS_TV_PAL */
#else
    osTvType = 0; /* OS_TV_NTSC */
#endif

    pm3ds_log(PM3DS_LOG_INFO, "osTvType := %d; spawning boot_main thread", osTvType);
    /* M7: boot_main priority 0x3F (lowest). Its terminal while(true){}
     * does nothing useful — letting it run at 0x20 starved the main
     * thread (frame counter stuck at 2 in M6 verification). At 0x3F it
     * only gets scheduled when nothing else needs the CPU. */
    Thread game = threadCreate(boot_main, NULL, 0x8000, 0x3F, -1, false);
    if (!game) {
        pm3ds_log(PM3DS_LOG_WARN, "boot_main threadCreate failed");
    }
    (void)game;
#else
    pm3ds_log(PM3DS_LOG_INFO, "PM3DS_BOOT_GAME unset -- shim-only smoke loop");
#endif

    /* M7: pull in the global retrace callback pointer that nuGfxFuncSet
     * stores. boot_main registers gfxRetrace_Callback via that shim;
     * the frame loop below invokes it every frame to drive the decomp's
     * step_game_loop / gfx_draw_frame chain. */
    extern void (*pm3ds_gfx_retrace_cb)(int);
    int retrace_task_num = 0;
    int retrace_cb_logged = 0;

    /* M8.1: load_engine_data finishes with
     *   gOverrideFlags |= GLOBAL_OVERRIDES_DISABLE_DRAW_FRAME;
     *   set_game_mode(GAME_MODE_STARTUP);
     * The startup state machine is supposed to clear that flag once it
     * advances, but with weak-stubbed update_* functions the state
     * machine never progresses. Force-clear the flag after a delay so
     * gfx_draw_frame actually emits a non-empty DL.
     *
     * GLOBAL_OVERRIDES_DISABLE_DRAW_FRAME = 0x00000008 (bit 3) per
     * include/enums.h:4346. First attempt used 0x80000000 (bit 31) —
     * cleared a flag that wasn't set, leaving bit 3 still on.
     */
    extern int gOverrideFlags;
    int clear_disable_draw_done = 0;
    int clear_disable_draw_frame = 0;
#define PM3DS_GLOBAL_OVERRIDES_DISABLE_DRAW_FRAME 0x00000008

    while (aptMainLoop()) {
        pm3ds_bc_main = "input";
        hidScanInput();
        osContStartReadData(NULL);
        (void)pm3ds_vi_consume_pending();
#ifdef PM3DS_DEBUG_DL
        osSpTaskStart(&debug_task);
#endif
        /* M5.1d v2: audio_step runs on a dedicated thread (spawned in
         * pm3ds_audio_init at priority 0x18). Re-added here as a
         * fallback in case the thread spawn fails — refill is
         * idempotent (each DONE buffer is refilled exactly once via
         * its status field). Cheap insurance against any future
         * threadCreate regression. */
        /* only without the thread: on New 3DS it runs on another core, and
         * two mixers at once would corrupt the FIFO */
        { extern bool pm3ds_audio_thread_running(void);
          if (!pm3ds_audio_thread_running()) pm3ds_audio_step(); }

#ifdef PM3DS_INVOKE_RETRACE_CB
        /* M7: invoke the game's registered retrace callback. This
         * calls into step_game_loop / gfx_draw_frame — the actual
         * game tick. Will surface lots of stubbed-code crashes; that's
         * intentional, each one narrows the gap to playable frames.
         *
         * Opt-in via -DPM3DS_INVOKE_RETRACE_CB=1 because the first
         * call here is almost certain to crash on something deep in
         * step_game_loop. Keep the M6 build (no flag) clean for
         * fallback testing. */
        if (pm3ds_gfx_retrace_cb) {
            if (!retrace_cb_logged) {
                pm3ds_log(PM3DS_LOG_INFO,
                          "M7: invoking gfxRetrace_Callback (first time)");
                retrace_cb_logged = 1;
            }
            /* M8.3 breadcrumb: log every cb fire for the first 20
             * frames so we can pin down which one hangs (Frame
             * counter on bottom screen reportedly stuck at 4). */
            extern uint32_t pm3ds_gfx_frame_number(void);
            uint32_t _f = pm3ds_gfx_frame_number();
            /* M9: also log around the title-screen frames. The title is
             * reached at ~frame 188, long past the _f<20 window, and the
             * per-frame hang lands here — so re-open the breadcrumb gate
             * once pm3ds_title_dbg is armed (by decode_yay0), capped. */
            int _tdbg = 0;
#ifdef PM3DS_TITLE_DBG
            { extern int pm3ds_title_dbg; static int _tcap = 0;
              if (pm3ds_title_dbg && _tcap < 12) { _tcap++; _tdbg = 1; } }
#endif
            if (_f < 20 || _tdbg) {
                pm3ds_log(PM3DS_LOG_INFO,
                          "cb-fire frame=%lu slot=%d (enter)",
                          (unsigned long)_f, retrace_task_num);
            }
            { extern uint64_t pm3ds_t_game;
              uint64_t _t0 = svcGetSystemTick();
              pm3ds_bc_main = "game step";
              /* fast-forward: several game steps per pass, only the last
               * one's display lists are drawn */
              pm3ds_ff_poll();
#ifdef PM3DS_STEP_PROF
              { extern void pm3ds_prof_step_begin(void); pm3ds_prof_step_begin(); }
#endif
              extern int pm3ds_gfx_extra_steps(void);
              int extra = pm3ds_gfx_extra_steps();   /* Old 3DS: frames behind, made up without drawing them */
              int steps = pm3ds_fast_forward() ? pm3ds_cfg.ff_speed : 1 + extra;
              { extern bool pm3ds_steps_must_draw(void);
                if (pm3ds_steps_must_draw()) steps = 1; }   /* the credits (hud_info.c) */
              for (int s = 0; s < steps; s++) {
                  if (s > 0) {   /* drop the lists of the step before (the queue is bounded) */
                      const void *skipped[16];
                      int nskipped;
                      pm3ds_gfx_drain_dl_list(skipped, 16, &nskipped);
                  }
                  { extern void gfx_interp_new_tick(void); gfx_interp_new_tick(); }
                  /* The callback steps the game on every other call (N64: 60 Hz
                   * retrace, 30 Hz game). Make both calls here so each loop pass
                   * is one game step and one rendered frame, paced at 30 Hz by
                   * C3D_FrameRate: re-rendering the unchanged lists on the off
                   * call doubled the translation cost for nothing. */
                  /* a step whose lists would be dropped (fast-forward, Old 3DS
                   * catch-up) skips building them: the callback leaves out
                   * gfx_draw_frame at a task count of 3, as the N64 did when
                   * its RDP fell behind (~2.7 ms a step on an Old 3DS) */
                  int tasks = s < steps - 1 ? 3 : retrace_task_num;
                  pm3ds_gfx_retrace_cb(tasks);
                  pm3ds_gfx_retrace_cb(tasks);
              }
#ifdef PM3DS_STEP_PROF
              { extern void pm3ds_prof_step_end(int); extern s32 get_game_mode(void); pm3ds_prof_step_end(get_game_mode()); }
#endif
              { extern uint64_t pm3ds_t_game_step;
                pm3ds_t_game_step = svcGetSystemTick() - _t0; }
              pm3ds_t_game += svcGetSystemTick() - _t0; }
            if (_f < 20 || _tdbg) {
                pm3ds_log(PM3DS_LOG_INFO,
                          "cb-fire frame=%lu slot=%d (returned)",
                          (unsigned long)_f, retrace_task_num);
            }
            /* The argument is nusys' count of queued gfx tasks, and the
             * callback skips gfx_draw_frame when it is >= 3. Tasks run
             * synchronously here, so it stays 0. (Cycling it 0..3 made every
             * other game step land on 3: half the frames were never drawn,
             * and frameCounter parity locked so HUD HP/FP never animated.) */
            retrace_task_num = 0;

#ifndef PM3DS_M8_NO_CLEAR_DRAW_FLAG
            /* M8.1: after ~120 frames (~2 seconds) of the retrace
             * callback firing, clear DISABLE_DRAW_FRAME so
             * gfx_draw_frame stops early-returning.
             *
             * Disable with -DPM3DS_M8_NO_CLEAR_DRAW_FLAG=1 to keep
             * gfx_draw_frame early-returning forever (probe 1:
             * isolates the post-clear draw chain from the rest of
             * the game tick). */
            clear_disable_draw_frame++;
            if (!clear_disable_draw_done && clear_disable_draw_frame > 120) {
                pm3ds_log(PM3DS_LOG_INFO,
                          "M8.1: gOverrideFlags before=0x%08x",
                          (unsigned)gOverrideFlags);
                gOverrideFlags &= ~PM3DS_GLOBAL_OVERRIDES_DISABLE_DRAW_FRAME;
                pm3ds_log(PM3DS_LOG_INFO,
                          "M8.1: gOverrideFlags after=0x%08x (cleared bit 0x8)",
                          (unsigned)gOverrideFlags);
                clear_disable_draw_done = 1;
            }
#else
            (void)clear_disable_draw_frame;
            (void)clear_disable_draw_done;
#endif
        }
#endif

        { extern uint64_t pm3ds_t_present;
          uint64_t _t0 = svcGetSystemTick();
          pm3ds_bc_main = "present";
          pm3ds_gfx_present_frame();
          pm3ds_t_present += svcGetSystemTick() - _t0; }
    }

    /* Stop our threads before libctru's exit frees the heap their stacks
     * live in: the audio mixer runs on its own core (New 3DS) and kept
     * running into the teardown (data abort on core 2 when closing). */
    g_retrace_thread_run = 0;
    if (g_retrace_thread) threadJoin(g_retrace_thread, 100 * 1000 * 1000ULL);
    { extern void pm3ds_audio_close(void); pm3ds_audio_close(); }
    { extern void pm3ds_bottom_screen_lcd_restore(void); pm3ds_bottom_screen_lcd_restore(); }
    pm3ds_gfx_present_shutdown();
    C3D_Fini();
    gfxExit();
    return 0;
}
