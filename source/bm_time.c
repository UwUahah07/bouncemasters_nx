/* ---------------------------------------------------------------------------
 * bm_time.c -- drive UnityEngine.Time from our own clock.
 *
 * WHY THIS EXISTS ALONGSIDE bm_vsync.c
 * ------------------------------------
 * bm_vsync.c advances the engine's NATIVE vsync state, which is what stops
 * WaitVSync blocking and what the native TimeManager derives from. That is
 * necessary and it stays.
 *
 * It is not sufficient on Unity 6. cloverpit_nx measured that TimeManager::Update
 * never runs at all on this engine generation, so anything downstream of it is
 * inert -- and UnityEngine.Time's getters are downstream of it.
 *
 * This works one level up, at the managed boundary. Time's getters are ordinary
 * compiled methods in libil2cpp, so hooking them makes C# see time advance no
 * matter what the native subsystem is doing. The two paths do not fight: both
 * derive from the same monotonic base, so if TimeManager ever does run it steps
 * native time with the same clock these getters report.
 *
 * The symptom is specific and matches what this port is stuck on: a loading
 * spinner driven by `transform.Rotate(0, 0, speed * Time.deltaTime)` renders one
 * frame of motion and then sits still forever, because deltaTime reads 0 on
 * every frame after the first.
 *
 * IL2CPP CALLING CONVENTION
 * -------------------------
 * A static managed method compiles to a native function taking a hidden trailing
 * `const MethodInfo*`. Every hook here ignores its arguments and returns a
 * value, so the extra parameter is harmless -- it sits unread in the next
 * register. Float returns come back in s0, which is what these signatures give.
 *
 * bm_time_tick() MUST be called once per rendered frame. It is what samples
 * deltaTime; without it every delta reads as the clamp floor.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <switch.h>

#include "bm_managed.h"
#include "config.h"
#include "diag.h"
#include "so_util.h"
#include "util.h"

/* Clamp per-frame delta. A synchronous scene load can take seconds; reporting
 * that as one delta makes physics and animation explode on the frame after it.
 * 100 ms is about six frames -- enough that a hitch stays a hitch. The floor
 * keeps anything computing a rate from dividing by zero. */
#define BMT_MIN_DELTA 0.001f
#define BMT_MAX_DELTA 0.100f

#define BMT_FIXED_DELTA (1.0f / 60.0f)

static uint64_t g_base_ns;      /* captured at install */
static uint64_t g_last_ns;
static float    g_delta = BMT_FIXED_DELTA;   /* REAL elapsed, unscaled */
static float    g_smooth = BMT_FIXED_DELTA;

/* Unity keeps two clocks, and the difference is the whole point of timeScale.
 *
 *   Time.time / deltaTime / smoothDeltaTime   SCALED by timeScale
 *   Time.unscaledTime / unscaledDeltaTime     real
 *   Time.realtimeSinceStartup                 real, never scaled
 *   Time.fixedDeltaTime                       the fixed step, not scaled
 *
 * The first version reported the real delta for both, on the reasoning that
 * Unity applies timeScale itself and scaling here would apply it twice. That is
 * true of a real engine, where TimeManager does the scaling before anything
 * reads Time.deltaTime -- but this port replaces the getters outright, so
 * nothing else is left to do it. Whatever the getter returns IS deltaTime.
 *
 * The symptom was exact: Bouncemasters slows down on the opening swings by
 * setting timeScale, the log showed `dt=16.84 ms scale=0.34` frame after frame,
 * and nothing ever slowed -- which makes those shots impossible to aim and the
 * game impossible to progress. Two accumulators now, so scaled and unscaled
 * time diverge the way managed code expects. */
static double   g_scaled_s;      /* accumulated SCALED seconds   */
static double   g_unscaled_s;    /* accumulated real seconds     */
static uint32_t g_frames;
static int      g_ready;
static float    g_time_scale = 1.0f;   /* set via Time.timeScale; see below */

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* Real seconds since install. Never scaled -- this is what
 * realtimeSinceStartup means, and what the AsDouble variants report. */
static float elapsed_s(void) {
  if (!g_ready) return 0.0f;
  return (float)((double)(now_ns() - g_base_ns) * 1e-9);
}

void bm_time_tick(void) {
  if (!g_ready) return;

  uint64_t t = now_ns();
  float d = (float)((double)(t - g_last_ns) * 1e-9);
  g_last_ns = t;

  if (d < BMT_MIN_DELTA) d = BMT_MIN_DELTA;
  if (d > BMT_MAX_DELTA) d = BMT_MAX_DELTA;
  g_delta = d;

  /* Unity's smoothDeltaTime is a low-pass of deltaTime. Matching that keeps
   * anything using it for camera smoothing from juddering. */
  g_smooth += (d - g_smooth) * 0.2f;

  /* Advance both clocks. The scaled one is what Time.time reports, so a game
   * paused with timeScale = 0 sees Time.time stop while unscaledTime keeps
   * running -- which is exactly what pause menus rely on. */
  g_unscaled_s += (double)d;
  g_scaled_s   += (double)d * (double)g_time_scale;

  g_frames++;

#if DEBUG_LOG
  /* Once a second. There was previously NO way to tell from a log whether
   * managed time was advancing -- only the vsync pump reported, and that is a
   * different clock for a different consumer. When the vsync pump was stalling
   * at its 1 us floor the managed clock was fine, and nothing in the log said
   * so; the two had to be told apart by reasoning rather than by reading.
   * They are separate lines now. */
  if ((g_frames % 60u) == 0u)
    debugPrintf("[time] frame %u  t=%.2fs (real %.2fs)  dt=%.2f ms "
                "(real %.2f)  scale=%.2f\n",
                (unsigned)g_frames, g_scaled_s, g_unscaled_s,
                (double)(g_delta * g_time_scale * 1000.0f),
                (double)(g_delta * 1000.0f), (double)g_time_scale);
#endif
}

/* ------------------------------------------------------------------ hooks */

static float bmt_time(void)                 { return (float)g_scaled_s; }
static float bmt_deltaTime(void)            { return g_delta * g_time_scale; }
static float bmt_unscaledTime(void)         { return (float)g_unscaled_s; }
static float bmt_unscaledDeltaTime(void)    { return g_delta; }
/* Unity's fixedDeltaTime is the configured step, NOT scaled -- the physics
 * loop applies timeScale by running fewer steps, not by shortening them. */
static float bmt_fixedDeltaTime(void)       { return BMT_FIXED_DELTA; }
static float bmt_smoothDeltaTime(void)      { return g_smooth * g_time_scale; }
static float bmt_timeScale(void)            { return g_time_scale; }
static int   bmt_frameCount(void)           { return (int)g_frames; }
static float bmt_realtimeSinceStartup(void) { return elapsed_s(); }

/* timeSinceLevelLoad should reset per scene. We have no scene-load signal, so
 * it tracks total elapsed. Anything using it for a fade-in gets a value that
 * only ever grows, which is right on the first scene and harmless after --
 * far better than the 0 it reads today. */
static float bmt_timeSinceLevelLoad(void)   { return (float)g_scaled_s; }

/* ---- the double-precision variants ----
 *
 * These read the NATIVE clock, which is the one that does not advance. Leaving
 * them unhooked while the float getters are hooked is worse than leaving both:
 * managed code that mixes Time.time with Time.timeAsDouble would see two clocks
 * disagreeing, one moving and one frozen. dump.cs shows this game references
 * them, so they are not hypothetical. */
static double bmt_timeAsDouble(void) { return g_scaled_s; }
static double bmt_realtimeSinceStartupAsDouble(void) {
  if (!g_ready) return 0.0;
  return (double)(now_ns() - g_base_ns) * 1e-9;   /* real, never scaled */
}

/* ---- timeScale: a value, not a constant ----
 *
 * get_timeScale used to return a hardcoded 1.0f. That is wrong the moment the
 * game pauses: a pause menu sets Time.timeScale = 0, our setter was not hooked
 * so the assignment went to the frozen native TimeManager, and our getter kept
 * answering 1.0 -- so the pause would never take effect and gameplay would keep
 * running underneath the menu.
 *
 * Hooking both makes them coherent: the setter records, the getter reports what
 * was set. Note deltaTime deliberately stays UNSCALED here -- Unity applies
 * timeScale to deltaTime itself, and applying it twice would make a 0.5x slow
 * motion run at 0.25x. Unity's own contract is that unscaledDeltaTime ignores
 * it, which is what we return for both; if the game turns out to depend on
 * deltaTime being scaled, multiply in bmt_deltaTime, not here. */
static void  bmt_set_timeScale(float v) {
  if (v < 0.0f) v = 0.0f;          /* Unity clamps negatives to 0 */
  g_time_scale = v;
}

/* ---- RationalTime ----
 *
 * struct RationalTime { long m_Count; TicksPerSecond m_TicksPerSecond; } where
 * TicksPerSecond is { uint m_Numerator; uint m_Denominator; }. 16 bytes.
 *
 * Report elapsed time in nanosecond ticks: count = ns, rate = 1e9/1. That is
 * exact, needs no rounding, and matches what the float getters report. */
typedef struct { int64_t count; uint32_t num, den; } URationalTime;

static void bmt_rational_fill(URationalTime *r) {
  if (!r) return;
  r->count = g_ready ? (int64_t)(now_ns() - g_base_ns) : 0;
  r->num   = 1000000000u;
  r->den   = 1u;
}
static void bmt_timeAsRational_Injected(URationalTime *ret) { bmt_rational_fill(ret); }

/* The non-Injected form returns the 16-byte struct by value. On AArch64 a
 * 16-byte POD of two 8-byte-or-smaller fields is returned in x0:x1, not through
 * x8 -- so returning it directly is correct and no hidden pointer is involved. */
static URationalTime bmt_timeAsRational(void) {
  URationalTime r; bmt_rational_fill(&r); return r;
}

/* ---------------------------------------------------------------- install */

int bm_time_install(so_module *il2cpp) {
  static const uint32_t g_time        [3] = BMM_G3_Time_get_time;
  static const uint32_t g_deltaTime   [3] = BMM_G3_Time_get_deltaTime;
  static const uint32_t g_uTime       [3] = BMM_G3_Time_get_unscaledTime;
  static const uint32_t g_uDelta      [3] = BMM_G3_Time_get_unscaledDeltaTime;
  static const uint32_t g_fixed       [3] = BMM_G3_Time_get_fixedDeltaTime;
  static const uint32_t g_smoothg     [3] = BMM_G3_Time_get_smoothDeltaTime;
  static const uint32_t g_scale       [3] = BMM_G3_Time_get_timeScale;
  static const uint32_t g_frame       [3] = BMM_G3_Time_get_frameCount;
  static const uint32_t g_real        [3] = BMM_G3_Time_get_realtimeSinceStartup;
  static const uint32_t g_level       [3] = BMM_G3_Time_get_timeSinceLevelLoad;
  static const uint32_t g_tdbl        [3] = BMM_G3_Time_get_timeAsDouble;
  static const uint32_t g_rtdbl       [3] = BMM_G3_Time_get_realtimeSinceStartupAsDouble;
  static const uint32_t g_setscale    [3] = BMM_G3_Time_set_timeScale;
  static const uint32_t g_rat         [3] = BMM_G3_Time_get_timeAsRational;
  static const uint32_t g_ratinj      [3] = BMM_G3_Time_get_timeAsRational_Injected;

  g_base_ns = g_last_ns = now_ns();
  g_ready = 1;

  int n = 0;
  n += bm_managed_hook(il2cpp, "Time.get_time",
                       BMM_RVA_Time_get_time, g_time, (void *)&bmt_time);
  n += bm_managed_hook(il2cpp, "Time.get_deltaTime",
                       BMM_RVA_Time_get_deltaTime, g_deltaTime, (void *)&bmt_deltaTime);
  n += bm_managed_hook(il2cpp, "Time.get_unscaledTime",
                       BMM_RVA_Time_get_unscaledTime, g_uTime, (void *)&bmt_unscaledTime);
  n += bm_managed_hook(il2cpp, "Time.get_unscaledDeltaTime",
                       BMM_RVA_Time_get_unscaledDeltaTime, g_uDelta, (void *)&bmt_unscaledDeltaTime);
  n += bm_managed_hook(il2cpp, "Time.get_fixedDeltaTime",
                       BMM_RVA_Time_get_fixedDeltaTime, g_fixed, (void *)&bmt_fixedDeltaTime);
  n += bm_managed_hook(il2cpp, "Time.get_smoothDeltaTime",
                       BMM_RVA_Time_get_smoothDeltaTime, g_smoothg, (void *)&bmt_smoothDeltaTime);
  n += bm_managed_hook(il2cpp, "Time.get_timeScale",
                       BMM_RVA_Time_get_timeScale, g_scale, (void *)&bmt_timeScale);
  n += bm_managed_hook(il2cpp, "Time.get_frameCount",
                       BMM_RVA_Time_get_frameCount, g_frame, (void *)&bmt_frameCount);
  n += bm_managed_hook(il2cpp, "Time.get_realtimeSinceStartup",
                       BMM_RVA_Time_get_realtimeSinceStartup, g_real, (void *)&bmt_realtimeSinceStartup);
  n += bm_managed_hook(il2cpp, "Time.get_timeSinceLevelLoad",
                       BMM_RVA_Time_get_timeSinceLevelLoad, g_level, (void *)&bmt_timeSinceLevelLoad);

  n += bm_managed_hook(il2cpp, "Time.get_timeAsDouble",
                       BMM_RVA_Time_get_timeAsDouble, g_tdbl, (void *)&bmt_timeAsDouble);
  n += bm_managed_hook(il2cpp, "Time.get_realtimeSinceStartupAsDouble",
                       BMM_RVA_Time_get_realtimeSinceStartupAsDouble, g_rtdbl,
                       (void *)&bmt_realtimeSinceStartupAsDouble);
  n += bm_managed_hook(il2cpp, "Time.set_timeScale",
                       BMM_RVA_Time_set_timeScale, g_setscale, (void *)&bmt_set_timeScale);
  n += bm_managed_hook(il2cpp, "Time.get_timeAsRational",
                       BMM_RVA_Time_get_timeAsRational, g_rat, (void *)&bmt_timeAsRational);
  n += bm_managed_hook(il2cpp, "Time.get_timeAsRational_Injected",
                       BMM_RVA_Time_get_timeAsRational_Injected, g_ratinj,
                       (void *)&bmt_timeAsRational_Injected);

  debugPrintf("[time] %d/15 Time accessors hooked (every member UnityEngine.Time has)\n", n);
  if (n < 15)
    debugPrintf("[time] %d NOT installed -- mixed clocks are worse than none;\n"
                "       check the guard mismatches above\n", 15 - n);
  if (n == 0)
    debugPrintf("[time] NONE installed -- managed time will stay frozen and any\n"
                "       Time.deltaTime-driven animation will render one frame\n"
                "       and stop. Regenerate bm_managed.h from a dump.cs that\n"
                "       matches THIS libil2cpp.\n");
  return n;
}
