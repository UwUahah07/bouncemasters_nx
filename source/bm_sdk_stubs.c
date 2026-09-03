/* ---------------------------------------------------------------------------
 * bm_sdk_stubs.c -- in-process replacements for the third-party native SDKs
 * that Bouncemasters ships and P/Invokes into:
 *
 *     libEOSSDK.so                      (Epic Online Services, 23 MB)
 *     libapplovin-native-crash-reporter.so + the MAX plugin's native module
 *     libcrashlytics*.so                (4 libs, SWIG-bound)
 *     libzstd-jni-1.5.7-4.so            (JNI-only; never P/Invoked)
 *     libUnityHelpers_Android.so
 *
 * WHY THIS EXISTS
 * ---------------
 * Static analysis of the shipped APK (see PORTING.md, "SDK surface") shows the
 * managed side declares P/Invokes against three native module names -- EOSSDK,
 * AppLovin and Crashlytics -- covering 298 EOS_* entry points plus the SWIG
 * Crashlytics shim. None of these modules is referenced by name from libunity.so
 * or libil2cpp.so, which means they are pulled in purely by managed
 * [DllImport], resolved through our fake dlopen()/dlsym() in libc_shim.c.
 *
 * Loading the real .so files is not an option and not merely inconvenient:
 *
 *   - libEOSSDK.so is a 23 MB networking stack that expects an Android JVM,
 *     Play Services-grade TLS trust anchors, and a live connection to Epic.
 *     On a Switch with no account and (usually) no network it can only ever
 *     fail -- but it would fail *slowly*, after blocking init and timeouts.
 *   - The AppLovin and Crashlytics libs exist to serve ads and upload crash
 *     telemetry. Neither has any business running here, and the Crashlytics
 *     handler installs its own signal handlers, which would fight the loader's
 *     crash dumper in nx_crash_handler.c.
 *
 * So the same trick firebase_stub.c uses applies: answer the managed side's
 * native lookups with trivial C stubs, and let the managed code take its own
 * "this platform doesn't have it" branch.
 *
 * HOW IT WORKS
 * ------------
 * dlsym_fake() in libc_shim.c calls bm_sdk_stub_lookup() after it has failed to
 * find a symbol in a loaded module and in the libc/GLES shim table. We match on
 * the symbol name and hand back one of three stub classes:
 *
 *   bm_stub_zero()   -> 0     for voids, bools, counts, and status ints whose
 *                             zero value is benign.
 *   bm_stub_handle() -> ptr   for anything the managed side null-checks before
 *                             use (SWIG proxy cPtr, EOS handles). Points into
 *                             one shared zeroed page; nothing ever reads it.
 *   bm_stub_fail()   -> err   for the few EOS calls where "succeeded and did
 *                             nothing" would leave the managed state machine
 *                             waiting forever on a callback that never fires.
 *
 * On AArch64 a fixed-arity C function invoked through a wider prototype is
 * harmless: surplus arguments sit unread in x2..x7 / v0..v7 and the callee
 * cleans up nothing. That is what makes one stub safe for 298 signatures.
 *
 * THE ONE THING THAT NEEDS ON-DEVICE TUNING
 * -----------------------------------------
 * EOS_Initialize's return value decides whether the managed EOSManager gives up
 * cleanly or keeps retrying. Failing it is the right default -- a hard, honest
 * "no EOS here" at the first call, before any callback is registered. If the
 * boot log shows the game retrying EOS_Initialize in a loop, or a managed
 * exception escaping from EOSManager, flip BM_EOS_INIT_RESULT to EOS_SUCCESS
 * and let the subsequent calls no-op instead. Both paths are one #define apart;
 * which one this build wants is an empirical question, not a design one.
 *
 * Every lookup is logged, so a DEBUG_LOG=1 run produces an exact ledger of the
 * SDK symbols the game asked for and the stub class each got. That log is the
 * tuning signal -- do not guess from this file, read the log.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "diag.h"
#include "bm_managed.h"
#include "so_util.h"
#include "util.h"   /* debugPrintf */

/* --------------------------------------------------------------------------
 * EOS_EResult values.
 *
 * CONFIRM THESE against eos_common.h from the EOS SDK version this APK was
 * built with before trusting the failure path. They are stable across recent
 * SDK releases, but the enum is Epic's to renumber and a wrong value here
 * turns a clean bail-out into an unrecognised-error branch.
 * ------------------------------------------------------------------------ */
#define EOS_SUCCESS            0
#define EOS_NOT_CONFIGURED    14   /* "SDK not configured for this platform" */
#define EOS_NOT_IMPLEMENTED   16
#define EOS_INVALID_PARAMETERS 10

#ifndef BM_EOS_INIT_RESULT
#define BM_EOS_INIT_RESULT EOS_NOT_CONFIGURED
#endif

/* One shared, zeroed object handed out for every non-null handle the managed
 * side asks for. It is never dereferenced by us and never written by the game
 * in any path that matters -- SWIG proxies only compare it against null. If a
 * future build starts storing through one of these, give that symbol its own
 * buffer rather than widening this one. */
static uint8_t g_bm_obj[4096] __attribute__((aligned(16)));

static long  bm_stub_zero(void)   { return 0; }
static void *bm_stub_handle(void) { return g_bm_obj; }
static long  bm_stub_fail(void)   { return BM_EOS_INIT_RESULT; }

/* EOS_Initialize / EOS_Platform_Create are the two gates. Everything downstream
 * of a failed init should never be called; if it is, it gets a zero or a handle
 * like anything else and the managed layer sees an inert SDK. */
static long bm_eos_initialize(void) {
  debugPrintf("[sdk] EOS_Initialize -> %d (stubbed; no EOS on Switch)\n",
              (int)BM_EOS_INIT_RESULT);
  return BM_EOS_INIT_RESULT;
}

static long bm_eos_shutdown(void) {
  debugPrintf("[sdk] EOS_Shutdown -> EOS_SUCCESS\n");
  return EOS_SUCCESS;
}

/* EOS_Platform_Create returns a handle, not an EOS_EResult. Returning NULL is
 * the documented "creation failed" signal and is what a managed null-check
 * expects after a failed Initialize. */
static void *bm_eos_platform_create(void) {
  debugPrintf("[sdk] EOS_Platform_Create -> NULL (stubbed)\n");
  return (void *)0;
}

/* --------------------------------------------------------------------------
 * Symbol classification
 * ------------------------------------------------------------------------ */

static int starts_with(const char *s, const char *p) {
  return strncmp(s, p, strlen(p)) == 0;
}

static int bm_is_eos_symbol(const char *s) {
  return starts_with(s, "EOS_");
}

static int bm_is_applovin_symbol(const char *s) {
  /* The MAX Unity plugin's native entry points are _Max*/ /* on the C side and
   * MaxSdk* / AppLovin* from the managed declarations. The bundled
   * libapplovin-native-crash-reporter.so exports al_* / applovin_*. */
  return starts_with(s, "_Max")      || starts_with(s, "MaxSdk")
      || starts_with(s, "AppLovin")  || starts_with(s, "applovin_")
      || starts_with(s, "al_");
}

static int bm_is_crashlytics_symbol(const char *s) {
  return starts_with(s, "CrashlyticsInternalPINVOKE")
      || starts_with(s, "Crashlytics_")
      || starts_with(s, "crashlytics_")
      || starts_with(s, "SWIGRegisterExceptionCallbacks_Crashlytics")
      || starts_with(s, "SWIGRegisterStringCallback_Crashlytics");
}

static int bm_is_misc_sdk_symbol(const char *s) {
  /* zstd-jni is reached through JNI, not P/Invoke, so it should never land
   * here -- but if a future build adds a [DllImport("zstd-jni")] we would
   * rather inert-stub it than return NULL and take a DllNotFoundException.
   * UnityHelpers_Android is a thin Playgendary-side helper; same reasoning. */
  return starts_with(s, "ZSTD_")   || starts_with(s, "Zstd")
      || starts_with(s, "UnityHelpers_");
}

/* Symbols whose managed callers null-check the result. Anything that hands back
 * an opaque pointer, a struct-by-pointer, or a SWIG cPtr belongs here; a zero
 * would trip an ArgumentNullException inside the SDK's own proxy layer before
 * the game ever sees it. */
static int bm_returns_handle(const char *s) {
  return strstr(s, "_Copy")      != NULL   /* EOS_*_Copy*  -> out-param + handle */
      || strstr(s, "_Create")    != NULL
      || strstr(s, "_Get")       != NULL   /* interface getters: EOS_Platform_GetAuthInterface etc. */
      || strstr(s, "_Parse")     != NULL
      || strstr(s, "FromString") != NULL
      || strstr(s, "_new_")      != NULL   /* SWIG constructors */
      || starts_with(s, "new_");
}

/* --------------------------------------------------------------------------
 * The resolver libc_shim.c calls
 * ------------------------------------------------------------------------ */

void *bm_sdk_stub_lookup(const char *symbol) {
  if (!symbol) return (void *)0;

  if (bm_is_eos_symbol(symbol)) {
    /* Exact-match the handful of calls whose return value steers the managed
     * state machine, before falling through to the generic classes. */
    if (!strcmp(symbol, "EOS_Initialize")) {
      return (void *)&bm_eos_initialize;
    }
    if (!strcmp(symbol, "EOS_Shutdown")) {
      return (void *)&bm_eos_shutdown;
    }
    if (!strcmp(symbol, "EOS_Platform_Create")) {
      return (void *)&bm_eos_platform_create;
    }
    /* EOS_*_AddNotify* hands back a notification id the game later passes to
     * the matching RemoveNotify. Zero is the documented invalid id and the
     * remove path tolerates it. */
    if (strstr(symbol, "AddNotify")) {
      debugPrintf("[sdk] dlsym(%s) -> zero [eos/notify]\n", symbol);
      return (void *)&bm_stub_zero;
    }
    if (bm_returns_handle(symbol)) {
      debugPrintf("[sdk] dlsym(%s) -> handle [eos]\n", symbol);
      return (void *)&bm_stub_handle;
    }
    debugPrintf("[sdk] dlsym(%s) -> zero [eos]\n", symbol);
    return (void *)&bm_stub_zero;
  }

  if (bm_is_applovin_symbol(symbol)) {
    if (bm_returns_handle(symbol)) {
      debugPrintf("[sdk] dlsym(%s) -> handle [applovin]\n", symbol);
      return (void *)&bm_stub_handle;
    }
    debugPrintf("[sdk] dlsym(%s) -> zero [applovin]\n", symbol);
    return (void *)&bm_stub_zero;
  }

  if (bm_is_crashlytics_symbol(symbol)) {
    /* Never hand Crashlytics a live handle: its managed layer would then try to
     * register native signal handlers over ours. Zero everywhere. */
    debugPrintf("[sdk] dlsym(%s) -> zero [crashlytics]\n", symbol);
    return (void *)&bm_stub_zero;
  }

  if (bm_is_misc_sdk_symbol(symbol)) {
    debugPrintf("[sdk] dlsym(%s) -> zero [misc-sdk]\n", symbol);
    return (void *)&bm_stub_zero;
  }

  /* Not ours -- let dlsym_fake continue to the EGL/GLES fallback. */
  return (void *)0;
}

/* Crashlytics SWIG callback registration.
 *
 * These are the ONLY two symbols in the whole boot that resolve to zero:
 *
 *   [sdk] dlsym(SWIGRegisterStringCallback_CrashlyticsInternal)    -> zero
 *   [sdk] dlsym(SWIGRegisterExceptionCallbacks_CrashlyticsInternal) -> zero
 *
 * They take delegate pointers for the native side to store, and the boot does
 * get past them ("Registering Crashlytics exception handlers" is followed by the
 * Config work), so they are not the current blocker. But a zero here is the same
 * shape as the Firebase Future bug in sec.30: a registration that silently does
 * nothing. Returning a real no-op keeps the C# side from ever seeing a null
 * function pointer where it expects a callable, and costs nothing.
 *
 * Deliberately NOT invoking the callbacks: unlike a Future completion, nothing
 * awaits these. They are for reporting crashes we do not intend to report. */
static void bm_crashlytics_register_noop(void *a, void *b, void *c) {
  (void)a; (void)b; (void)c;
}

/* ---------------------------------------------------------------------------
 * Cut EOS off at the managed boundary instead of the native one
 *
 * The native stubs above are honest: EOS_Initialize reports NotConfigured,
 * because there is no EOS here and never will be. The managed side treats that
 * as fatal and throws:
 *
 *     Exception: Epic Online Services didn't init correctly: NotConfigured
 *       at EOSManager+EOSSingleton.Init (IEOSCoroutineOwner, String)
 *
 * On the first boot that got this far, that exception was the last managed
 * activity in the log -- one frame rendered, then nothing. An unhandled
 * exception kills the coroutine that threw it, and the boot sequence was in it.
 *
 * The knob in config.h anticipated this and offered EOS_SUCCESS instead. That
 * is the worse of the two options: EOS_Platform_Create would then have to
 * return a live-looking handle, EOSManager would believe it had a working
 * backend, and the next thing to break would be a login callback that can never
 * fire -- a hang rather than an exception, and a harder one to read.
 *
 * Neutralise Init instead. It returns void, so a hook that returns immediately
 * IS the whole method: EOS never initialises, nothing throws, and the caller
 * carries on. Nothing downstream can be waiting on a backend that was never
 * started.
 *
 * The native stubs stay exactly as they are. They are now unreachable on the
 * normal path, and they remain correct for anything that reaches EOS by another
 * route -- and for the day this hook's guard stops matching.
 * ------------------------------------------------------------------------ */

static void bm_eos_init_noop(void) { }

int bm_eos_managed_install(so_module *il2cpp) {
  static const uint32_t g[3] = BMM_G3_EOSManager_Init;
  int n = bm_managed_hook(il2cpp, "EOSManager.EOSSingleton.Init",
                          BMM_RVA_EOSManager_Init, g, (void *)&bm_eos_init_noop);
  debugPrintf("[sdk] EOS managed init %s\n",
              n ? "neutralised (no EOS, no exception)"
                : "NOT neutralised -- expect the NotConfigured exception and a "
                  "dead boot coroutine");
  return n;
}

/* ---------------------------------------------------------------------------
 * AppLovin MAX: report initialised, and never try to initialise
 *
 * With Firebase unblocked the boot reached:
 *
 *     Config: ADS_MODE = False
 *     MaxAds: COnfig changed. Check AdsMode
 *     MaxAds: Not initialized yet
 *
 * and stopped. The game polls MaxSdk.IsInitialized() and waits. It never turns
 * true: MaxSdkAndroid.InitializeSdk() calls through an AndroidJavaClass
 * (MaxUnityPluginClass) that does not exist here, and the SDK signals
 * completion via OnSdkInitializedEvent delivered from Java. Exactly the shape
 * of the Firebase Future -- a callback that cannot arrive.
 *
 * Two hooks:
 *
 *   IsInitialized() -> true    unblocks the poll.
 *   InitializeSdk() -> no-op   stops the attempt entirely, so nothing reaches
 *                              the missing Java class and no half-initialised
 *                              state is left behind for a later call to trip
 *                              over.
 *
 * Reporting "initialised" for an SDK that is not running is safe here because
 * ADS_MODE is False in this build's remote config -- the game has already
 * decided not to show ads. Every ad-showing entry point is stubbed inert by the
 * dlsym table above, so a call that does slip through returns zero rather than
 * faulting.
 * ------------------------------------------------------------------------ */

static int32_t bm_max_isInitialized(void) { return 1; }
static void    bm_max_initializeSdk(void) { }

int bm_maxads_install(so_module *il2cpp) {
  static const uint32_t g_isinit[3] = BMM_G3_MaxSdkAndroid_IsInitialized;
  static const uint32_t g_init  [3] = BMM_G3_MaxSdkAndroid_InitializeSdk;

  int n = 0;
  n += bm_managed_hook(il2cpp, "MaxSdkAndroid.IsInitialized",
                       BMM_RVA_MaxSdkAndroid_IsInitialized, g_isinit,
                       (void *)&bm_max_isInitialized);
  n += bm_managed_hook(il2cpp, "MaxSdkAndroid.InitializeSdk",
                       BMM_RVA_MaxSdkAndroid_InitializeSdk, g_init,
                       (void *)&bm_max_initializeSdk);

  debugPrintf("[sdk] MaxAds %s\n",
              n == 2 ? "reports initialised (SDK never started)"
                     : "NOT fully neutralised -- the boot will wait on "
                       "\"MaxAds: Not initialized yet\"");
  return n;
}

/* ---------------------------------------------------------------------------
 * Epic login check: declare it complete
 *
 * With MaxAds cleared the boot reached "MaxAds: Ads disabled" and went silent --
 * frames rendering, engine clock advancing, and NOTHING calling into any shim.
 * That absence was the clue: a pure managed wait, not a stalled native call.
 *
 * EpicLoginCheckLoadingStep awaits UniTask.WaitUntil(() =>
 * persistentCheckComplete), and only the EOS login callback sets that flag. EOS
 * is deliberately absent, so the flag never becomes true.
 *
 * Forcing the predicate to return true says "the Epic login check has finished",
 * which is honest: there is no Epic account here and never will be, so the check
 * is trivially complete. The flag is private to this step and read nowhere else,
 * so nothing downstream is misled.
 * ------------------------------------------------------------------------ */
int bm_epiclogin_install(so_module *il2cpp) {
  /* [1] The predicate patch from sec.33. Kept: it is verified safe and one of
   * the step's two WaitUntil calls does use it. On its own it was not enough --
   * the step tracer showed EpicLoginCheckLoadingStep still entering and never
   * completing with this applied. */
  static const uint32_t g[2] = BMM_G2_EpicLoginCheck_pred;
  int n = bm_managed_patch(il2cpp, "EpicLoginCheck.persistentCheckComplete",
                           BMM_RVA_EpicLoginCheck_pred, g, 2,
                           BMM_PATCH_EpicLoginCheck_pred);

  /* [2] Skip the step outright by returning a completed UniTask.
   *
   * Its state machine awaits TWO WaitUntil predicates around
   * EpicSDKManager.CheckPersistentLogin, and only one of them is the flag above.
   * Chasing the second is pointless work: with EOS neutralised there is no
   * persistent login to check and nothing downstream consumes the result.
   *
   * default(UniTask) -- a null source -- IS UniTask.CompletedTask, and a
   * 16-byte struct returns in x0:x1. So `mov x0,#0 ; mov x1,#0 ; ret` is a
   * complete, correct implementation of "this step is already done". */
  static const uint32_t g2[3]   = BMM_G3_EpicLoginCheck_LoadStepInternal;
  static const uint32_t body[3] = { 0xd2800000u,   /* mov x0, #0  (source) */
                                    0xd2800001u,   /* mov x1, #0  (token)  */
                                    0xd65f03c0u }; /* ret                  */
  int m = bm_managed_patch_n(il2cpp,
                             "EpicLoginCheck.LoadStepInternal -> CompletedTask",
                             BMM_RVA_EpicLoginCheck_LoadStepInternal,
                             g2, body, 3);

#if BM_FORCE_OFFLINE
  /* [3a] Force the login SCREEN to take its own offline path on frame one, so
   * it is never shown long enough to press anything. See bm_managed.h. */
  {
    static const uint32_t g1[1] = BMM_G1_EpicLoginScreen_needHide;
    static const uint32_t b1[1] = { BMM_PATCH_EpicLoginScreen_needHide };
    int o = bm_managed_patch_n(il2cpp, "EpicLoginScreen -> always Play Offline",
                               BMM_RVA_EpicLoginScreen_needHide, g1, b1, 1);
    debugPrintf("[sdk] Epic login screen: %s\n",
                o ? "auto-continues offline (never waits for input)"
                  : "NOT forced -- the screen will be shown");
  }
#endif

  /* [3b] The "Log in with Epic Games" button. Without this it softlocks: the
   * popup waits on a callback EOS will never deliver, and the game's own
   * callback only dismisses on success anyway. Make it the Offline button. */
  static const uint32_t g3[3] = BMM_G3_EpicLoginPopup_Login;
  static const uint32_t b3[3] = BMM_BODY_EpicLoginPopup_Login;
  int p = bm_managed_patch_n(il2cpp,
                             "EpicLoginPopup.LoginButton -> Offline behaviour",
                             BMM_RVA_EpicLoginPopup_Login, g3, b3, 3);
  debugPrintf("[sdk] Epic login button: %s\n",
              p ? "dismisses the popup (no EOS; behaves as Play Offline)"
                : "NOT patched -- pressing it will softlock");

  debugPrintf("[sdk] Epic login check: predicate %s, step %s\n",
              n ? "forced" : "NOT forced",
              m ? "SKIPPED (returns CompletedTask; no EOS to check)"
                : "NOT skipped -- the boot will wait at EpicLoginCheckLoadingStep");
  return n && m;
}

/* Unused-warning suppression for the fail stub: it is referenced only when
 * BM_EOS_INIT_RESULT tuning moves a symbol onto it. Keep it reachable. */
void *bm_sdk_stub_fail_ref(void) { return (void *)&bm_stub_fail; }
