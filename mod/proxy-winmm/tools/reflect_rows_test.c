/*
 * reflect_rows_test.c -- offline check of mvptable.c's reflection, against the
 * game's own shaders, with no game running.
 *
 * It links the SHIPPED mvptable.c and calls mvptable_reflect_rows() directly --
 * not a transcription of it -- over a file of concatenated DXBC containers
 * extracted from base/common.tangoresource, and compares every result against
 * an expectations file produced independently by the toolkit's own RDEF parser
 * (flat-to-vr-RE-toolkit/tools/dxbc-reflect.py). Two different reflectors on
 * the same bytes: D3DReflect from d3dcompiler_47.dll here, a from-scratch
 * Python RDEF walker there.
 *
 * Expectations file, one line per shader, in the order the shaders appear:
 *     <cb0size> <x> <y> <z> <w>          (-1 for a row that is absent, and
 *                                         cb0size 0 when there is no cb0)
 *
 * Usage: reflect_rows_test <shaders.bin> <expected.txt>
 *
 * Builds and runs nothing from the game. See README-reflect-rows-test.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include <stdint.h>

#include "../src/mvptable.h"

/* ---- stubs for the two things mvptable.c pulls in from the rest of the
 * proxy. Neither is on the path under test. ---- */
void log_msg(const char *fmt, ...) {
    (void)fmt; /* silence: the reflection path logs only on failure */
}
uint64_t shaderdump_hash_for_shader(const void *vs_ptr) {
    (void)vs_ptr;
    return 0; /* only used by the vs_ptr lookups, which this test does not touch */
}

static int fail = 0;

int main(int argc, char **argv) {
    FILE *f;
    long size;
    unsigned char *blob;
    size_t pos = 0;
    int n = 0, no_cb = 0, complete = 0, contiguous = 0, scattered = 0, mismatches = 0;
    FILE *exp;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <shaders.bin> <expected.txt>\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    blob = (unsigned char *)malloc((size_t)size);
    if (!blob || fread(blob, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    fclose(f);

    exp = fopen(argv[2], "r");
    if (!exp) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }

    while (pos + 32 <= (size_t)size) {
        unsigned int declared;
        int rows[4], cb0 = 0, got;
        int ecb0, ex, ey, ez, ew;

        if (memcmp(blob + pos, "DXBC", 4) != 0) {
            fprintf(stderr, "not a DXBC container at %zu\n", pos);
            return 2;
        }
        memcpy(&declared, blob + pos + 24, 4);
        if (declared < 64 || pos + declared > (size_t)size) {
            fprintf(stderr, "bad container size %u at %zu\n", declared, pos);
            return 2;
        }

        got = mvptable_reflect_rows(blob + pos, declared, &cb0, rows);
        pos += declared;
        n++;

        if (fscanf(exp, "%d %d %d %d %d", &ecb0, &ex, &ey, &ez, &ew) != 5) {
            fprintf(stderr, "expectations file ran out at shader %d\n", n);
            return 2;
        }
        if (!got) no_cb++;
        if (cb0 != ecb0 || rows[0] != ex || rows[1] != ey || rows[2] != ez || rows[3] != ew) {
            if (mismatches < 10) {
                printf("  MISMATCH shader %d: D3DReflect cb0=%d rows=%d,%d,%d,%d ; "
                       "expected cb0=%d rows=%d,%d,%d,%d\n",
                       n, cb0, rows[0], rows[1], rows[2], rows[3], ecb0, ex, ey, ez, ew);
            }
            mismatches++;
        }
        if (rows[0] >= 0 && rows[1] >= 0 && rows[2] >= 0 && rows[3] >= 0) {
            complete++;
            if (rows[1] == rows[0] + 16 && rows[2] == rows[0] + 32 && rows[3] == rows[0] + 48) {
                contiguous++;
            } else {
                scattered++;
            }
        }
    }
    fclose(exp);

    printf("\nshaders reflected      : %d\n", n);
    printf("  no cb0 found         : %d\n", no_cb);
    printf("  all four rows found  : %d  (contiguous %d, scattered %d)\n",
           complete, contiguous, scattered);
    printf("  disagreements with the independent RDEF parser: %d\n", mismatches);

    /* The property the change actually turns on: every shader with a complete
     * set of rows is now patchable, contiguous or not. Before the change only
     * the contiguous ones were. */
    printf("\npatchable under the OLD contiguous-only rule : %d\n", contiguous);
    printf("patchable under the NEW rule                 : %d  (+%d)\n", complete, scattered);

    if (mismatches) { printf("\nFAILED: %d disagreements\n", mismatches); fail = 1; }
    else printf("\nPASSED: the shipped reflection agrees with the independent parser on all %d\n", n);
    return fail;
}
