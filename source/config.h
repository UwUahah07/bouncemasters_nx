/* ---------------------------------------------------------------------------
 * config.h -- build-time switches for the Bouncemasters Switch loader.
 * ------------------------------------------------------------------------- */

#ifndef BM_CONFIG_H
#define BM_CONFIG_H

/* OPENBONK BRING-UP ("build 0").
 *
 * 1 = skip every patch and hook whose address was derived from the
 *     Bouncemasters libunity.so / libil2cpp.so (granularity, audio, vsync
 *     block, GC bridge, managed Time/Input/Screen/Prefs hooks). Those RVAs mean
 *     nothing in OpenBonk's builds (Unity 2023.2.22f1) and some of them lie past
 *     the end of its libunity mapping, so touching them would fault. The goal of
 *     build 0 is only to see in debug.log how far the engine gets by itself.
 * Re-derive the offsets one by one and switch the pieces back on. */
#define OB_BRINGUP 0

#include "bm_offsets.h"

/* ---------------------------------------------------------------------------
 * Diagnostics
 *
 * DEBUG_LOG 1 writes debug.log next to the .nro and turns on every diagnostic
 * in the tree: the loader's module map, the JNI approximation ledger, the SDK
 * stub lookup log, and the crash dumper. It costs frame rate. Leave it on for
 * the entire bring-up -- the boot log is the only thing that will tell you
 * which of the 300-odd stubbed SDK entry points the game actually called, and
 * in what order, and guessing at that from source is a waste of a week.
 * ------------------------------------------------------------------------ */
#define DEBUG_LOG 0

/* Log every JNI call the engine makes, including the ones that resolve fine.
 * Extremely verbose. Turn on only when chasing a specific hang. */
#define DEBUG_JNI_TRACE 0

/* ---------------------------------------------------------------------------
 * Game files
 *
 * The loader locates its own folder at runtime, so the directory name is the
 * user's choice -- these are the names expected inside it. Staged by
 * tools/stage_sd.py from the user's own APK.
 * ------------------------------------------------------------------------ */
#define BM_LIB_MAIN     "libmain.so"
#define BM_LIB_UNITY    "libunity.so"
#define BM_LIB_IL2CPP   "libil2cpp.so"
#define BM_ASSET_DIR    "assets"

/* Fallback paths, used only until bm_resolve_game_root() succeeds.
 *
 * These are NOT the folder name the port requires. bm_root.c walks argv[0] and
 * the candidate directories looking for libmain.so and adopts whichever
 * actually holds the game, so the user can name the folder anything. These
 * values are what g_root and g_log hold before that runs -- which matters,
 * because the very first log write happens before resolution completes and has
 * to go somewhere. Keep them consistent with each other; LOG_NAME must be
 * GAME_HOME + "/debug.log" or early lines land outside the folder the rest of
 * the run uses.
 */
#define GAME_HOME   "sdmc:/switch/openbonk_nx"
#define LOG_NAME    "sdmc:/switch/openbonk_nx/debug.log"

/* ---------------------------------------------------------------------------
 * Application identity
 *
 * What the JNI Context shim in unity_jni.c reports for getPackageName() and the
 * PackageInfo version fields. Read directly from the APK's binary
 * AndroidManifest.xml, not guessed:
 *
 *     package      com.joybits.bouncemasters
 *     versionCode  42
 *     versionName  1.0.0
 *     minSdk 23, targetSdk 35, compileSdk 35
 *
 * The publisher is JoyBits. Worth stating plainly because Bouncemasters is
 * widely attributed elsewhere to Playgendary; the package name and the only
 * publisher strings in the APK both say joybits.
 *
 * These are not cosmetic. Analytics and SDK init paths compare the package name
 * against their own configuration, and a mismatch tends to surface as an SDK
 * refusing to initialise rather than as anything that names the package.
 * ------------------------------------------------------------------------ */
#define CS_PACKAGE       "com.megabonk.nativeport.splittest"
#define CS_VERSION_NAME  "0.1.0"
#define CS_VERSION_CODE  32

/* assets/bin/Data/unity_app_guid. Unity derives its per-app storage path from
 * this, so it must match the staged data or PlayerPrefs lands somewhere the
 * next run will not look. */
#define CS_APP_GUID      "ac4e28a3-fc4a-4508-928e-904096c76ac0"

/* ---------------------------------------------------------------------------
 * Third-party SDK behaviour -- see bm_sdk_stubs.c
 *
 * BM_EOS_INIT_RESULT decides what EOS_Initialize reports to the managed
 * EOSManager. EOS_NotConfigured (14) is the default: a hard, honest failure at
 * the first call, before any callback is registered, so the managed side takes
 * its own no-backend branch immediately.
 *
 * If the boot log shows EOS_Initialize being retried in a loop, or a managed
 * exception escaping EOSManager, set this to 0 (EOS_SUCCESS) instead and let
 * the downstream calls no-op. Which of the two this build wants is an empirical
 * question -- read the log, do not reason about it from here.
 * ------------------------------------------------------------------------ */
#define BM_EOS_INIT_RESULT 14

/* ---------------------------------------------------------------------------
 * Rendering
 *
 * The game's boot.config carries no graphics API preference, so Unity picks
 * from the player settings baked into data.unity3d. libunity.so has both the
 * Vulkan and GLES backends compiled in, but only libEGL.so is a hard DT_NEEDED
 * -- Vulkan is dlopen'd. libc_shim.c's dlopen refuses "libvulkan.so", which
 * forces the GLES path onto mesa/nouveau. Leave that refusal in place; the
 * nouveau Vulkan driver is not a viable target here.
 * ------------------------------------------------------------------------ */
#define BM_FORCE_GLES 1

/* Apply the vsync patch from bm_offsets.h. Turn off to test whether this build
 * actually needs it -- if the game runs at a sane frame rate without it, it
 * does not, and one less patch is one less thing to re-derive on every update. */
#define BM_PATCH_VSYNC 1

/* ---------------------------------------------------------------------------
 * Audio
 *
 * Consumed by opensles.c and the FMOD pump in jni_fake.c. These values are
 * INHERITED FROM cloverpit_nx UNCHANGED and are not tuned for this game --
 * there is no way to tune them without a device. They are recorded here as
 * knobs, with what each one does, so the first crackle has somewhere to start.
 *
 * BM_AUDIO_DEVICE_RATE is the one to look at first if audio is wrong. 24000 is
 * deliberately below the console's native 48000: the inherited pump resamples,
 * and halving the rate halves the work at the cost of high end. If audio is
 * muddy rather than broken, try 48000 before touching anything else.
 *
 * FRAMES_PER_BUFFER and CALLBACK_FRAMES trade latency against underrun. Raise
 * them if audio stutters under load; lower them if input feels detached from
 * its sound effect.
 * ------------------------------------------------------------------------ */
#define BM_AUDIO_DEVICE_RATE       24000
#define BM_AUDIO_FRAMES_PER_BUFFER 64
#define BM_AUDIO_CALLBACK_FRAMES   1024

/* FMOD's DSP period, forced into libunity. See GAME_RVA_FMOD_PERIOD_* .
 *
 * FMOD computes its up-front buffer count as
 *     N = (dspNumBuffers * dspBufferLength) / framesPerBuffer
 * by integer division, and framesPerBuffer comes from the
 * PROPERTY_OUTPUT_FRAMES_PER_BUFFER we answer. When the product is smaller than
 * the divisor, N is 0 -- every init step reports success and NOTHING is ever
 * enqueued. cloverpit_nx calls that "the silent-but-healthy-looking state this
 * port spent three cycles in", and it is exactly where this port was:
 *     [audio] enq=0 consumed=0 cb=0 dry=0 short=0
 * with FMOD's mixer thread idling in nanosleep waiting to be asked for data. */
#define BM_AUDIO_PERIOD_FRAMES     256

/* Up-front buffers, forced. 0 = leave FMOD's own division alone.
 *
 * Set to 4 rather than 0 because N=0 is measured here, not hypothetical: the
 * division is what failed, so forcing the result is the fix that cannot depend
 * on getting the geometry right first. 4 blocks is a few tens of milliseconds
 * of slack against underrun. Raise it if audio drops out under load; it must
 * never be 0. Drop to 0 once the period is confirmed sane and FMOD's own
 * arithmetic can be trusted. */
#define BM_AUDIO_UPFRONT_BUFFERS   4

/* ---------------------------------------------------------------------------
 * Screen
 *
 * The resolution reported to the engine through ScreenManagerAndroid, which
 * would otherwise ask a Java view hierarchy that does not exist. 1920x1080 is
 * the docked panel; handheld renders 1280x720 and the compositor scales, so one
 * value serves both and avoids a mid-session resolution change the engine would
 * have to handle.
 * ------------------------------------------------------------------------ */
#define BM_FORCE_SCREEN_W 854
#define BM_FORCE_SCREEN_H 480

/* The resolution actually in force, defined in config.c and assigned by main.c
 * from the two constants above. Read by the touch, DPI, viewport and JNI
 * display paths -- they follow these rather than the #defines, so a future
 * dynamic docked/handheld switch only has to update these two. */
extern int screen_width;
extern int screen_height;

/* ---------------------------------------------------------------------------
 * Input
 *
 * BM_ENABLE_POINTER_INPUT gates nxp_init(), and the name undersells it badly.
 *
 * It reads as "the stick-driven cursor is optional", and the inherited comment
 * here said exactly that -- claiming the real path was the touchscreen plus
 * injection at android::NewInput::ProcessTouchEvent. That description does not
 * match this port. There is no ProcessTouchEvent injection; input reaches the
 * game through the UnityEngine.Input hooks in bm_input.c, and those read from
 * bm_touch, which is fed only by android_native_feed_hid() -> nxp_poll().
 *
 * And nxp_init() is where hidInitializeTouchScreen() and hidInitializeMouse()
 * live. With this at 0 the TOUCH PANEL ITSELF is never initialised, so the flag
 * does not disable a fallback cursor -- it disables all input. Which is what it
 * did: no touch, and no cursor to fall back on.
 *
 * nx_pointer is the single source for touchscreen fingers, the stick/gyro
 * cursor and USB-mouse taps; they share one slot allocator so nothing has to
 * special-case the cursor. It is not optional here.
 *
 * BM_LOG_BUTTONS logs every HID button transition. Very noisy; for working out
 * why a binding does not land, then straight back off.
 * ------------------------------------------------------------------------ */
#define BM_ENABLE_POINTER_INPUT 1
#define BM_LOG_BUTTONS 0

/* ---------------------------------------------------------------------------
 * Per-call IO and allocator tracing
 *
 * Separate from DEBUG_LOG on purpose, and separately off. Each traced line is a
 * log write, and log writes go to the SD card: cloverpit_nx records a loading
 * screen that hung for 48 seconds with the engine's main thread parked inside
 * fsFileWrite emitting its own trace. Tracing every fopen in a game that probes
 * several candidate paths per asset is the fastest way to reproduce that.
 *
 * Turn one on to chase a specific bug, then turn it straight back off. The
 * asset index in bm_assets.c already logs its misses, which is usually the
 * information TRACE_IO would have been turned on to get.
 * ------------------------------------------------------------------------ */
#define TRACE_IO   0
#define TRACE_MMAP 0

/* ---------------------------------------------------------------------------
 * mmap arena granularity
 *
 * The arena hands back region-aligned reservations, and the value has to match
 * whatever region granularity the rest of the allocator assumes -- a mismatch
 * means Unity trims pages the arena still believes are live.
 *
 * MUST EQUAL BM_REGION_GRANULARITY_MB in bm_patch_granularity.h. That header
 * rewrites Unity's block allocator from 256 MB regions to 64 MB; this arena
 * hands back region-aligned reservations, and if the two disagree Unity trims
 * pages the arena still believes are live.
 *
 * 64 MB is the known-good floor. killerbean_nx records that 16 MB was tried and
 * CORRUPTED Unity's Dynamic Heap allocator during init, via overlapping regions
 * produced by the over-map/trim pattern -- a memory-corruption bug that presents
 * as an unrelated crash later. Not a knob to experiment with casually.
 * ------------------------------------------------------------------------ */
#define MMAP_ARENA_ALIGN ((size_t)64 * 1024 * 1024)

/* ---------------------------------------------------------------------------
 * Memory
 *
 * The three modules map to roughly 110 MB: libil2cpp 81.4 MB across four
 * PT_LOADs, libunity 26.7 MB, libmain a few KB. Add 14 MB of IL2CPP metadata
 * and the managed GC heap on top. That fits comfortably in the ~3.2 GB a title
 * override gets, and nowhere near what applet mode gets -- which is why applet
 * mode is not supported.
 *
 * They are NOT loaded at a fixed address. main.c carves SO_REGION_BYTES off the
 * tail of the heap in __libnx_initheap and places the three end to end out of
 * it with a bump allocator, because so_load() takes (base, max_size) and
 * refuses any module larger than the space it is given.
 *
 * LOAD_ADDRESS is kept only because the Makefile passes -DLOAD_ADDRESS and a
 * couple of files in this lineage historically read it. Nothing in this tree
 * does any more; the value below is inert. Do not add code that trusts it.
 *
 * The GPU arena matters more than the total. nouveau's page-aligned buffers
 * come out of newlib's general heap by default, fragment it, and eventually a
 * ~20 MB contiguous run cannot be placed -- which presents as a console freeze
 * at a reproducible frame count rather than as an allocation failure. See
 * bm_gpuarena.c.
 * ------------------------------------------------------------------------ */
#ifndef LOAD_ADDRESS
#define LOAD_ADDRESS 0xC0000000
#endif

#define BM_GPU_ARENA_MB 320

/* Held back from the heap in __libnx_initheap so switch-mesa has room.
 *
 * The stock libnx margin is 2 MB, which starves the driver: a graphics
 * allocation failing inside the compositor path wedges the console rather than
 * failing the process, so this is not a soft limit. 192 MB covers a
 * triple-buffered 1080p swapchain (~24 MB) plus render textures, command
 * buffers and texture staging, with margin. If the managed side OOMs instead,
 * this is the knob to trade back. */
#define GFX_RESERVE_MB 192u

/* Log every GameLoadingStep entry, via a detour on LoadStep (bm_steptrace.c).
 *
 * The bootstrap logs only completions, so the step that hangs is the one that
 * never appears -- and there is no way to tell "started and stuck" from "never
 * reached". This closes that gap. A handful of lines per boot; leave it on
 * until the boot is solved. */
/* Skip the Epic login screen and start offline automatically.
 *
 * There is no EOS on this platform, so LOGIN WITH EPIC GAMES cannot succeed and
 * its screen has no way back. 1 makes the screen take its own PLAY OFFLINE path
 * on the first frame. Set to 0 to get the screen back (the popup/button patches
 * still stop it hanging). */
#define BM_FORCE_OFFLINE 1

#define BM_TRACE_LOADING_STEPS 1

/* Detour RemoteConfigService.Bind to log entry/exit.
 *
 * OFF. It was added to test a hypothesis that turned out to be wrong (the stuck
 * step is EpicLoginCheck, not RemoteConfig), and it is now a confound rather
 * than an instrument.
 *
 * Bind+0x10 is `bl RemoteConfigService.Initialize` -- the instruction
 * IMMEDIATELY after the 16 bytes hook_arm64 overwrites, and the exact
 * instruction whose callee then faulted on a null `this`. The trampoline is
 * written to preserve x0 across the replay and by inspection it does, so this
 * may well be innocent. But "I hooked Bind and the game began faulting at
 * Bind+0x10" is not a coincidence to carry while debugging something else, and
 * the trace has already served its purpose: it fired zero times, which is how
 * we know Bind was not on the path to the earlier hang.
 *
 * Turn it back on only to investigate Bind itself, and expect to re-verify the
 * register state at Bind+0x10 first. */
#define BM_TRACE_REMOTECONFIG_BIND 0

#endif /* BM_CONFIG_H */
