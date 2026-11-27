/* ---------------------------------------------------------------------------
 * bm_offsets.h -- addresses inside the game's own libunity.so.
 *
 * VERIFIED AGAINST A MATCHING SYMBOL FILE. Every address below was read from
 * libunity_sym.so and confirmed by disassembly, not inferred from a pattern.
 *
 *   libunity.so      26,914,664 bytes
 *   libunity_sym.so  12,585,544 bytes, not stripped, 91,020 FUNC symbols
 *   Build ID         02e993102d7c6d21c27fb5195ba94aa5958f1ae3   (BOTH files)
 *   Unity            6000.0.58f2 (92dee566b325)
 *
 * The Build ID is the identity that matters. The symbol file is authoritative
 * only for the binary it was built alongside; before trusting any of this after
 * a game update, run:
 *
 *     readelf -n libunity.so | grep 'Build ID'
 *
 * If it differs from the value above, every constant here is void. Re-derive
 * with tools/symbols.py (if you have a matching sym file) or with
 * tools/find_offsets.py (if you do not).
 *
 * STILL DO NOT COPY THESE FROM ANOTHER PORT. Bouncemasters ships the same Unity
 * version -- 6000.0.58f2 -- that laytonbmr_nx documented
 * OFF_ANDROID_VSYNC_COUNTER = 0x1243158 for. In this binary 0x1243158 is inside
 * .text. Same engine version, different respin, different link order, different
 * answer. Trusting it would have written a frame counter over an instruction.
 * ------------------------------------------------------------------------- */

#ifndef BM_OFFSETS_H
#define BM_OFFSETS_H

/* ===========================================================================
 * ANDROID VSYNC
 *
 * Unity's Android player blocks until a Choreographer callback bumps a frame
 * counter. Choreographer is an Android framework service that does not exist
 * here, so nothing ever bumps the counter and the engine waits forever. This is
 * the black-screen-after-splash hang.
 *
 * The whole path, from the symbol file:
 *
 *   AndroidVSync::WaitForLastPresentationAndGetTimestamp()  0x00a25820  (460 b)
 *     +0x104 -> GetVSyncInfo()                              0x00a44eb8  (132 b)
 *     +0x15c -> WaitVSync(long)                             0x00a44e00   (96 b)
 *
 *   GetWantedVSyncCount()                                   0x0086c7f0   (52 b)
 *     +0x004 -> ShouldForceDisableVSync()                   0x00a31e2c   (40 b)
 *
 * Each of those three callees has EXACTLY ONE caller, which is what makes
 * patching any of them safe -- there is no second path to reason about.
 * ========================================================================= */

/* WaitVSync(long) disassembles to precisely the loop the structural matcher in
 * tools/find_offsets.py is written to recognise:
 *
 *   0x00a44e00  str   x30, [sp, #-0x30]!
 *   0x00a44e0c  adrp  x20, #0x1a33000
 *   0x00a44e10  add   x20, x20, #0xc0        ; x20 = &s_VsyncMonitor
 *   0x00a44e18  mov   x0, x20
 *   0x00a44e1c  bl    #0x1920f70             ; pthread_mutex_lock@plt
 *   0x00a44e20  adrp  x22, #0x1a33000
 *   0x00a44e24  ldr   x21, [x22, #0x118]     ; x21 = s_FrameCounter   <-- loop head
 *   0x00a44e28  cmp   x21, x19
 *   0x00a44e2c  b.ge  #0xa44e40              ; reached the target frame?
 *   0x00a44e30  add   x0, x20, #0x28         ; &s_VsyncMonitor.cond
 *   0x00a44e34  mov   x1, x20
 *   0x00a44e38  bl    #0x1920f80             ; pthread_cond_wait@plt
 *   0x00a44e3c  b     #0xa44e24              ; back to the ldr
 *   0x00a44e40  ...   mutex_unlock, return x21
 */
#define GAME_RVA_WaitVSync          0x0090d508u   /* function entry (OPENBONK)  */
#define GUARD_WaitVSync             0xf81d0ffeu   /* str x30, [sp, #-0x30]!     */

#define GAME_RVA_WaitVSync_loop     0x0090d52cu   /* the ldr the loop re-enters */
#define GUARD_WaitVSync_loop        0xf9452ed5u   /* ldr x21, [x22, #0xa58]     */

/* s_VsyncMonitor: pthread_mutex + pthread_cond, cond at +0x28. */
/* SetFrameTimeNanos(long ns) -- the engine's OWN frame-clock entry point.
 *
 * It does exactly what a vsync pump must, and does it correctly:
 *
 *     lock(s_VsyncMonitor)
 *       s_PrevFrameTime = s_LastFrameTime
 *       s_LastFrameTime = ns
 *       s_FrameCounter += 1
 *     unlock
 *     tail-call pthread_cond_broadcast(&s_VsyncMonitor.cond)
 *
 * Calling it beats reproducing it: the field ordering, the lock and the
 * broadcast are the engine's own, so they cannot drift from what GetVSyncInfo
 * expects. It also confirms the field semantics inferred in sec.16 were right --
 * Prev really is the previous Last, and the counter really is bumped here.
 */
#define GAME_RVA_SetFrameTimeNanos  0x0090d568u
#define GUARD_SetFrameTimeNanos     0xf81e0ffeu   /* str x30, [sp, #-0x20]! */

#define GAME_RVA_VSYNC_MONITOR      0x017c6a00u   /* .bss, 88 bytes             */
#define GAME_RVA_VSYNC_COND         0x017c6a28u   /* MONITOR + 0x28             */

/* s_FrameCounter, the value WaitVSync spins on. Symbol-confirmed at offset +0
 * of an 8-byte OBJECT in .bss -- exactly what a uint64 counter should be. */
#define GAME_RVA_VSYNC_COUNTER      0x017c6a58u   /* .bss, uint64               */

/* ---------------------------------------------------------------------------
 * THE VSYNC STATE BLOCK -- all four fields symbol-named
 *
 * s_FrameCounter is the first field of a four-field block, not a lone counter.
 * GetVSyncInfo() (0x00a44eb8) reads all four under the monitor lock:
 *
 *   +0x00  0x01a33118  uint64  s_FrameCounter    -- what WaitVSync compares
 *   +0x08  0x01a33120  uint64  s_LastFrameTime   -- NANOSECONDS
 *   +0x10  0x01a33128  uint64  s_PrevFrameTime   -- NANOSECONDS
 *   +0x18  0x01a33130  double  s_NanoTimeOffset  -- seconds, added to the base
 *
 * and computes, with the 1e-09 ns-to-seconds constant from .rodata:
 *
 *   out[0x00] = s_LastFrameTime * 1e-9 + s_NanoTimeOffset   <- absolute time, s
 *   out[0x08] = s_FrameCounter                              <- the counter
 *   out[0x10] = (s_LastFrameTime - s_PrevFrameTime) * 1e-9  <- interval, s
 *
 * THE UNITS MATTER. These are nanosecond wall-clock timestamps, not vsync
 * sequence numbers. Incrementing them by 1 per frame -- the obvious thing if
 * you read the block as a counter triple -- yields a frame interval of one
 * nanosecond and an engine clock that never advances meaningfully. Write real
 * monotonic nanoseconds. See bm_vsync.c.
 *
 * The .rodata constant at 0x00147d78 is 1e-09 exactly. It is a literal-pool
 * entry with no covering symbol, which is why tools/symbols.py reports it as
 * OUT -- that is expected for an anonymous constant and not a problem. It is
 * recorded here for reference only; nothing needs to read it at runtime.
 * ------------------------------------------------------------------------ */
#define GAME_RVA_LAST_FRAME_TIME    0x017c6a60u   /* .bss, uint64, nanoseconds  */
#define GAME_RVA_PREV_FRAME_TIME    0x017c6a68u   /* .bss, uint64, nanoseconds  */
#define GAME_RVA_NANO_TIME_OFFSET   0x017c6a70u   /* .bss, double, seconds      */

/* AndroidVSync::s_LastVsyncCounter -- the target frame the engine last asked
 * for. WaitForLastPresentationAndGetTimestamp() computes the next target as
 * s_LastVsyncCounter + N and writes the result back here every frame. */
#define GAME_RVA_LAST_VSYNC_COUNTER 0x01a2dd90u   /* .bss, uint64               */
#define GAME_RVA_LAST_TIMESTAMP     0x0199f968u   /* .bss, AndroidVSync::s_LastTimestamp */

/* ShouldForceDisableVSync() returns 1 when the graphics-device object is null.
 * GetWantedVSyncCount() tests bit 0 of that result and returns 0 when set. */
#define GAME_RVA_ShouldForceDisableVSync  0x00a31e2cu
#define GUARD_ShouldForceDisableVSync     0xf81f0ffeu  /* str x30, [sp,#-0x10]! */

#define VSYNC_PERIOD_NS             16666667ull   /* 60 Hz, docked and handheld */

/* ---------------------------------------------------------------------------
 * STRATEGY: PUMP THE COUNTER, DO NOT PATCH THE WAIT
 *
 * The obvious fix -- overwrite WaitVSync's first instruction with a bare RET --
 * is wrong, and wrong in a way that is expensive to diagnose. It is worth
 * understanding why before reaching for it.
 *
 * WaitVSync takes the target frame in x0 and returns the observed counter in
 * x0, so a bare RET does hand back "we got there" and the engine does stop
 * blocking. But the caller never uses that return value. Look at
 * WaitForLastPresentationAndGetTimestamp() around 0x00a25980:
 *
 *   x20      = s_LastVsyncCounter + N        ; the target, grows every frame
 *   [sp+8]   = s_FrameCounter (via GetVSyncInfo)
 *   bl       WaitVSync(x20)
 *   str      x20, [x19, #0xd90]              ; s_LastVsyncCounter = x20
 *   sub      x8, x20, [sp+8]                 ; delta = target - observed
 *   timestamp = base + delta * period
 *
 * The frame timestamp is derived from the DIFFERENCE between the target and the
 * real counter. With WaitVSync neutered, the target keeps advancing every frame
 * while s_FrameCounter stays frozen at its initial value, so that difference
 * grows without bound and the engine clock accelerates away. Time.deltaTime
 * becomes enormous and climbing. Physics, tweens and timers all break, and the
 * symptom -- a black or badly misbehaving screen -- looks close enough to the
 * original hang that you will suspect the patch did not apply.
 *
 * killerbean_nx documents the sibling failure from the other direction: leave
 * the counter frozen and the managed clock freezes, deltaTime goes to 0, the
 * boot logo's DOTween fade never completes, and boot never starts.
 *
 * So: advance the counter instead. Once per rendered frame, from the loader's
 * own present loop, under the monitor lock:
 *
 *     lock(s_VsyncMonitor)
 *       s_FrameCounter += 1
 *       seq_prev = seq;  seq += 1
 *       broadcast(s_VsyncMonitor.cond)
 *     unlock(s_VsyncMonitor)
 *
 * That keeps target and observed within one frame of each other, so delta stays
 * ~1 and the timestamp advances by ~one period per frame, which is the truth.
 * WaitVSync then returns on its own and needs no patch at all. See bm_vsync.c.
 *
 * The broadcast is not optional. Bumping the counter without signalling the
 * condition variable leaves the engine parked in pthread_cond_wait, which is
 * indistinguishable from the original hang.
 *
 * Companion patch (optional, applied second):
 *
 *     write32(base + GAME_RVA_ShouldForceDisableVSync + 0, 0x52800020); // mov w0,#1
 *     write32(base + GAME_RVA_ShouldForceDisableVSync + 4, 0xd65f03c0); // ret
 *
 * That makes GetWantedVSyncCount() return 0, so QualitySettings reports vsync
 * as off and the engine stops budgeting frame time for a wait it is no longer
 * doing. It is NOT sufficient on its own: the wait path in
 * AndroidVSync::WaitForLastPresentationAndGetTimestamp() never calls
 * ShouldForceDisableVSync(); it inlines the same test as a virtual call through
 * vtable slot +0x18 at 0x00a2583c..0x00a25854. Patch WaitVSync as well.
 *
 * GUARD DISCIPLINE. Read the 4 bytes at each RVA and compare against the GUARD_
 * value BEFORE writing. On mismatch: log loudly and skip that patch. Do not
 * patch anyway, and do not abort. Skipping costs frame pacing -- visible,
 * survivable, and it points straight at "re-derive the offsets". Patching a
 * shifted binary corrupts one instruction somewhere in 18.6 MB of .text and
 * surfaces as a crash with no relationship to its cause.
 * ------------------------------------------------------------------------ */

/* ===========================================================================
 * OTHER HOOK POINTS
 *
 * Not needed for first boot, but these are the addresses you reach for next,
 * and they are cheap to record while a matching symbol file is in hand. Sizes
 * are from the symbol table; all are FUNC.
 * ========================================================================= */

/* Internet reachability ---------------------------------------------------
 *
 * The managed getter resolves through two tail-call thunks into a JNI call:
 *
 *   Application_Get_Custom_PropInternetReachability  0x00681b60  b -> 0x00a6de68
 *     GetInternetReachability()                      0x00a6de68  b -> 0x00a283b4
 *       DVM::GetInternetReachability()               0x00a283b4  (64 b, JNI:
 *                                                     getNetworkConnectivity)
 *
 * Bouncemasters has a full offline path -- offline earnings, an offline popup,
 * NoInternetConnection handling, an InternetReachabilityWaiter -- so the honest
 * answer is also the one that unblocks boot. Report NotReachable(0) and the
 * game takes a route it was designed to take, rather than waiting on HTTP that
 * will never complete.
 *
 * Patch the DVM function, not either thunk: it has 64 bytes of room, while the
 * thunks are 4 bytes each and writing two instructions into one would spill
 * into whatever follows.
 *
 *     write32(base + GAME_RVA_DVM_GetInternetReachability + 0, 0x52800000); // mov w0,#0
 *     write32(base + GAME_RVA_DVM_GetInternetReachability + 4, 0xd65f03c0); // ret
 *
 * killerbean_nx reaches the same result by replacing the il2cpp icall instead.
 * Both work; this way keeps it in the same guarded-patch table as everything
 * else here, with no icall-table lookup to get right.
 */
#define GAME_RVA_DVM_GetInternetReachability 0x00a283b4u
#define GUARD_DVM_GetInternetReachability    0xd10083ffu  /* sub sp, sp, #0x20 */

/* ===========================================================================
 * APK FILESYSTEM HANDLER -- make it decline everything
 *
 * Unity's VFS asks each registered handler "can you handle this path?" before
 * opening anything. FileSystemAndroidAPK::CanHandle answers by calling
 * ZipCentralDirectoryWrapper::ApkStat, which scans for a zip central directory.
 *
 * There is no APK here. Every asset is a loose file staged on the SD card. So
 * that scan searches a file that is not a zip, finds nothing, and does it again
 * for the NEXT path. Over the fake-fd layer on an SD card that is not merely
 * wasteful -- the first boot that reached the frame loop sat at a black screen
 * with the main thread parked in fsFileWrite/fsFileRead, and the stall
 * backtrace showed:
 *
 *     FileSystem::GetHandlerForPath          libunity+0x945400
 *     FileSystemAndroidAPK::CanHandle+0x84   libunity+0xa5d6b4
 *     ZipCentralDirectoryWrapper::ApkStat    libunity+0xa3a818
 *     ZipCentralDirectoryWrapper::FindCentralDirectory
 *
 * Forcing CanHandle to return false makes GetHandlerForPath fall through to
 * LocalFileSystemPosix, which is the handler that should be servicing loose
 * files anyway. Nothing is lost: the APK handler could never have succeeded.
 *
 *     write32(base + GAME_RVA_APK_CanHandle + 0, 0x52800000);  // mov w0, #0
 *     write32(base + GAME_RVA_APK_CanHandle + 4, 0xd65f03c0);  // ret
 *
 * Note the jar:file:// path shapes still work -- bm_assets.c rewrites them to
 * real SD paths before they ever reach a handler. See bm_assets.h.
 * ========================================================================= */
#define GAME_RVA_APK_CanHandle    0x00a5d630u   /* 172 b */
#define GUARD_APK_CanHandle       0xd10143ffu   /* sub sp, sp, #0x50 */

/* ===========================================================================
 * DEVICE API LEVEL -- report 33, the same number our JNI already reports
 *
 * android::systeminfo::DeviceApiLevel() returned 0 on this port, while
 * jni_fake.c has always answered Build.VERSION.SDK_INT = 33. The two halves of
 * the same fiction disagreed, and Unity believed the native one.
 *
 * That single 0 hung the boot. ChoreographerBase::Get() (0x00a44fdc) picks its
 * implementation like this:
 *
 *     bl   GetContextType()          ; 1 == ActivityOrService, which we report
 *     cmp  w0, #1  /  b.eq +0x54
 *     bl   android::systeminfo::DeviceApiLevel()
 *     cmp  w0, #0x17                 ; API > 23 -> NDK Choreographer
 *     b.gt +0x140
 *     ...                            ; otherwise fall back to JAVA
 *
 * With API 0 it took the Java path and constructed ChoreographerJava, whose
 * constructor ends in an unconditional spin:
 *
 *     0xa4ab5c  ldr   x8, [x19, #0xe8]     ; the Java Choreographer instance
 *     0xa4ab60  cbz   x8, +0x10            ; still null -> wait
 *     0xa4ab68  cbnz  x8, +0x14            ; arrived -> unlock and return
 *     0xa4ab74  bl    pthread_cond_wait
 *     0xa4ab78  b     0xa4ab5c             ; forever
 *
 * That field is filled by a Java Handler thread calling
 * android.view.Choreographer.getInstance(). There is no Java here, so it is
 * never filled, and the engine parks inside nativeRender on the second frame --
 * which is exactly where this port stalled, via
 * UnityPlayerLoop -> UnityPause -> EnableFrameTimeTracker -> ChoreographerBase::Get.
 *
 * Reporting 33 takes the NDK path instead, which is the one this port is
 * already built for: android_native_unity.c implements ALooper_prepare /
 * _pollOnce / _wake / _addFd with a create-on-demand per-thread looper, and its
 * own comment names NdkLooper as the reason. imports.c likewise stubs
 * AChoreographer_getInstance to NULL deliberately, describing the NDK
 * choreographer as the path Unity 6 takes.
 *
 * 33 rather than the minimum 24: it matches NX_SDK_INT in jni_fake.c, and it
 * also clears the second check inside ChoreographerNDK's constructor
 * (`cmp w0, #0x1c`, API > 28) that selects postFrameCallback64 over the 32-bit
 * variant. Reporting 24 would leave those two answers inconsistent again.
 *
 *     write32(base + GAME_RVA_DeviceApiLevel + 0, 0x52800420);  // mov w0, #33
 *     write32(base + GAME_RVA_DeviceApiLevel + 4, 0xd65f03c0);  // ret
 *
 * IF THIS MOVES THE HANG RATHER THAN FIXING IT: ChoreographerNDK's constructor
 * has its own wait at 0xa4b2ec..0xa4b300, entered only when
 * NdkLooper::CreateHandler (0x00a5145c) returns non-NULL --
 * `cbz x0, +0x2c` at 0xa4b2d8 skips it entirely otherwise. Forcing that to
 * return NULL gives a Choreographer that constructs cleanly and does nothing,
 * which is fine here because bm_vsync.c already drives the frame counter.
 * ========================================================================= */
/* [1 of 3] Force the context branch.
 *
 * ChoreographerBase::Get() decides in two stages, and the API level is only
 * consulted in ONE of them:
 *
 *     0xa44ff4  bl   GetContextType()
 *     0xa44ff8  cmp  w0, #1
 *     0xa44ffc  b.eq +0x34            ; ==1 -> DeviceApiLevel check -> maybe NDK
 *     0xa45000  cbnz w0, +0x7c        ; !=0 -> third path
 *     0xa45004  ...                   ; ==0 -> JAVA, unconditionally
 *
 * This port reports ActivityOrService, which is 0 -- truthfully, because the
 * game's Java class really is UnityPlayerForActivityOrService. So the API-level
 * branch is never taken and patching DeviceApiLevel alone changed nothing: the
 * boot hung at ChoreographerJava's constructor with a byte-identical backtrace.
 *
 * Force the b.eq to an unconditional b, sending Get() into the API-level check
 * whatever the context type is. 0x540001a0 -> 0x1400000d: same target
 * (+13 instructions), no condition.
 *
 * WHY NOT JUST CLAIM GameActivity AT initJni. Because the Java class genuinely
 * is ActivityOrService, and initJni's contextType feeds input, window and
 * lifecycle handling as well as this. Lying globally to fix one decision would
 * change all of them. This confines the lie to the single branch that needs it.
 * (Reasoning adopted from hmsniper_nx, which hit this in the same order.)
 */
#define GAME_RVA_Choreographer_ctx_branch  0x00a44ffcu
#define GUARD_Choreographer_ctx_branch     0x540001a0u   /* b.eq #0xa45030 */
#define PATCH_Choreographer_ctx_branch     0x1400000du   /* b    #0xa45030 */

/* [2 of 3] Stop ChoreographerNDK's constructor waiting on its looper.
 *
 *     0xa4b2d0  bl   NdkLooper::CreateHandler
 *     0xa4b2d4  str  x0, [x19, #0x180]
 *     0xa4b2d8  cbz  x0, +0x2c          ; NULL -> skip the wait, return cleanly
 *     ...
 *     0xa4b2ec  ldr  x8, [x19, #0x178]
 *     0xa4b2f0  cbnz x8, +0x14
 *     0xa4b2fc  bl   pthread_cond_wait
 *     0xa4b300  b    0xa4b2ec           ; until the looper thread signals
 *
 * Patch [1] routes us to ChoreographerNDK, which then waits for its own looper
 * thread. That would trade the Java hang for an NDK one. The cbz is the
 * documented escape: a NULL handler skips the wait entirely.
 *
 * `bl CreateHandler` -> `mov x0, #0` takes it. The result is a Choreographer
 * that constructs cleanly and delivers no callbacks -- which is exactly right
 * here, because bm_vsync.c already drives the frame counter and we do not want
 * Unity pacing frames from a second source.
 *
 * RESIDUAL RISK, stated plainly: the handler is stored at [x19, #0x180] and
 * anything that later dereferences it without a null check turns this from an
 * early hang into a later crash. Enable()/Disable() are the candidates
 * (0xa4b3d4 / 0xa4b3fc). If a Data Abort appears at ChoreographerNDK+0x180,
 * that is this patch, and the follow-up is to stub those two as well.
 */
#define GAME_RVA_NdkLooper_CreateHandler_call 0x00a4b2d0u
#define GUARD_NdkLooper_CreateHandler_call    0x94001863u   /* bl #0xa5145c   */
#define PATCH_NdkLooper_CreateHandler_call    0xd2800000u   /* mov x0, #0     */

/* [3 of 3] The API level itself. */
#define GAME_RVA_DeviceApiLevel   0x00a451b4u
#define GUARD_DeviceApiLevel      0xd100c3ffu   /* sub sp, sp, #0x30 */
#define BM_REPORTED_API_LEVEL     33

/* ===========================================================================
 * FMOD OutputOpenSL: make it actually enqueue
 *
 * Inside FMOD::OutputOpenSL::init, verified as the ONLY site in this libunity
 * matching the pattern (one candidate found by scanning .text for
 * `ldr w9,[x19,#0x3f8]` followed by `cbz w9`):
 *
 *   0x18c06c8  ldr  w9, [x19, #0x3f8]   framesPerBuffer, from the AudioManager
 *   0x18c06cc  cbz  w9, fail            property we answer (64)
 *   ...
 *   0x18c0714  ldr  w9, [x19, #0x3f8]   re-read
 *   0x18c0720  mul  w10, w20, w21       dspNumBuffers * dspBufferLength
 *   0x18c0748  udiv w9, w10, w9         N = product / framesPerBuffer
 *
 * N is the number of buffers FMOD enqueues up front. Integer division: when the
 * product is smaller than the divisor, N == 0, init still reports success, and
 * nothing is ever enqueued. That is the observed state --
 * `[audio] enq=0 consumed=0 cb=0` with the mixer thread asleep in nanosleep.
 *
 * [1] Force the period. Replace the load with a constant and the following
 *     cbz with the store, so [x19,#0x3f8] becomes BM_AUDIO_PERIOD_FRAMES and
 *     the rest of the function sees a sane geometry.
 * [2] Force the count. Replace the udiv outright, so N cannot be 0 whatever
 *     the geometry turns out to be.
 *
 * [2] alone would do, but [1] keeps the period FMOD later divides by consistent
 * with what it was told, which is what stops the mixer glitching once it runs.
 *
 * The udiv guard word here (0x1ac90949) is byte-identical to cloverpit_nx's
 * GUARD_FMOD_BUFCOUNT on a different Unity build -- the same code, unchanged.
 * ========================================================================= */
#define GAME_RVA_FMOD_PERIOD_LDR   0x018c06c8u
#define GUARD_FMOD_PERIOD_LDR      0xb943fa69u   /* ldr w9,[x19,#0x3f8] */
#define GAME_RVA_FMOD_PERIOD_CBZ   0x018c06ccu
#define GUARD_FMOD_PERIOD_CBZ      0x34000129u   /* cbz w9, fail        */
/* movz w9, #BM_AUDIO_PERIOD_FRAMES  /  str w9,[x19,#0x3f8] */
#define PATCH_FMOD_PERIOD_MOV      (0x52800000u | ((BM_AUDIO_PERIOD_FRAMES & 0xffffu) << 5) | 9u)
#define PATCH_FMOD_PERIOD_STR      0xb903fa69u

#define GAME_RVA_FMOD_BUFCOUNT     0x018c0748u
#define GUARD_FMOD_BUFCOUNT        0x1ac90949u   /* udiv w9, w10, w9 */
#define PATCH_FMOD_BUFCOUNT        (0x52800000u | ((BM_AUDIO_UPFRONT_BUFFERS & 0xffffu) << 5) | 9u)

/* Engine entry points ----------------------------------------------------- */
#define GAME_RVA_UnityInitApplication     0x00a54ca0u  /* 6380 b */
#define GAME_RVA_UnityPlayerLoop          0x00a56de0u  /*  920 b */
#define GAME_RVA_InitializeGfxDevice      0x010644a8u  /*  900 b */

/* Window / EGL. AttachWindow and SetWindow are where an ANativeWindow becomes
 * an EGL surface -- the seam the loader's fake native window has to satisfy. */
#define GAME_RVA_AndroidGraphics_SetWindow              0x00a391e4u  /*  420 b */
#define GAME_RVA_AndroidDisplayManagerGLES_AttachWindow 0x00a3cc10u  /*  924 b */
#define GAME_RVA_ContextGLES_OnPostCreateSurface        0x00a26decu  /* 1000 b */
#define GAME_RVA_WindowContextEGL_Present               0x00ac2248u  /*  400 b */

/* Screen. ScreenManagerAndroid queries the Java view hierarchy for size, safe
 * area and cutouts; all three need answering from the loader. */
#define GAME_RVA_ScreenManagerAndroid_SetRequestedResolution 0x00a46fd8u /* 584 b */
#define GAME_RVA_ScreenManagerAndroid_UpdateResolutionData   0x00a46e24u /* 436 b */
#define GAME_RVA_ScreenManagerAndroid_GetSafeArea            0x00a46318u /* 456 b */
#define GAME_RVA_ScreenManagerAndroid_FindGlSurfaceView      0x00a46198u /* 384 b */

/* ===========================================================================
 * AUDIO: two patches, both required
 *
 * The port implements Android OpenSL ES in opensles.c on top of an SDL2 audio
 * device. Getting FMOD to actually use it takes two changes, and the second is
 * not optional -- the first alone gets you as far as slCreateEngine() and then
 * a hard failure.
 * ========================================================================= */

/* [1] Force the audio backend selector to OpenSL.
 *
 * AndroidAudio::GetAndroidAudioOutputType(int) picks between AudioTrack, OpenSL,
 * AAudio and a no-sound path using an internal Unity enum -- not FMOD_OUTPUTTYPE.
 * This build returns 1, 2, 3 and 4 from five sites; 2 is OpenSL, matching
 * cloverpit_nx's AUDIO_SELECT_OPENSL on the same Unity generation.
 *
 * libc_shim.c's dlopen already refuses libaaudio.so, so AAudio would fail to
 * load anyway -- but failing a dlopen and never selecting the backend are
 * different things, and only the second is quiet. Patch the selector:
 *
 *     write32(base + GAME_RVA_GetAndroidAudioOutputType + 0, 0x52800040); // movz w0,#2
 *     write32(base + GAME_RVA_GetAndroidAudioOutputType + 4, 0xd65f03c0); // ret
 */
#define AUDIO_SELECT_OPENSL                     2
#define GAME_RVA_GetAndroidAudioOutputType      0x00a54114u  /* 388 b */
#define GUARD_GetAndroidAudioOutputType         0xd10203ffu  /* sub sp, sp, #0x80 */

/* [2] Defeat FMOD's OpenSL buffer-geometry bound check.
 *
 * Forcing OpenSL is only half the job. FMOD then validates the output period
 * against the game's BAKED DSP buffer settings and refuses the device with
 *
 *     Unity: FMOD failed to initialize the output device. (60)
 *
 * if framesPerBuffer > (dspNumBuffers - 1) * dspBufferLength, even after one
 * halving. A title whose baked dspNumBuffers is 1 makes that bound (1-1)*len ==
 * 0, which NO positive period can satisfy -- so no value the fake AudioManager
 * reports can rescue it and the branch itself has to go.
 *
 * Located in THIS build with the register-masked 8-word fingerprint from
 * cloverpit_nx (FP_FMOD_BUFGEOM). Matches exactly once in 18.6 MB of .text, and
 * the match sits inside FMOD::OutputOpenSL::init (0x18c056c..0x18c09f0), which
 * is the confirmation that matters:
 *
 *   0x018c06c8  ldr  w9, [x19, #0x3f8]   ; framesPerBuffer
 *   0x018c06cc  cbz  w9, fail            ; zero guard -- LEFT INTACT
 *   0x018c06d0  sub  w10, w20, #1        ; dspNumBuffers - 1
 *   0x018c06d4  mul  w10, w10, w21       ; * dspBufferLength
 *   0x018c06d8  cmp  w9, w10
 *   0x018c06dc  b.ls +0xc                ; already fits -> skip the halving
 *   0x018c06e0  lsr  w9, w9, #1          ; halve framesPerBuffer
 *   0x018c06e4  str  w9, [x19, #0x3f8]
 *   0x018c06e8  cmp  w9, w10
 *   0x018c06ec  b.ls +0x10               ; <-- PATCH to an unconditional b
 *   0x018c06f0  mov  x0, x19 ; bl ...    ; FAILURE path -> error 60
 *
 * Rewrite the b.ls to a plain b: identical target, always the success path.
 * 0x54000089 (b.ls +0x10) becomes 0x14000004 (b +0x10) -- same 4-instruction
 * displacement, encoded unconditionally.
 *
 * The success path derives its buffer count as (w20*w21)/period, which stays
 * >= 1 because jni_fake reports a small 64-frame period, so forcing the branch
 * does not produce a degenerate count. Both zero guards above are left intact
 * and pass (48000 and 64 are non-zero).
 */
#define GAME_RVA_FMOD_BUFGEOM_BRANCH   0x018c06ecu
#define GUARD_FMOD_BUFGEOM_BRANCH      0x54000089u  /* b.ls #0x18c06fc */
#define PATCH_FMOD_BUFGEOM_BRANCH      0x14000004u  /* b    #0x18c06fc */

#define GAME_RVA_AudioManager_InitFMOD    0x00c2946cu  /* 1884 b */
#define GAME_RVA_FMOD_OutputOpenSL_init   0x018c056cu  /* 1156 b */

/* ===========================================================================
 * SWAPPY: no patch needed, and here is why
 *
 * Unity 6 drives Android frame pacing through the Swappy wrapper. The engine
 * loads it at runtime via Swappy::LoadSwappyWrapperLibrary() (0x00a2dae8),
 * which dlopen()s libswappywrapper.so.
 *
 * That library is NOT in this APK -- zero matches across the whole archive --
 * so the dlopen fails on real hardware too, and libc_shim.c's dlopen refuse
 * list ("libswappywrapper.so", "libvulkan.so", "libaaudio.so") makes the
 * failure immediate and logged rather than a filesystem miss.
 *
 * The consequence is exactly what this port wants. In
 * WaitForLastPresentationAndGetTimestamp(), the branch at 0x00a25934 calls
 * Swappy::IsEnabledAndActive() (0x00a2d700) and:
 *
 *   tbz w8, #0, #0xa2597c    ; Swappy inactive -> go straight to WaitVSync
 *
 * With Swappy dead, bit 0 is clear, and control falls to WaitVSync(x20) --
 * the path bm_vsync.c is built to service. Likewise
 * Swappy::GetSwappyTargetFrameRate() (0x00a2d70c) returns 0, and the
 * fcmp/fcsel at 0x00a25864 then selects GetEffectiveRenderFrameRate() instead.
 *
 * So Swappy disables itself correctly and the engine takes the fallback the
 * rest of this port targets. Do not add a patch here; verify the dlopen refusal
 * is still in libc_shim.c instead. If a future build ever ships
 * libswappywrapper.so, the refusal is what keeps this true.
 * ========================================================================= */
#define GAME_RVA_Swappy_IsEnabledAndActive     0x00a2d700u  /* 12 b  */
#define GAME_RVA_Swappy_GetTargetFrameRate     0x00a2d70cu  /* 132 b */
#define GAME_RVA_Swappy_LoadWrapperLibrary     0x00a2dae8u  /* 304 b */

/* Input. This is the injection point for the controller-to-touch mapping --
 * the same seam killerbean_nx drives from nx_newinput.c. The game is a
 * touchscreen title, so a synthesised touch here is indistinguishable from a
 * finger to both the engine and the managed code. */
#define GAME_RVA_NewInput_ProcessTouchEvent 0x00a6cf00u  /* 892 b */
#define GAME_RVA_ProcessTouchEvent          0x00a36e88u  /* 796 b */

#endif /* BM_OFFSETS_H */
