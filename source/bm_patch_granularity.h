/* ---------------------------------------------------------------------------
 * bm_patch_granularity.h -- in-memory libunity.so patch table that moves
 * Unity's block allocator from 256 MB region granularity to 64 MB.
 *
 *   BOUNCEMASTERS  (Unity 6000.0.58f2 / 92dee566b325, arm64, IL2CPP)
 *   libunity.so  BuildID sha1 02e993102d7c6d21c27fb5195ba94aa5958f1ae3
 *
 * WHY THIS IS NOT OPTIONAL
 * -----------------------
 * Unity's block allocator reserves memory in 256 MB-aligned regions. On a 4 GB
 * Switch that granularity does not fit the address space a .so loader leaves
 * behind, so the reservation fails and the game dies during MemoryManager init:
 *
 *     Unity: MemoryManager: Using 'Dynamic Heap' Allocator.
 *     [gpua] memalign FAILED size=524284 KB
 *
 * 524284 KB is 512 MB - 4 KB: two 256 MB regions. Rewriting the granularity to
 * 64 MB turns that into requests the heap can actually satisfy. This is the same
 * transform every Unity port in this lineage needs -- Zookeeper DX, PvZ Fusion,
 * Fruit Ninja, Killer Bean, CloverPit -- and without it Unity games do not run
 * on Switch. It is not a workaround for a sizing mistake; the sizing is a
 * consequence of the granularity.
 *
 * 64 MB is the KNOWN-GOOD FLOOR. killerbean_nx records that 16 MB was tried and
 * CORRUPTED Unity's Dynamic Heap allocator at init -- overlapping regions from
 * the over-map/trim pattern, producing a null-prev free-list crash. Do not go
 * lower. MMAP_ARENA_ALIGN in config.h MUST equal this value: the mmap arena
 * hands back region-aligned reservations, and a mismatch means Unity trims
 * pages the arena still believes are live.
 *
 * HOW THESE OFFSETS WERE DERIVED
 * ------------------------------
 * From a symbolized companion build, libunity_sym.so, BuildID-identical to the
 * shipped libunity.so (02e99310..., verified byte-for-byte against the copy in
 * the APK). Procedure, mirroring killerbean_nx's:
 *
 *   1. Every instruction in .text (4,889,026 of them) was decoded for the six
 *      encoding classes that can participate in a 256 MB region computation,
 *      and each hit attributed to its owning function by symbol lookup.
 *      431 raw hits; 20 of them land in allocator-named functions.
 *   2. Independently, the 10 named allocator functions were scanned directly.
 *      Same 20 sites, same addresses.
 *
 * Two scans with different starting assumptions agreeing site-for-site is why
 * completeness is not an open question here.
 *
 * CORROBORATION AGAINST A WORKING PORT. Killer Bean (Unity 2021.3.31f1) needs
 * 21 sites across 11 functions. This build needs 20 across 10 -- the same
 * functions, and many of the {from} words are BYTE-IDENTICAL despite being a
 * different Unity version and a different link:
 *
 *     0x12be0009  0x52a20009  0x92648d36  0xd35cdc33  0xd35cfd15
 *     0xd35cfc28  0xd35c9c2a  0xd35cdc29  0xf2a2000b  0xcb0a7108
 *
 * TRANSFORMS
 *     MOVZ/MOVK  imm16   0x1000 -> 0x0400          256MB -> 64MB
 *     MOVN       imm16   0xF000 -> 0xFC00          0x0FFFFFFF -> 0x03FFFFFF
 *     LSR (UBFM) immr    28 -> 26                  >>28 -> >>26
 *     UBFX(UBFM) immr 28->26, imms -= 2            field WIDTH preserved
 *     SUB shifted        lsl #28 -> lsl #26
 *     AND bitmask        low bound bit 28 -> 26, TOP BOUND PRESERVED
 *     LSR (UBFM) immr    40 -> 38                  level-1 index, see below
 *
 * THE LEVEL-1 SITES ([L1] below) ARE NOT OPTIONAL
 * -----------------------------------------------
 * The region lookup is a two-level page table:
 *     level 1:  ptr >> 40
 *     level 2:  (ptr >> 28) & 0xFFF
 * Moving level 2 to bit 26 without moving level 1 leaves the two levels
 * non-contiguous: level 2 then spans bits 26..37 while level 1 still starts at
 * bit 40, so bits 38-39 are dropped and every pointer above 256 GB aliases onto
 * a wrong slot. That is not hypothetical here -- this console mapped libunity at
 * 0x78ebd87000 (~484 GB). killerbean_nx found these two only after a live NULL
 * return from GetAllocatorContainingPtr whose caller then did an unchecked load.
 *
 * SAFETY
 * ------
 * bm_patch_granularity() is VERIFY-FIRST and ALL-OR-NOTHING: it reads every
 * target word, and only if ALL 20 match {from} does it write anything. One
 * mismatch means this is not the binary the table was derived from, and a
 * partially-patched allocator is far worse than an unpatched one. After a game
 * update, expect it to fire -- that is the table telling you to re-run
 * tools/scan_granularity.py, not a bug.
 *
 * MIT.
 * ------------------------------------------------------------------------- */

#ifndef BM_PATCH_GRANULARITY_H
#define BM_PATCH_GRANULARITY_H

#include <stdint.h>

typedef struct { uint32_t rva; uint32_t from; uint32_t to; const char *what; } BmPatchWord;

/* Region granularity in MB after patching. MUST equal MMAP_ARENA_ALIGN. */
#define BM_REGION_GRANULARITY_MB 64u

static const BmPatchWord BM_GRANULARITY_WORDS[] = {
  /*  0 */ { 0x0762418u, 0x12be0009u, 0x12bf8009u, "LocalLowLevelAllocator::ReserveMemoryBlock +0x88" },  /* movn 0xF000->0xFC00 */
  /*  1 */ { 0x0762428u, 0x92648d35u, 0x92669535u, "LocalLowLevelAllocator::ReserveMemoryBlock +0x98" },  /* and mask bits 28..63 -> 26..63 */
  /*  2 */ { 0x0762d04u, 0x52a20008u, 0x52a08008u, "TLSAllocator +0x10" },  /* movz/movk 0x1000->0x0400 */
  /*  3 */ { 0x07645fcu, 0x52a20009u, 0x52a08009u, "BucketAllocator::BucketAllocator +0xe8" },  /* movz/movk 0x1000->0x0400 */
  /*  4 */ { 0x0767080u, 0x52a20009u, 0x52a08009u, "DynamicHeapAllocator::DynamicHeapAllocator +0x4c" },  /* movz/movk 0x1000->0x0400 */
  /*  5 */ { 0x076753cu, 0x12be000du, 0x12bf800du, "DynamicHeapAllocator::RequestLargeAllocMemor +0x50" },  /* movn 0xF000->0xFC00 */
  /*  6 */ { 0x0767568u, 0x92648d36u, 0x92669536u, "DynamicHeapAllocator::RequestLargeAllocMemor +0x7c" },  /* and mask bits 28..63 -> 26..63 */
  /*  7 */ { 0x0769548u, 0xd35cdc33u, 0xd35ad433u, "MM::VirtualAllocator::MarkMemoryBlocks +0x10" },  /* ubfx #28,#55->#26,#53 */
  /*  8 */ { 0x0769550u, 0xd35cfd15u, 0xd35afd15u, "MM::VirtualAllocator::MarkMemoryBlocks +0x18" },  /* lsr #28->#26 */
  /*  9 */ { 0x07695f4u, 0x52a20008u, 0x52a08008u, "MM::VirtualAllocator::ReserveMemoryBlock +0x50" },  /* movz/movk 0x1000->0x0400 */
  /* 10 */ { 0x0769928u, 0xd35cfc28u, 0xd35afc28u, "MM::VirtualAllocator::GetMemoryBlockFromPoin +0x0" },  /* lsr #28->#26 */
  /* 11 */ { 0x0769938u, 0x92646c28u, 0x92667428u, "MM::VirtualAllocator::GetMemoryBlockFromPoin +0x10" },  /* and mask bits 28..55 -> 26..55 */
  /* 12 */ { 0x0769940u, 0xd35c9c2au, 0xd35a942au, "MM::VirtualAllocator::GetMemoryBlockFromPoin +0x18" },  /* ubfx #28,#39->#26,#37 */
  /* 13 */ { 0x0769954u, 0xd35cdc29u, 0xd35ad429u, "MM::VirtualAllocator::GetMemoryBlockFromPoin +0x2c" },  /* ubfx #28,#55->#26,#53 */
  /* 14 */ { 0x076995cu, 0xf2a2000bu, 0xf2a0800bu, "MM::VirtualAllocator::GetMemoryBlockFromPoin +0x34" },  /* movz/movk 0x1000->0x0400 */
  /* 15 */ { 0x07699a0u, 0xcb0a7108u, 0xcb0a6908u, "MM::VirtualAllocator::GetMemoryBlockFromPoin +0x78" },  /* sub lsl#28->lsl#26 */
  /* 16 */ { 0x07699b4u, 0xd368fc28u, 0xd366fc28u, "MM::VirtualAllocator::GetBlockInfoFromPointe +0x0" },  /* lsr #40->#38 [L1] */
  /* 17 */ { 0x07699c4u, 0xd35c9c29u, 0xd35a9429u, "MM::VirtualAllocator::GetBlockInfoFromPointe +0x10" },  /* ubfx #28,#39->#26,#37 */
  /* 18 */ { 0x076b394u, 0xd368fc28u, 0xd366fc28u, "MM::GetAllocatorContainingPtr +0xc" },  /* lsr #40->#38 [L1] */
  /* 19 */ { 0x076b3acu, 0xd35c9e89u, 0xd35a9689u, "MM::GetAllocatorContainingPtr +0x24" },  /* ubfx #28,#39->#26,#37 */
};
#define BM_GRANULARITY_WORDS_N \
  ((int)(sizeof(BM_GRANULARITY_WORDS) / sizeof(BM_GRANULARITY_WORDS[0])))

#endif /* BM_PATCH_GRANULARITY_H */
