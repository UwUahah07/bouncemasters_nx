/* ---------------------------------------------------------------------------
 * bm_vsync.c -- drive Unity's Android vsync state from our own render loop.
 *
 * THE PROBLEM
 * -----------
 * Unity's Android player expects Choreographer to bump a frame counter once per
 * display refresh. Choreographer is an Android framework service; there is none
 * here. So s_FrameCounter never advances, WaitVSync(long) spins forever in
 * pthread_cond_wait, and the game hangs on a black screen after the splash.
 *
 * WHY NOT JUST PATCH WaitVSync
 * ----------------------------
 * Because the engine clock is derived from the counter, not from the wait.
 * WaitForLastPresentationAndGetTimestamp() computes each frame's timestamp as
 *
 *     (target_frame - observed_counter) * period + base
 *
 * where target_frame advances every frame regardless. Neuter the wait and the
 * observed counter stays frozen while the target climbs, so that difference
 * grows without bound and the engine clock accelerates away -- deltaTime
 * enormous and rising, physics and tweens destroyed. Leave the counter frozen
 * without neutering the wait and you get the opposite failure, which
 * killerbean_nx documents: deltaTime pinned at 0, the boot logo's DOTween fade
 * never completes, boot never starts.
 *
 * Both failure modes present as "black screen", which is why it is worth being
 * precise here rather than trying the cheap patch first.
 *
 * WHAT THIS DOES
 * --------------
 * Advances the whole vsync state block once per presented frame and signals the
 * condition variable the engine is waiting on. WaitVSync then returns on its
 * own and needs no patch at all.
 *
 * The block is four fields, all symbol-named in libunity_sym.so:
 *
 *   s_FrameCounter    uint64  frame count      -- what WaitVSync compares
 *   s_LastFrameTime   uint64  NANOSECONDS      -- this frame's timestamp
 *   s_PrevFrameTime   uint64  NANOSECONDS      -- last frame's timestamp
 *   s_NanoTimeOffset  double  seconds          -- added to the converted base
 *
 * GetVSyncInfo() turns them into the engine's clock:
 *
 *   absolute time   = s_LastFrameTime * 1e-9 + s_NanoTimeOffset
 *   frame interval  = (s_LastFrameTime - s_PrevFrameTime) * 1e-9
 *
 * THE UNITS ARE THE WHOLE POINT. These are nanosecond wall-clock timestamps,
 * not sequence numbers. Reading the block as a counter triple and incrementing
 * each field by 1 per frame -- which is what it looks like at a glance -- gives
 * a frame interval of one nanosecond and an engine clock that never
 * meaningfully advances. Write real monotonic nanoseconds, which is what the
 * fields are for and what makes the interval reflect the frame we actually
 * just presented.
 *
 * CALL bm_vsync_tick() EXACTLY ONCE PER PRESENTED FRAME, after the buffer swap.
 * It is the port's entire notion of time passing: call it more often and the
 * game runs fast, less often and it runs slow.
 * ------------------------------------------------------------------------- */

#include <switch.h>
#include <stdint.h>
#include <pthread.h>

#include "config.h"
#include "bm_offsets.h"
#include "diag.h"
#include "util.h"   /* debugPrintf */
#include "so_util.h"

/* Clamp the per-frame delta. A pause -- a breakpoint, a long asset load, the
 * console suspending -- produces one enormous interval, and Unity will happily
 * integrate physics across it. Reporting at most this much keeps a hitch a
 * hitch instead of teleporting everything. 100 ms is about six frames. */
#ifndef BM_VSYNC_MAX_DELTA_NS
#define BM_VSYNC_MAX_DELTA_NS 100000000ull
#endif

/* Floor, so an interval is never zero. GetVSyncInfo()'s result feeds
 * Time.deltaTime, and a zero there divides by zero in anything computing a
 * rate. One microsecond is far below a real frame and safely non-zero. */
#ifndef BM_VSYNC_MIN_DELTA_NS
#define BM_VSYNC_MIN_DELTA_NS 1000ull
#endif

/* The loader's shims own these. The engine's pthread objects are our fakes, so
 * they must be locked through the same entry points the engine used to create
 * them -- calling the host libnx primitives directly on these addresses would
 * be operating on a structure the host runtime never allocated. */
extern int pthread_mutex_lock_fake(pthread_mutex_t **m);
extern int pthread_mutex_unlock_fake(pthread_mutex_t **m);
extern int pthread_cond_broadcast_fake(pthread_cond_t **c);

extern so_module unity_mod;

static pthread_mutex_t **g_monitor;
static pthread_cond_t  **g_cond;
static volatile uint64_t *g_counter;
static volatile uint64_t *g_last_ns;
static volatile uint64_t *g_prev_ns;

static int      g_ready;
static uint64_t g_ticks;

/* The pump's OWN state. Deliberately not read back out of the engine's block.
 *
 * The first version computed its delta as `now_ns() - *g_last_ns`, i.e. it
 * trusted whatever was in s_LastFrameTime. That works only while nothing else
 * writes it. Once the Choreographer patches landed and ChoreographerNDK came
 * up, something began writing that field at or ahead of our clock, so
 * `now > last` was false on every tick, the delta collapsed to the 1 us floor,
 * and the log filled with:
 *
 *     [vsync] tick 240  counter=240  dt=1 us       (28 of 29 ticks)
 *
 * 60 ticks/second x 1 us is 60 microseconds of engine time per real second.
 * Rendering looked perfect -- 780 frames, alive=1, swaps fine -- because the
 * managed Time hooks in bm_time.c keep their own base. But Unity's NATIVE
 * systems (coroutine WaitForSeconds, animation, particles) derive from this
 * block, so a splash screen waiting on a timer waits roughly forever. That is
 * the "renders but never progresses" signature exactly.
 *
 * These two make the pump authoritative: the delta comes from the real clock
 * measured against our own previous sample, and the value written is our own
 * accumulator. If anything else writes the field, we simply overwrite it on the
 * next tick -- which is correct, because in this port we ARE the frame clock. */
static uint64_t g_real_prev;   /* real time at the previous tick              */
static uint64_t g_reported;    /* the value we last wrote to s_LastFrameTime  */

/* The engine's own frame-clock entry point, resolved at init. When present we
 * call it instead of writing the fields ourselves -- see bm_offsets.h. */
typedef void (*fn_setFrameTimeNanos)(int64_t ns);
static fn_setFrameTimeNanos g_set_frame_time;

/* Monotonic nanoseconds straight off the ARM generic timer.
 *
 * Not clock_gettime(CLOCK_MONOTONIC): newlib guards the POSIX clock
 * declarations behind feature macros that only become visible once <switch.h>
 * has been included, so a file that reaches for it without that include fails
 * to compile with a bare "CLOCK_MONOTONIC undeclared" -- which reads like a
 * missing <time.h> and is not. diag.c and nx_pointer.c already read time this
 * way; matching them keeps one answer in the tree instead of two.
 *
 * It is also the cheaper call. This runs once per presented frame and feeds the
 * engine's entire notion of elapsed time, so it wants the hardware counter, not
 * a trip through libc. */
static uint64_t now_ns(void) {
  return armTicksToNs(armGetSystemTick());
}

/* -------------------------------------------------------------------------
 * Resolve the state block once, after libunity is mapped.
 * Returns 0 on success. Safe to call repeatedly.
 * ------------------------------------------------------------------------ */
int bm_vsync_init(void) {
  if (g_ready) return 0;

  uintptr_t base = (uintptr_t)unity_mod.load_virtbase;
  if (!base) {
    debugPrintf("[vsync] libunity not mapped yet -- init deferred\n");
    return -1;
  }

  /* OPENBONK: the block addresses below are not guard-checked individually, so
   * check the one instruction they were all read from -- WaitVSync's loop head
   * (ldr x21,[x22,#0xa58]) -- and refuse to write anything on a mismatch. */
  {
    uint32_t live = *(volatile uint32_t *)(base + GAME_RVA_WaitVSync_loop);
    if (live != GUARD_WaitVSync_loop) {
      debugPrintf("[vsync] WaitVSync loop guard mismatch at +0x%x (expected 0x%08x, "
                  "found 0x%08x) -- vsync pump NOT started\n",
                  (unsigned)GAME_RVA_WaitVSync_loop, (unsigned)GUARD_WaitVSync_loop,
                  (unsigned)live);
      return -1;
    }
  }

  g_monitor = (pthread_mutex_t **)(base + GAME_RVA_VSYNC_MONITOR);
  g_cond    = (pthread_cond_t  **)(base + GAME_RVA_VSYNC_COND);
  g_counter = (volatile uint64_t *)(base + GAME_RVA_VSYNC_COUNTER);
  g_last_ns = (volatile uint64_t *)(base + GAME_RVA_LAST_FRAME_TIME);
  g_prev_ns = (volatile uint64_t *)(base + GAME_RVA_PREV_FRAME_TIME);

  /* Seed both timestamps to now, so the very first tick reports one real frame
   * interval rather than the distance from zero to the monotonic clock -- which
   * would be however long the console has been awake, expressed as a single
   * deltaTime. */
  uint64_t t = now_ns();
  g_real_prev = t;
  g_reported  = t;
  *g_prev_ns  = t;
  *g_last_ns  = t;

  debugPrintf("[vsync] monitor=%p cond=%p counter=%p (unity base=0x%lx)\n",
              (void *)g_monitor, (void *)g_cond, (void *)g_counter,
              (unsigned long)base);
  /* Prefer the engine's own setter. Guard-checked: on a mismatch we fall back
   * to writing the fields directly, which still works, just without the
   * engine's ordering guarantees. */
  {
    uint32_t *p = (uint32_t *)(base + GAME_RVA_SetFrameTimeNanos);
    if (*p == GUARD_SetFrameTimeNanos) {
      g_set_frame_time = (fn_setFrameTimeNanos)(base + GAME_RVA_SetFrameTimeNanos);
      debugPrintf("[vsync] using engine SetFrameTimeNanos @ +0x%x\n",
                  (unsigned)GAME_RVA_SetFrameTimeNanos);
    } else {
      debugPrintf("[vsync] SetFrameTimeNanos guard mismatch (found 0x%08x) -- "
                  "falling back to direct field writes\n", *p);
    }
  }

  debugPrintf("[vsync] seeded counter=%llu t=%llu ns\n",
              (unsigned long long)*g_counter, (unsigned long long)t);

  g_ready = 1;
  return 0;
}

/* -------------------------------------------------------------------------
 * One presented frame.
 *
 * Ordering matters. All four writes happen while holding the monitor, because
 * that is the lock GetVSyncInfo() reads them under. Writing them unlocked would
 * let the engine observe the counter advanced but the timestamps not, and
 * compute an interval of zero for that frame.
 *
 * The broadcast happens inside the lock too. That is marginally less efficient
 * than signalling after the unlock, and it is the right trade: a waiter woken
 * between two of our writes would read a torn state block, and one wasted
 * wakeup per frame costs nothing beside that.
 * ------------------------------------------------------------------------ */
void bm_vsync_tick(void) {
  if (!g_ready && bm_vsync_init() != 0) return;

  /* Measure against OUR previous sample, never against the engine's field. */
  uint64_t now = now_ns();
  uint64_t delta = (now > g_real_prev) ? (now - g_real_prev) : 0;
  g_real_prev = now;

  if (delta > BM_VSYNC_MAX_DELTA_NS) delta = BM_VSYNC_MAX_DELTA_NS;
  if (delta < BM_VSYNC_MIN_DELTA_NS) delta = BM_VSYNC_MIN_DELTA_NS;

  /* ADOPT THE ENGINE'S BASE, ADVANCE BY OUR OWN DELTA.
   *
   * EnableFrameTimeTracker() writes s_LastFrameTime from the ENGINE's clock and
   * derives s_NanoTimeOffset from that same base, so absolute time only comes
   * out right if our timestamps continue from where it left off. It runs after
   * this pump starts, and its base is far from armGetSystemTick()'s.
   *
   * The original pump computed `now - *g_last_ns` and so was destroyed by that
   * single write: the engine's value was ahead of our clock, `now > last` was
   * false forever, the delta pinned to the 1 us floor, and the engine clock
   * advanced 60 us per real second. Rendering was perfect (780 frames) and the
   * game never left its splash, because native WaitForSeconds never elapsed.
   *
   * Reading the field is still right -- ADDING our own real delta to it is what
   * makes it safe. A foreign write re-bases us and we carry on from there; it
   * can no longer stall us, because the advance never comes from the field. */
  uint64_t cur = *g_last_ns;
  if (cur != g_reported) {
#if DEBUG_LOG
    static int rebased;
    if (!rebased && g_ticks > 1) {
      rebased = 1;
      debugPrintf("[vsync] re-based by the engine: we wrote %llu, found %llu "
                  "(%+lld ns). Adopting its base and advancing from there.\n",
                  (unsigned long long)g_reported, (unsigned long long)cur,
                  (long long)((int64_t)cur - (int64_t)g_reported));
    }
#endif
    g_reported = cur;
  }
  g_reported += delta;

  if (g_set_frame_time) {
    /* Engine path: it takes the lock, stores Prev/Last, bumps the counter and
     * broadcasts. Must NOT be called holding the monitor -- it locks it. */
    g_set_frame_time((int64_t)g_reported);
  } else {
    pthread_mutex_lock_fake(g_monitor);
    *g_prev_ns = cur;
    *g_last_ns = g_reported;
    (*g_counter)++;
    pthread_cond_broadcast_fake(g_cond);
    pthread_mutex_unlock_fake(g_monitor);
  }

  g_ticks++;

#if DEBUG_LOG
  /* Once a second at 60 Hz. Enough to confirm the pump runs and the engine is
   * consuming frames; not enough to drown the log. If the counter climbs here
   * while the screen stays black, the problem is downstream of vsync and this
   * file is not where to keep looking. */
  if ((g_ticks % 60) == 0) {
    /* Report what the ENGINE will compute, not just what we intended.
     * GetVSyncInfo derives the frame interval as (Last - Prev) * 1e-9, so
     * printing that number is a direct read-out of the value the engine acts
     * on. When the pump was floored this read 0.001 ms while our own `dt`
     * looked plausible in isolation -- the two together would have named the
     * bug immediately. */
    uint64_t L = *g_last_ns, P = *g_prev_ns;
    double engine_dt_ms = (L > P) ? (double)(L - P) / 1e6 : 0.0;
    debugPrintf("[vsync] tick %llu  counter=%llu  dt=%llu us  "
                "engine sees %.2f ms\n",
                (unsigned long long)g_ticks,
                (unsigned long long)*g_counter,
                (unsigned long long)(delta / 1000ull),
                engine_dt_ms);
  }
#endif
}

/* --------------------------------------------------------------------------
 * The pump thread
 *
 * WHY A THREAD AND NOT THE RENDER LOOP
 * ------------------------------------
 * The first version of this called bm_vsync_tick() from bm_boot.c's frame loop,
 * right after nativeRender() returned. That cannot work, and the reason is a
 * plain deadlock:
 *
 *     nativeRender()
 *       -> WaitForLastPresentationAndGetTimestamp()
 *          -> WaitVSync(target)
 *             -> pthread_cond_wait(s_VsyncMonitor.cond)   blocks until
 *                                                         s_FrameCounter >= target
 *
 * The counter is what nativeRender is waiting on, so bumping it *after*
 * nativeRender returns means bumping it after something that never returns.
 * The engine parks on the condvar on the first frame and the loop never runs a
 * second iteration.
 *
 * Choreographer is a separate Android system service delivering callbacks off
 * the render thread, so an independent ticker is what the engine was written
 * against. This is that.
 *
 * BIONIC TLS. This thread runs engine code -- pthread_*_fake operate on the
 * engine's own primitives, and the shims read the stack guard through tpidr.
 * Every thread that touches engine state needs its own Bionic-shaped TLS block
 * installed, the same way diag.c does for the watchdog. Without it the first
 * call reads a guard from libnx's thread pointer and aborts.
 * ------------------------------------------------------------------------ */

static Thread   g_vsync_thread;
static uint8_t  g_vsync_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
static volatile int g_vsync_run;

static void vsync_thread_main(void *arg) {
  (void)arg;
  install_bionic_tls(g_vsync_tls);
  debugPrintf("[vsync] pump thread started (%llu ns period)\n",
              (unsigned long long)VSYNC_PERIOD_NS);

  uint64_t next = now_ns() + VSYNC_PERIOD_NS;
  while (g_vsync_run) {
    uint64_t t = now_ns();
    if (t < next) {
      svcSleepThread((int64_t)(next - t));
    } else if (t - next > VSYNC_PERIOD_NS * 8) {
      /* Fell far behind -- a long asset load, or the console suspended. Do not
       * try to catch up by ticking N times: that would hand the engine a burst
       * of frames it never rendered. Re-base and carry on. */
      next = t;
    }
    next += VSYNC_PERIOD_NS;
    bm_vsync_tick();
  }
  debugPrintf("[vsync] pump thread stopped after %llu ticks\n",
              (unsigned long long)g_ticks);
}

int bm_vsync_start(void) {
  if (!g_ready && bm_vsync_init() != 0) return -1;
  if (g_vsync_run) return 0;

  g_vsync_run = 1;
  /* Priority 0x2C is one step below the engine main thread (0x2B here), so the
   * pump cannot starve rendering but still runs promptly when the renderer
   * blocks -- which is exactly the moment it is needed. */
  Result rc = threadCreate(&g_vsync_thread, vsync_thread_main, NULL, NULL,
                           0x4000, 0x2C, -2);
  if (R_FAILED(rc)) {
    debugPrintf("[vsync] threadCreate failed 0x%08x -- the engine WILL deadlock\n", rc);
    g_vsync_run = 0;
    return -1;
  }
  threadStart(&g_vsync_thread);
  return 0;
}

void bm_vsync_stop(void) {
  if (!g_vsync_run) return;
  g_vsync_run = 0;
  threadWaitForExit(&g_vsync_thread);
  threadClose(&g_vsync_thread);
}

/* Frames pumped so far -- for the diagnostics overlay, and for telling whether
 * a hang is upstream or downstream of here. */
uint64_t bm_vsync_frames(void) { return g_ticks; }
