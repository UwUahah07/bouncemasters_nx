/* ---------------------------------------------------------------------------
 * bm_boot.c -- replaces the work the Java UnityPlayer normally does.
 *
 * On Android, UnityPlayer.java constructs a GLSurfaceView, drives a render
 * thread, and calls a set of file-local natives inside libunity.so. Those
 * natives are not exported, so they cannot be found with dlsym. They ARE
 * announced: libunity's JNI_OnLoad calls RegisterNatives, and jni_fake.c
 * captures the whole table. Every entry point below is resolved from that
 * capture by name.
 *
 * That is the one meaningful difference from cloverpit_nx's equivalent, which
 * resolves by name and falls back to a per-build RVA table when a name is
 * missing. There is no offset table here. If a name does not appear in the
 * capture the boot fails loudly and names it, which is a better failure than
 * calling a hardcoded address that may no longer be that function -- and it is
 * one less thing to re-derive after a game update.
 *
 * UNITY 6 SPECIFICS, inherited from cloverpit_nx (same engine generation,
 * 6000.x, verified against this game's libunity.so):
 *
 *   - initJni takes FOUR arguments: (env, thiz, Context, int). Unity 2020.3 and
 *     2022.3 take three. Calling the 4-arg form with three arguments leaves the
 *     fourth register holding whatever was there, which it then reads.
 *
 *   - nativeUnityPlayerSetRunning(bool) exists and GATES the frame loop. Without
 *     it nativeRender does nothing useful -- it returns, having rendered
 *     nothing, and the screen stays black while the loop happily spins.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "android_native_unity.h"
#include "bm_assets.h"
#include "bm_jni.h"
#include "bm_managed.h"
#include "bm_vsync.h"
#include "config.h"
#include "diag.h"
#include "jni_fake.h"
#include "so_util.h"
#include "util.h"

extern so_module unity_mod;
extern void *fake_unityplayer_thiz;
extern void *fake_context_obj;
extern void *fake_surface_obj;

void port_frame_tick(void);

/* --------------------------------------------------------------------------
 * The UnityPlayer natives
 * ------------------------------------------------------------------------ */

typedef void (*fn_initJni_u6)(void *env, void *thiz, void *ctx, int flag);
typedef void (*fn_v)(void *env, void *thiz);
typedef void (*fn_vz)(void *env, void *thiz, int b);

/* nativeRecreateGfxState(env, thiz, int, Surface) -- FOUR arguments, and the
 * Surface is the one that matters.
 *
 * Confirmed by disassembling this build at libunity+0xa57e8c:
 *
 *     mov x19, x3      ; the Surface object
 *     mov w22, w2      ; an int flag
 *     mov x21, x0      ; env
 *     ...
 *     mov w0, w22 ; mov x1, x19 ; mov x2, x21
 *     bl  AttachSurface(int, _jobject*, _JNIEnv*)      <- libunity+0xa54908
 *
 * AttachSurface is what installs the primary window. It is reachable ONLY from
 * here, so calling this function without the Surface leaves the window unset --
 * and AndroidGraphics::WaitForPrimaryWindow then blocks forever inside the
 * first nativeRender. That was this port's black screen: the engine had fully
 * initialised, the vsync pump was ticking at a clean 60 Hz, and the main thread
 * sat in cond_wait waiting for a window nothing had given it.
 *
 * The names mislead. "SendSurfaceChangedEvent" sounds like the one that carries
 * the surface; it does not. libunity+0xa57ef4 reads NOTHING from x2..x5 -- it
 * just fetches GlobalCallbacks::Get() and runs CallbackArray::Invoke(). It is a
 * notification, and it belongs AFTER the attach.
 *
 * Cross-checked against clayjam_nx, an independent Unity 6 port, which calls
 * the same pair in the same shapes and the same order. */
typedef void (*fn_recreate)(void *env, void *thiz, int flag, void *surface);

/* nativeRender returns a liveness flag (the tail at +0xa581b8 is `mov w8, w0`
 * off UnityPlayerLoop). Non-zero means the player is still running. */
typedef unsigned char (*fn_render)(void *env, void *thiz);

static struct {
  fn_initJni_u6 initJni;
  fn_recreate   recreateGfxState;
  fn_v          sendSurfaceChanged;
  fn_vz         setRunning;
  fn_render     render;
  fn_v          resume;
  fn_v          pause;
  fn_v          done;
  fn_vz         focusChanged;
  void (*orientationChanged)(void *env, void *thiz, int orient, int flag);
} U;

static int g_running;

/* Resolve one native from the RegisterNatives capture.
 * `required` entries abort the boot; optional ones just log. */
static void *resolve_native(const char *name, int required) {
  void *p = jni_lookup_unity_native(name);
  if (!p) {
    if (required)
      debugPrintf("[boot] MISSING required native: %s\n", name);
    else
      debugPrintf("[boot] optional native absent: %s\n", name);
    return NULL;
  }
  debugPrintf("[boot]   %-34s -> %p (+0x%lx)\n", name, p,
              (unsigned long)((uintptr_t)p - (uintptr_t)unity_mod.load_virtbase));
  return p;
}

#define RESOLVE(field, type, nm, req) U.field = (type)resolve_native(#nm, req)

static int resolve_all(void) {
  debugPrintf("[boot] resolving UnityPlayer natives from the RegisterNatives capture\n");

  RESOLVE(initJni,            fn_initJni_u6, initJni,                     1);
  RESOLVE(recreateGfxState,   fn_recreate,   nativeRecreateGfxState,      1);
  RESOLVE(sendSurfaceChanged, fn_v,          nativeSendSurfaceChangedEvent, 1);
  RESOLVE(setRunning,         fn_vz,         nativeUnityPlayerSetRunning, 1);
  RESOLVE(render,             fn_render,     nativeRender,                1);
  RESOLVE(resume,             fn_v,          nativeResume,                0);
  RESOLVE(pause,              fn_v,          nativePause,                 0);
  RESOLVE(done,               fn_v,          nativeDone,                  0);
  RESOLVE(focusChanged,       fn_vz,         nativeFocusChanged,          0);
  U.orientationChanged = (void (*)(void *, void *, int, int))
      resolve_native("nativeOrientationChanged", 0);

  if (!U.initJni || !U.recreateGfxState || !U.sendSurfaceChanged ||
      !U.setRunning || !U.render) {
    debugPrintf("[boot] FATAL: a required UnityPlayer native did not register.\n"
                "       libunity's JNI_OnLoad ran but did not announce it, which\n"
                "       usually means the RegisterNatives class filter in\n"
                "       jni_fake.c did not match this build's class name.\n"
                "       Check the '[jni] RegisterNatives ...' lines above for the\n"
                "       class names it actually saw.\n");
    return -1;
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

/* Suspend and resume, driven by the applet hook in main.c. Both are safe to
 * call before the engine is running; they no-op until then. */
void bm_boot_pause(void) {
  if (!g_running) return;
  if (U.pause) U.pause(fake_env, fake_unityplayer_thiz);
  U.setRunning(fake_env, fake_unityplayer_thiz, 0);
  debugPrintf("[boot] paused\n");
}

void bm_boot_resume(void) {
  if (!g_running) return;
  U.setRunning(fake_env, fake_unityplayer_thiz, 1);
  if (U.resume) U.resume(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] resumed\n");
}

void bm_boot_focus(int focused) {
  if (g_running && U.focusChanged)
    U.focusChanged(fake_env, fake_unityplayer_thiz, focused ? 1 : 0);
}

/* --------------------------------------------------------------------------
 * bm_boot_and_run -- start the engine and drive frames until it quits.
 * ------------------------------------------------------------------------ */

int bm_boot_and_run(void) {
  if (resolve_all() != 0) return -1;

  /* initJni faults on a null Context. These three are `= 0` in unity_glue.c and
   * are filled by unity_environment_init(); if that was skipped or failed, say
   * so here rather than taking the fault inside the engine. */
  if (!fake_unityplayer_thiz || !fake_context_obj) {
    debugPrintf("[boot] FATAL: fake UnityPlayer/Context objects are NULL --\n"
                "       unity_environment_init() did not run or did not complete\n");
    return -2;
  }

  /* Re-assert the main thread's Bionic TLS immediately before entering engine
   * code. jni_init, HID setup and the applet hook all ran since main.c installed
   * it, and any of them can leave tpidr pointing somewhere else. The engine
   * reads its stack guard through tpidr on the first call. */
  extern void bm_reassert_main_tls(void);
  bm_reassert_main_tls();

  debugPrintf("[boot] initJni(env, thiz, ctx, 0)  [Unity 6: four args]\n");
  U.initJni(fake_env, fake_unityplayer_thiz, fake_context_obj, 0);
  debugPrintf("[boot] initJni returned\n");

  /* Start the vsync pump BEFORE the graphics calls, not merely before the loop.
   * WaitVSync blocks until the presentation counter reaches its target and
   * nothing else raises it, and the engine can reach that wait from inside
   * recreateGfxState. clayjam_nx starts it at exactly this point for the same
   * reason. */
  if (bm_vsync_start() != 0)
    debugPrintf("[boot] WARNING: no vsync pump -- the first nativeRender will\n"
                "        park in WaitVSync and never return\n");

  /* ATTACH THE SURFACE FIRST. recreateGfxState carries it; sendSurfaceChanged
   * is only the notification that follows. Reversing these, or calling
   * recreateGfxState without the Surface, leaves the primary window unset and
   * the first nativeRender never returns. See the typedefs above.
   *
   * android_native_update_mode() has already sized the NWindow from
   * screen_width/height, so the engine picks the geometry up from the window
   * itself -- there is no width/height to pass here. */
  extern int screen_width, screen_height;
  debugPrintf("[boot] recreateGfxState(flag=0, surface=%p)  [attaches the window]\n",
              fake_surface_obj);
  U.recreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);

  debugPrintf("[boot] sendSurfaceChanged()  [notification; window is %dx%d]\n",
              screen_width, screen_height);
  U.sendSurfaceChanged(fake_env, fake_unityplayer_thiz);

  /* Tell the engine the orientation explicitly. 2 == ORIENTATION_LANDSCAPE
   * (1 is portrait). The APK's manifest declares screenOrientation=6
   * (SENSOR_LANDSCAPE), and without this the engine keeps its Android default
   * and can lay the UI out for the wrong aspect. Optional -- older builds do
   * not register it. */
  if (U.orientationChanged) {
    debugPrintf("[boot] orientationChanged(LANDSCAPE)\n");
    U.orientationChanged(fake_env, fake_unityplayer_thiz, 2, 0);
  }

  /* The gate. Without this nativeRender returns having done nothing. */
  U.setRunning(fake_env, fake_unityplayer_thiz, 1);
  g_running = 1;
  if (U.resume) U.resume(fake_env, fake_unityplayer_thiz);

  /* Focus. Unity gates some startup work on having it, and nothing else will
   * deliver it until the applet hook fires on a real focus change. */
  if (U.focusChanged) U.focusChanged(fake_env, fake_unityplayer_thiz, 1);

  debugPrintf("[boot] entering frame loop\n");

  uint64_t frames = 0;
  uint32_t g_frame_count_local = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    /* PUMP INPUT FIRST, before the engine reads it.
     *
     * android_native_feed_hid() is the whole input path in one call: padUpdate,
     * then nxp_update() to read the touch panel / sticks / gyro / USB mouse,
     * then nxp_poll() to drain the resulting events into bm_touch's slot
     * tracker -- which is what bm_input.c's UnityEngine.Input hooks report from.
     *
     * NOTHING CALLED IT. The function was written, declared in
     * android_native_unity.h, and referenced only from comments in bm_input.c.
     * So the panel was never read, the tracker never had a finger in it,
     * Input.touchCount always answered 0, and the stick cursor could not be
     * summoned either -- every symptom of "input does nothing" at once, from one
     * missing call.
     *
     * It must run BEFORE nativeRender: the engine polls Input during its
     * PlayerLoop, so feeding afterwards would always be one frame stale and, on
     * a tap shorter than two frames, would miss the press entirely. */
    android_native_feed_hid();

    /* The engine renders and presents. imports.c's eglSwapBuffers wrapper is
     * what actually puts the frame on screen. The return value is a liveness
     * flag; a zero means the player has shut itself down and the loop should
     * not keep calling it. */
    unsigned char alive = U.render(fake_env, fake_unityplayer_thiz);
    if (!alive && frames > 0) {
      debugPrintf("[boot] nativeRender reported not-alive at frame %llu\n",
                  (unsigned long long)frames);
      break;
    }

    /* Android's vibrate(ms) is fire-and-forget; the Switch motor is
     * level-triggered and runs until told otherwise. This is what stops it. */
    bm_jni_rumble_tick();

    /* Deliver any Firebase Future completions queued during the previous
     * frame. Deferred rather than fired inline so the C# dispatch table is
     * certainly populated -- see firebase_stub.c. A queued completion that
     * never fires is the same hang as no completion at all, so this call is
     * not optional. */
    { extern void firebase_stub_pump(void); firebase_stub_pump(); }

    /* Samples deltaTime for the managed Time hooks. Must be once per RENDERED
     * frame -- the vsync pump ticks on its own thread at a fixed period and is
     * a different clock for a different purpose. */
    bm_time_tick();

    /* Audio counters, once a second, from the MAIN thread.
     *
     * opensles.c deliberately keeps these as plain counters because the audio
     * callback and FMOD's mixer thread must not do file I/O -- logging from
     * them is what stopped this console booting two builds ago. bm_audio_stats()
     * exists to format them for a thread that CAN log, and, like
     * android_native_feed_hid(), nothing ever called it.
     *
     * The counters separate the remaining causes without another guess:
     *   cb == 0                -> the SDL device never calls back at all
     *   enq == 0               -> FMOD is producing nothing to play
     *   consumed >> enqueued   -> blocks are being replayed
     *   dry / short high       -> underrun, i.e. audible gaps or silence */
    { extern void bm_audio_stats(char *out, size_t cap);
      static uint32_t last_stat;
      if (g_frame_count_local - last_stat >= 60) {
        last_stat = g_frame_count_local;
        char st[128];
        bm_audio_stats(st, sizeof st);
        debugPrintf("[audio] %s\n", st);
      } }

    bm_prefs_tick();     /* coalesced PlayerPrefs flush; see bm_prefs.c */

    port_frame_tick();
    diag_frame((int)frames);

    /* Log the first few frames individually, then once a second.
     *
     * The old version logged only frames 1 and 60, which could not tell
     * "stopped at frame 1" from "stopped at frame 5" -- and every stall so far
     * has died in the first handful of frames, inside managed startup. Those
     * early numbers are the cheapest signal there is for whether a fix moved
     * the boot forward or not, and they cost four log lines. */
    frames++; g_frame_count_local = (uint32_t)frames;
    if (frames <= 5 || (frames % 60) == 0)
      debugPrintf("[boot] frame %llu rendered (alive=%d)\n",
                  (unsigned long long)frames, (int)alive);
  }

  debugPrintf("[boot] leaving frame loop after %llu frames (quit=%d)\n",
              (unsigned long long)frames, jni_quit_requested);

  /* Flush save data BEFORE tearing the engine down.
   *
   * bm_prefs_tick() only commits once a second, so up to a second of progress
   * is sitting in memory at this point. Losing the last coin on every exit is
   * exactly the complaint this whole change set is fixing, so do it here --
   * before bm_vsync_stop() and the Unity teardown, because those can block and
   * anything after them is not guaranteed to run. */
  bm_prefs_flush_now();

  bm_vsync_stop();
  g_running = 0;
  U.setRunning(fake_env, fake_unityplayer_thiz, 0);
  if (U.pause) U.pause(fake_env, fake_unityplayer_thiz);
  if (U.done)  U.done(fake_env, fake_unityplayer_thiz);

  return 0;
}
