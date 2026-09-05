/* mvp_maptab.h - geometry and hash for the in-flight Map/Unmap pairing table.
 *
 * Split out of mvp_patch.c on 2026-09-05 for ONE reason: so that
 * tools/map_pairing_test.c can exercise the SHIPPED hash rather than a
 * transcription of it. (Far Cry 2 precedent: a Python transcription of
 * stereo.c passed its check, and compiling the real stereo.c into a harness
 * was what actually caught two bugs.)
 *
 * Nothing here touches D3D11 types, so the test needs no D3D headers.
 */
#ifndef MVP_MAPTAB_H
#define MVP_MAPTAB_H

#include <stddef.h>

/* 1024 slots for a working set of at most a few dozen in-flight maps. The old
 * table was 32 and filled permanently; this is not a tuned number, it is
 * deliberately far larger than any plausible working set, because the cost is
 * 1024 * 24 bytes and the failure mode of being too small is silent. */
#define MVP_MAPTAB_BITS  10
#define MVP_MAPTAB_SIZE  (1 << MVP_MAPTAB_BITS)
#define MVP_MAPTAB_MASK  (MVP_MAPTAB_SIZE - 1)
#define MVP_MAPTAB_PROBE 24

/* Mixes BOTH halves of the (context, resource) key.
 *
 * This is the whole point of the 2026-09-05 fix. A hash on `res` alone would
 * pile every deferred context's concurrent map of the same buffer into one
 * bucket - precisely the collision that sank the old linear table, since
 * "two deferred contexts may legally Map the same ID3D11Buffer at the same
 * time (WRITE_DISCARD renames per context)".
 *
 * splitmix64-style finaliser over two odd-multiplier-scrambled halves. The
 * inputs are heap pointers, which are typically 16-byte aligned and share
 * high bits, so the low bits of a naive combination would be near-constant;
 * the finaliser is what moves entropy down into the masked bits.
 */
static unsigned mvp_map_hash(const void *ctx, const void *res) {
    unsigned long long h = (unsigned long long)(size_t)ctx * 0x9E3779B97F4A7C15ull;
    h ^= (unsigned long long)(size_t)res * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return (unsigned)h & MVP_MAPTAB_MASK;
}

#endif /* MVP_MAPTAB_H */
