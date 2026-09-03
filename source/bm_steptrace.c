/* ---------------------------------------------------------------------------
 * bm_steptrace.c -- log which GameLoadingStep is ENTERED.
 *
 * THE MISSING PIECE
 * -----------------
 * The bootstrap logs `<Step> successfully loaded` when a step FINISHES and
 * nothing when it starts. A step that begins and never returns is therefore
 * completely invisible -- which is exactly the shape of this port's remaining
 * hang. Five steps report done, six never appear, and nothing in the log says
 * whether the sixth started or was never reached. Every diagnosis so far has
 * had to guess across that gap.
 *
 * bm_managed_hook() cannot help: it REPLACES a method. To observe one we have
 * to call it afterwards, and that needs a detour.
 *
 * WHY NO RUNTIME TRAMPOLINE IS NEEDED
 * -----------------------------------
 * The usual detour allocates executable memory, copies the overwritten
 * instructions into it, and appends a branch back. On Switch that means
 * svcMapProcessCodeMemory and a code-memory reservation -- real work, and a new
 * failure mode.
 *
 * Not required here. GameLoadingStep.LoadStep begins:
 *
 *     sub  sp, sp, #0x70          d101c3ff
 *     str  x30, [sp, #0x40]       f90023fe
 *     stp  x22, x21, [sp, #0x50]  a90557f6
 *     stp  x20, x19, [sp, #0x60]  a9064ff4
 *
 * All four are position-INDEPENDENT -- no adrp, adr, b, bl or literal load. (The
 * first PC-relative instruction is the fifth, `adrp x22`.) So the prologue can
 * simply be re-emitted inside our own .text, which is already executable, and
 * the detour branches to LoadStep+0x10 to continue. No allocation, no
 * relocation, nothing to fail at runtime.
 *
 * That safety is specific to this method's first four words, and the installer
 * verifies them before hooking. If a game update changes the prologue the guard
 * fails and the trace is skipped rather than corrupting the method.
 *
 * IL2CPP CALLING CONVENTION
 *     UniTask LoadStep(GameLoadingStep this, Action<GameLoadingStep> cb,
 *                      const MethodInfo *)
 *     x0 = this, x1 = cb, x2 = MethodInfo; a 16-byte UniTask returns in x0:x1.
 * The detour preserves all of them across the log call.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <switch.h>
#include <string.h>

#include "bm_managed.h"
#include "config.h"
#include "diag.h"
#include "so_util.h"
#include "util.h"

/* LoadStep + 0x10: where the detour resumes after replaying the prologue. */
void *g_bm_loadstep_next;

/* Il2CppClass::name.
 *
 * Layout for this IL2CPP generation:
 *     0x00  const Il2CppImage *image
 *     0x08  void              *gc_desc
 *     0x10  const char        *name
 *
 * Every object's first word is its Il2CppClass*, so the type name of a managed
 * instance is two dereferences away. Both are validated before use -- a wrong
 * offset here would fault inside a logging call, which would be a poor way to
 * lose a debugging session. */
#define IL2CPP_CLASS_NAME_OFF 0x10

/* so_find_module_by_addr() only knows the three MAPPED MODULES, so it rejects
 * every runtime allocation -- which is where an Il2CppClass and its name string
 * both live. That is why the first run reported "(bad klass)" six times: the
 * pointers were fine, the test was wrong.
 *
 * Ask the kernel instead. svcQueryMemory answers for any address in the process,
 * which is exactly the question being asked: is this safe to dereference. */
static int readable(const void *p) {
  if (!p || (uintptr_t)p < 0x1000) return 0;
  MemoryInfo mi; u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)p))) return 0;
  if (mi.type == MemType_Unmapped || mi.type == MemType_Reserved) return 0;
  return (mi.perm & Perm_R) != 0;
}

static const char *managed_type_name(void *obj) {
  if (!obj) return "(null)";
  void *klass = *(void **)obj;
  if (!readable(klass)) return "(bad klass)";
  const char *nm = *(const char **)((char *)klass + IL2CPP_CLASS_NAME_OFF);
  if (!readable(nm)) return "(bad name)";
  /* Must look like an identifier before it is printed as one. */
  for (int i = 0; i < 64; i++) {
    unsigned char c = (unsigned char)nm[i];
    if (c == 0) return i ? nm : "(empty)";
    if (c < 0x20 || c > 0x7e) return "(not a string)";
  }
  return "(unterminated)";
}

/* Called from the detour with `this` still in x0.
 *
 * `used` because the ONLY reference is a `bl` inside inline assembly, which the
 * compiler cannot see. With -ffunction-sections and --gc-sections -- which this
 * Makefile passes -- the linker would otherwise be free to discard it, and the
 * detour would branch into nothing. */
__attribute__((used)) void bm_loadstep_log(void *thiz) {
  static unsigned n;
  const char *nm = managed_type_name(thiz);
  void *klass = (thiz && readable(thiz)) ? *(void **)thiz : NULL;

  /* Print the class pointer as an il2cpp-relative offset even when the name
   * reads back. A name can be resolved offline against dump.cs if it is ever
   * wrong or missing, and an offset costs one field. */
  extern so_module il2cpp_mod;
  unsigned long long krva = 0;
  if (klass && il2cpp_mod.load_virtbase)
    krva = (unsigned long long)((uintptr_t)klass -
                                (uintptr_t)il2cpp_mod.load_virtbase);

  debugPrintf("[step] ENTER #%u  %-40s (klass=%p il2cpp+0x%llx)\n",
              ++n, nm, klass, krva);
}

/* The detour itself.
 *
 * naked: the compiler must not emit a prologue, because the whole point is that
 * this function reproduces LoadStep's own prologue byte-for-byte and then falls
 * into the middle of it. */
__attribute__((used, naked)) static void bm_loadstep_detour(void) {
  __asm__ volatile(
      /* Preserve the incoming arguments and the caller's x30 across the log. */
      "stp  x29, x30, [sp, #-16]!   \n"
      "stp  x0,  x1,  [sp, #-16]!   \n"
      "stp  x2,  x3,  [sp, #-16]!   \n"
      "bl   bm_loadstep_log         \n"   /* x0 already holds `this` */
      "ldp  x2,  x3,  [sp], #16     \n"
      "ldp  x0,  x1,  [sp], #16     \n"
      "ldp  x29, x30, [sp], #16     \n"

      /* Replay LoadStep's original first four instructions verbatim. */
      "sub  sp, sp, #0x70           \n"
      "str  x30, [sp, #0x40]        \n"
      "stp  x22, x21, [sp, #0x50]   \n"
      "stp  x20, x19, [sp, #0x60]   \n"

      /* Continue at LoadStep + 0x10. */
      "adrp x16, g_bm_loadstep_next \n"
      "add  x16, x16, :lo12:g_bm_loadstep_next \n"
      "ldr  x16, [x16]              \n"
      "br   x16                     \n");
}

/* ---------------------------------------------------------------------------
 * RemoteConfigService.Bind -- entry AND exit
 *
 * The step tracer narrows the hang to step #6, and the log ordering identifies
 * it as RemoteConfigLoadingStep: ENTER #6 is immediately followed by the
 * Crashlytics registration and the whole Config sequence. Its body is short:
 *
 *     RemoteConfigService.Instance()
 *     RemoteConfigService.Bind(...)
 *     await UniTask.NextFrame()
 *
 * and ShowLoaderScreenLoadingStep awaits the same UniTask.NextFrame and
 * completes, so the await mechanism is not the problem. That leaves a throw
 * inside the method -- which UniTask stores in the task rather than raising, so
 * it is completely silent.
 *
 * A TRAMPOLINE, NOT JUST A DETOUR. The LoadStep detour above only reports entry,
 * because it branches into the middle of the original and never comes back to
 * us. Here the replay is written as a *callable* function instead: it re-emits
 * Bind's prologue and branches to Bind+0x10, and because that prologue saves x30
 * on entry, Bind's own epilogue returns to whoever called the trampoline. So a
 * plain C wrapper can log, call through, and log again.
 *
 * ENTER with no EXIT means Bind threw -- which is the hypothesis, stated so it
 * can be falsified in one boot rather than reasoned about for another round.
 *
 * Bind's first four instructions, all position-independent:
 *     str x30, [sp, #-0x20]!    f81e0ffe
 *     stp x20, x19, [sp, #0x10] a9014ff4
 *     mov x19, x1               aa0103f3
 *     mov x20, x0               aa0003f4
 * ------------------------------------------------------------------------- */

void *g_bm_bind_next;                       /* Bind + 0x10 */

__attribute__((used, naked))
static void bm_bind_trampoline(void *thiz, void *arr, void *mi) {
  (void)thiz; (void)arr; (void)mi;
  __asm__ volatile(
      "str  x30, [sp, #-0x20]!      \n"
      "stp  x20, x19, [sp, #0x10]   \n"
      "mov  x19, x1                 \n"
      "mov  x20, x0                 \n"
      "adrp x16, g_bm_bind_next     \n"
      "add  x16, x16, :lo12:g_bm_bind_next \n"
      "ldr  x16, [x16]              \n"
      "br   x16                     \n");
}

static void bm_bind_detour(void *thiz, void *arr, void *mi) {
  debugPrintf("[step] RemoteConfigService.Bind ENTER (this=%p arr=%p)\n", thiz, arr);
  bm_bind_trampoline(thiz, arr, mi);
  debugPrintf("[step] RemoteConfigService.Bind EXIT ok\n");
}

static int install_bind_trace(so_module *il2cpp) {
#if !BM_TRACE_REMOTECONFIG_BIND
  (void)il2cpp;
  return 0;
#else
  uintptr_t addr = (uintptr_t)il2cpp->load_virtbase + BMM_RVA_RemoteConfigService_Bind;
  static const uint32_t guard[4] = BMM_G4_RemoteConfigService_Bind;
  const uint32_t *live = (const uint32_t *)addr;
  for (int i = 0; i < 4; i++) {
    if (live[i] != guard[i]) {
      debugPrintf("[step] Bind trace SKIPPED: prologue word %d differs "
                  "(expected 0x%08x, found 0x%08x)\n", i, guard[i], live[i]);
      return 0;
    }
  }
  g_bm_bind_next = (void *)(addr + 0x10);
  hook_arm64(addr, (uintptr_t)&bm_bind_detour);
  debugPrintf("[step] tracing RemoteConfigService.Bind entry/exit\n");
  return 1;
#endif
}

int bm_steptrace_install(so_module *il2cpp) {
#if !BM_TRACE_LOADING_STEPS
  (void)il2cpp;
  return 0;
#else
  if (!il2cpp || !il2cpp->load_virtbase) return 0;

  uintptr_t addr = (uintptr_t)il2cpp->load_virtbase + BMM_RVA_GameLoadingStep_LoadStep;
  static const uint32_t guard[4] = BMM_G4_GameLoadingStep_LoadStep;
  const uint32_t *live = (const uint32_t *)addr;

  for (int i = 0; i < 4; i++) {
    if (live[i] != guard[i]) {
      debugPrintf("[step] trace SKIPPED: prologue word %d differs "
                  "(expected 0x%08x, found 0x%08x).\n"
                  "       The detour replays these four instructions verbatim, "
                  "so it cannot be installed over a different prologue.\n",
                  i, guard[i], live[i]);
      return 0;
    }
  }

  g_bm_loadstep_next = (void *)(addr + 0x10);
  hook_arm64(addr, (uintptr_t)&bm_loadstep_detour);
  debugPrintf("[step] tracing GameLoadingStep.LoadStep entries "
              "(il2cpp+0x%x, resume +0x10)\n",
              (unsigned)BMM_RVA_GameLoadingStep_LoadStep);
  install_bind_trace(il2cpp);
  return 1;
#endif
}
