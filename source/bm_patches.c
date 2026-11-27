/* ---------------------------------------------------------------------------
 * bm_patches.c -- guarded code patches applied to the game's libunity.so.
 *
 * THE DISCIPLINE
 * --------------
 * Every patch here reads the word it is about to overwrite and compares it
 * against a guard before writing. On mismatch it logs loudly and SKIPS that
 * patch -- it does not write anyway, and it does not abort.
 *
 * That asymmetry is deliberate. A skipped audio patch is silence; a skipped
 * reachability patch is a slower boot. Both are visible, survivable, and point
 * straight at "re-derive the offsets". A patch written into a shifted binary
 * corrupts one instruction somewhere in 18.6 MB of .text and surfaces later as
 * a crash with no relationship to its cause. Always fail toward the
 * diagnosable outcome.
 *
 * Call bm_apply_patches() once, after libunity is mapped and before the engine
 * starts. It returns the number applied; the log lists each one by name.
 *
 * WHAT IS NOT HERE
 * ----------------
 * Vsync. The engine's frame counter is driven at runtime by bm_vsync.c rather
 * than patched -- see the long note in bm_offsets.h for why patching WaitVSync
 * makes the engine clock run away instead of fixing it.
 *
 * Swappy. It needs no patch: libswappywrapper.so is absent from the APK, so
 * Swappy::IsEnabledAndActive() returns 0 and the engine takes the WaitVSync
 * fallback on its own. libc_shim.c's dlopen refuse list keeps that true and
 * makes the failure immediate rather than a filesystem miss.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "bm_offsets.h"
#include "bm_patch_granularity.h"
#include "diag.h"
#include "util.h"   /* debugPrintf */
#include "so_util.h"

extern so_module unity_mod;

/* so_util maps executable segments Perm_Rx, so a direct store into .text would
 * fault. so_patch_code() is the lineage's answer: it aliases the target pages
 * RW at a fresh ASLR slot, copies, unmaps, then flushes the D-cache and
 * invalidates the I-cache. That last step is why nothing here writes .text
 * directly -- without the I-cache invalidate the CPU can keep executing the
 * stale instruction for an arbitrary time after the store, which produces a
 * patch that appears to work intermittently.
 *
 * Reading through load_virtbase is fine without any of that: Perm_Rx is
 * readable. Only the write needs the alias. */

static int g_applied, g_skipped;

/* Write `n` words at `rva`, but only if the first word still matches `guard`. */
static int patch_words(const char *what, uint32_t rva, uint32_t guard,
                       const uint32_t *words, int n) {
  uintptr_t base = (uintptr_t)unity_mod.load_virtbase;
  if (!base) {
    debugPrintf("[patch] %s SKIPPED: libunity not mapped\n", what);
    g_skipped++;
    return 0;
  }

  uint32_t *p = (uint32_t *)(base + rva);
  uint32_t live = *p;
  if (live != guard) {
    debugPrintf("[patch] %s SKIPPED: guard mismatch at +0x%x "
                "(expected 0x%08x, found 0x%08x)\n"
                "        Offsets are stale for this libunity. Re-derive with\n"
                "        tools/symbols.py verify before trusting anything else.\n",
                what, (unsigned)rva, guard, live);
    g_skipped++;
    return 0;
  }

  if (so_patch_code(p, words, (size_t)n * 4) != 0) {
    debugPrintf("[patch] %s FAILED: so_patch_code could not write +0x%x\n",
                what, (unsigned)rva);
    g_skipped++;
    return 0;
  }
  debugPrintf("[patch] %s applied at +0x%x (%d word%s)\n",
              what, (unsigned)rva, n, n == 1 ? "" : "s");
  g_applied++;
  return 1;
}

/* --------------------------------------------------------------------------
 * The patches
 * ------------------------------------------------------------------------ */

/* movz w0, #2 ; ret  -- AUDIO_SELECT_OPENSL */
static const uint32_t P_RET_OPENSL[] = { 0x52800040u, 0xd65f03c0u };

/* movz w0, #0 ; ret  -- NetworkReachability.NotReachable */
static const uint32_t P_RET_ZERO[]   = { 0x52800000u, 0xd65f03c0u };

/* movz w0, #33 ; ret  -- DeviceApiLevel, matching jni_fake.c's NX_SDK_INT */
static const uint32_t P_RET_API33[]  = { 0x52800420u, 0xd65f03c0u };

static const uint32_t P_CHOREO_CTX[]   = { PATCH_Choreographer_ctx_branch };
static const uint32_t P_NDK_NOHANDLER[] = { PATCH_NdkLooper_CreateHandler_call };

/* b +0x10, replacing b.ls +0x10 -- same target, unconditional */
static const uint32_t P_FMOD_BRANCH[] = { PATCH_FMOD_BUFGEOM_BRANCH };

/* Move Unity's block allocator from 256 MB regions to 64 MB.
 *
 * ALL-OR-NOTHING, and that matters more here than for the single-site patches
 * above. The 20 words implement one computation across ten functions; writing
 * some of them leaves the allocator computing region indices two different ways
 * at once, which corrupts the free list rather than failing cleanly. So: verify
 * every word first, then write. See bm_patch_granularity.h for the derivation.
 *
 * Must run BEFORE any engine code -- before init arrays, before JNI_OnLoad --
 * because MemoryManager reserves its first regions during static init. */
static int apply_granularity(void) {
  uintptr_t base = (uintptr_t)unity_mod.load_virtbase;
  if (!base) {
    debugPrintf("[gran] libunity not mapped\n");
    return 0;
  }

  int bad = 0;
  for (int i = 0; i < BM_GRANULARITY_WORDS_N; i++) {
    const BmPatchWord *p = &BM_GRANULARITY_WORDS[i];
    uint32_t live = *(const uint32_t *)(base + p->rva);
    if (live != p->from) {
      debugPrintf("[gran] site %d MISMATCH at +0x%x: expected 0x%08x, found 0x%08x  (%s)\n",
                  i, (unsigned)p->rva, p->from, live, p->what);
      bad++;
    }
  }
  if (bad) {
    debugPrintf("[gran] %d of %d sites do not match -- NOTHING PATCHED.\n"
                "       This is not the libunity.so the table was derived from.\n"
                "       Re-derive with tools/scan_granularity.py and rebuild.\n"
                "       Running unpatched will fail in MemoryManager init with\n"
                "       a ~512 MB memalign that cannot be satisfied.\n",
                bad, BM_GRANULARITY_WORDS_N);
    g_skipped++;
    return 0;
  }

  for (int i = 0; i < BM_GRANULARITY_WORDS_N; i++) {
    const BmPatchWord *p = &BM_GRANULARITY_WORDS[i];
    if (so_patch_code((uint32_t *)(base + p->rva), &p->to, 4) != 0) {
      /* Cannot happen after the verify pass unless so_patch_code itself fails,
       * and by here some words are already written. Say so loudly: the process
       * is now in the state this function exists to avoid. */
      debugPrintf("[gran] FATAL: write failed at site %d (+0x%x) AFTER %d writes.\n"
                  "       The allocator is now half-patched. Do not trust this run.\n",
                  i, (unsigned)p->rva, i);
      return -1;
    }
  }

  debugPrintf("[gran] %d sites patched: Unity block allocator 256MB -> %u MB regions\n",
              BM_GRANULARITY_WORDS_N, BM_REGION_GRANULARITY_MB);
  g_applied++;
  return 1;
}

int bm_apply_patches(void) {
  g_applied = g_skipped = 0;

  /* FIRST. Everything else here is a behaviour tweak; this one decides whether
   * the engine can allocate at all. */
  apply_granularity();

  /* [1] Audio backend -> OpenSL.
   *
   * Order matters against [2]: this one decides that OpenSL is used at all,
   * and [2] only matters once it is. Applying [2] alone is harmless but
   * pointless; applying [1] alone gets you to slCreateEngine() and then FMOD
   * error 60. */
  patch_words("audio/select-opensl",
              GAME_RVA_GetAndroidAudioOutputType,
              GUARD_GetAndroidAudioOutputType,
              P_RET_OPENSL, 2);

  /* [2] FMOD OpenSL buffer-geometry bound check -> always pass. */
#if 0
  patch_words("audio/fmod-bufgeom",
              GAME_RVA_FMOD_BUFGEOM_BRANCH,
              GUARD_FMOD_BUFGEOM_BRANCH,
              P_FMOD_BRANCH, 1);
#endif

  /* [3] APK filesystem handler -> decline every path.
   *
   * Must be applied; without it Unity zip-scans a non-zip for every file open
   * and the boot never finishes. See the note in bm_offsets.h. */
  patch_words("fs/apk-canhandle-false",
              GAME_RVA_APK_CanHandle,
              GUARD_APK_CanHandle,
              P_RET_ZERO, 2);

  /* [3b] FMOD OpenSL: force the DSP period, then force the buffer count.
   *
   * Without these FMOD's up-front buffer count divides to zero, init reports
   * success, and the output is silent forever. Full derivation in bm_offsets.h.
   * Applied as two independent single-word patches so a guard mismatch on one
   * does not silently half-apply the other. */
#if 0
  {
    static const uint32_t period_mov[] = { PATCH_FMOD_PERIOD_MOV };
    static const uint32_t period_str[] = { PATCH_FMOD_PERIOD_STR };
    patch_words("audio/fmod-period-const",
                GAME_RVA_FMOD_PERIOD_LDR, GUARD_FMOD_PERIOD_LDR, period_mov, 1);
    patch_words("audio/fmod-period-store",
                GAME_RVA_FMOD_PERIOD_CBZ, GUARD_FMOD_PERIOD_CBZ, period_str, 1);
  }
#if BM_AUDIO_UPFRONT_BUFFERS
  {
    static const uint32_t bufcount[] = { PATCH_FMOD_BUFCOUNT };
    patch_words("audio/fmod-upfront-buffers",
                GAME_RVA_FMOD_BUFCOUNT, GUARD_FMOD_BUFCOUNT, bufcount, 1);
  }
#endif
#endif

#if 0
  /* [4a] Choreographer: force the context branch to the API-level check.
   *
   * Order within the group does not matter -- all three are inert until
   * ChoreographerBase::Get() runs -- but all three are required. Patching the
   * API level alone leaves the branch untaken; forcing the branch alone lands
   * on an API check that fails; doing both lands in ChoreographerNDK, whose
   * constructor then waits on a looper unless [4b] is applied. */
  patch_words("choreographer/force-ctx-branch",
              GAME_RVA_Choreographer_ctx_branch,
              GUARD_Choreographer_ctx_branch,
              P_CHOREO_CTX, 1);

  /* [4b] ChoreographerNDK: no looper handler, so its ctor does not wait. */
  patch_words("choreographer/ndk-no-handler",
              GAME_RVA_NdkLooper_CreateHandler_call,
              GUARD_NdkLooper_CreateHandler_call,
              P_NDK_NOHANDLER, 1);

  /* [4c] DeviceApiLevel -> 33.
   *
   * Selects the NDK Choreographer over the Java one. The Java constructor waits
   * forever for an instance only a Java Handler thread can supply, and that is
   * what parked this port inside nativeRender. Full reasoning in bm_offsets.h. */
  patch_words("android/api-level-33",
              GAME_RVA_DeviceApiLevel,
              GUARD_DeviceApiLevel,
              P_RET_API33, 2);
#endif

  /* [5] internetReachability -> NotReachable.
   *
   * Patch the DVM function rather than either of the two tail-call thunks that
   * reach it: it has 64 bytes of room, while each thunk is a single 4-byte
   * branch and writing two instructions into one would run into whatever
   * follows. Bouncemasters has a full offline mode, so this is the answer that
   * both is true here and unblocks the boot path. */
  patch_words("net/reachability-notreachable",
              GAME_RVA_DVM_GetInternetReachability,
              GUARD_DVM_GetInternetReachability,
              P_RET_ZERO, 2);

  debugPrintf("[patch] %d applied, %d skipped\n", g_applied, g_skipped);
  if (g_skipped) {
    debugPrintf("[patch] NOTE: a skipped patch is not fatal, but the run above\n"
                "        is not the configuration this port was tested against.\n");
  }
  return g_applied;
}


/* OPENBONK bring-up: apply ONLY the allocator-granularity patch. Every other
 * patch in bm_apply_patches() uses Bouncemasters-specific RVAs and some lie
 * past the end of OpenBonk's libunity mapping, so they stay off until each one
 * has been re-derived. */
int bm_apply_granularity_only(void) {
  g_applied = g_skipped = 0;
  return apply_granularity();
}
