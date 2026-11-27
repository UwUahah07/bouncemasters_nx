/* ---------------------------------------------------------------------------
 * bm_patch_granularity.h -- OPENBONK table (Unity 2023.2.22f1 / 6b19bf4f8115, arm64).
 *
 * Moves Unity's block allocator from 256 MB region granularity to 64 MB. See the
 * Bouncemasters original for the full rationale; the transforms are identical:
 *     MOVZ/MOVK imm16 0x1000->0x0400, MOVN 0xF000->0xFC00, LSR/UBFX immr 28->26
 *     (UBFX imms -= 2), SUB lsl#28->#26, AND mask low bit 28->26, LSR #40->#38.
 *
 * DERIVATION (no symbols available for this build): all of .text was scanned for
 * the same six encoding classes; the hits cluster at 0x656f90..0x65fb10 and
 * reproduce the Bouncemasters allocator sites word-for-word (17 sites are
 * byte-identical, the rest differ only in register numbers). Two sites exist
 * here that the Bouncemasters table does not have: TLSAllocator and the
 * DynamicHeapAllocator ctor clamp the block size as
 *       x = ((size >> 28) == 0) ? size : 0x10000000
 * so the shift AND the constant must both move (0x656fa0/a4 and 0x65b55c/60).
 *
 * VERIFY-FIRST, ALL-OR-NOTHING (see bm_patches.c). 22 sites.
 * MIT.
 * ------------------------------------------------------------------------- */

#ifndef BM_PATCH_GRANULARITY_H
#define BM_PATCH_GRANULARITY_H

#include <stdint.h>

typedef struct { uint32_t rva; uint32_t from; uint32_t to; const char *what; } BmPatchWord;

/* Region granularity in MB after patching. MUST equal MMAP_ARENA_ALIGN. */
#define BM_REGION_GRANULARITY_MB 64u

static const BmPatchWord BM_GRANULARITY_WORDS[] = {
  /*  0 */ { 0x06566f4u, 0x12be0009u, 0x12bf8009u, "LocalLowLevelAllocator::ReserveMemoryBlock [movn 0xF000->0xFC00]" },
  /*  1 */ { 0x06566fcu, 0x92648d35u, 0x92669535u, "LocalLowLevelAllocator::ReserveMemoryBlock [and mask bits 28..63 -> 26..63]" },
  /*  2 */ { 0x0656fa0u, 0xd35cfc28u, 0xd35afc28u, "TLSAllocator (clamp: size>>28) [lsr #28->#26]" },
  /*  3 */ { 0x0656fa4u, 0x52a20009u, 0x52a08009u, "TLSAllocator (clamp: 256MB) [movz 0x1000->0x0400]" },
  /*  4 */ { 0x065890cu, 0x52a20009u, 0x52a08009u, "BucketAllocator::BucketAllocator [movz 0x1000->0x0400]" },
  /*  5 */ { 0x065b55cu, 0x531c7d2au, 0x531a7d2au, "DynamicHeapAllocator ctor (clamp: size>>28, 32-bit) [lsr w #28->#26]" },
  /*  6 */ { 0x065b560u, 0x52a2000cu, 0x52a0800cu, "DynamicHeapAllocator ctor (clamp: 256MB) [movz 0x1000->0x0400]" },
  /*  7 */ { 0x065b9f8u, 0x12be000au, 0x12bf800au, "DynamicHeapAllocator::RequestLargeAllocMemory [movn 0xF000->0xFC00]" },
  /*  8 */ { 0x065ba00u, 0x92648d37u, 0x92669537u, "DynamicHeapAllocator::RequestLargeAllocMemory [and mask bits 28..63 -> 26..63]" },
  /*  9 */ { 0x065daf8u, 0xd35cdc33u, 0xd35ad433u, "MM::VirtualAllocator::MarkMemoryBlocks [ubfx #28,#55->#26,#53]" },
  /* 10 */ { 0x065dafcu, 0xd35cfd15u, 0xd35afd15u, "MM::VirtualAllocator::MarkMemoryBlocks [lsr #28->#26]" },
  /* 11 */ { 0x065db8cu, 0x52a20008u, 0x52a08008u, "MM::VirtualAllocator::ReserveMemoryBlock [movz 0x1000->0x0400]" },
  /* 12 */ { 0x065df50u, 0xd35cfc28u, 0xd35afc28u, "MM::VirtualAllocator::GetMemoryBlockFromPointer [lsr #28->#26]" },
  /* 13 */ { 0x065df60u, 0x92646c28u, 0x92667428u, "MM::VirtualAllocator::GetMemoryBlockFromPointer [and mask bits 28..55 -> 26..55]" },
  /* 14 */ { 0x065df68u, 0xd35c9c2au, 0xd35a942au, "MM::VirtualAllocator::GetMemoryBlockFromPointer [ubfx #28,#39->#26,#37]" },
  /* 15 */ { 0x065df80u, 0xd35cdc29u, 0xd35ad429u, "MM::VirtualAllocator::GetMemoryBlockFromPointer [ubfx #28,#55->#26,#53]" },
  /* 16 */ { 0x065df84u, 0xf2a2000bu, 0xf2a0800bu, "MM::VirtualAllocator::GetMemoryBlockFromPointer [movk 0x1000->0x0400]" },
  /* 17 */ { 0x065dfc4u, 0xcb0a7108u, 0xcb0a6908u, "MM::VirtualAllocator::GetMemoryBlockFromPointer [sub lsl#28->lsl#26]" },
  /* 18 */ { 0x065dfdcu, 0xd368fc28u, 0xd366fc28u, "MM::VirtualAllocator::GetBlockInfoFromPointer [lsr #40->#38 [L1]]" },
  /* 19 */ { 0x065dfecu, 0xd35c9c29u, 0xd35a9429u, "MM::VirtualAllocator::GetBlockInfoFromPointer [ubfx #28,#39->#26,#37]" },
  /* 20 */ { 0x065faf8u, 0xd368fc28u, 0xd366fc28u, "MM::GetAllocatorContainingPtr [lsr #40->#38 [L1]]" },
  /* 21 */ { 0x065fb10u, 0xd35c9e89u, 0xd35a9689u, "MM::GetAllocatorContainingPtr [ubfx #28,#39->#26,#37]" },
};
#define BM_GRANULARITY_WORDS_N \
  ((int)(sizeof(BM_GRANULARITY_WORDS) / sizeof(BM_GRANULARITY_WORDS[0])))

#endif /* BM_PATCH_GRANULARITY_H */
