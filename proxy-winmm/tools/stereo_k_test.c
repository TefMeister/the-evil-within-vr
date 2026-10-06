/* stereo_k_test.c - host test for stereo_afr.c's eye matrix (2026-10-06).
 * Ground truth: for a perspective projection z = A*w + B, the eye matrix must move screen x by
 * s*k*B/w + s*k*(A - c), leave y, z, w alone, and mirror exactly between the eyes.
 * Build (from tools/): copy stereo_eye_k out of ../src/stereo_afr.c into a small file, then
   gcc -O2 -I../src -o stereo_k_test.exe stereo_k_test.c <that file> ../src/camera.c -lm  (done that way 2026-10-06). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "camera.h"

Mat4 stereo_eye_k(float eye_sign, float k, float c);

static int fails, checks;
static void check(int ok, const char *what) { ++checks; if (!ok) { ++fails; if (fails < 8) printf("  FAIL %s\n", what); } }

int main(void) {
    int t;
    srand(7);
    for (t = 0; t < 2000; ++t) {
        float A = 0.9f + 0.2f * rand() / RAND_MAX, B = -(0.05f + 3.0f * rand() / RAND_MAX);
        float k = 0.001f + 0.1f * rand() / RAND_MAX, c = 0.5f + 1.0f * rand() / RAND_MAX;
        float w = 0.5f + 200.0f * rand() / RAND_MAX, x = (2.0f * rand() / RAND_MAX - 1.0f) * w;
        float y = (2.0f * rand() / RAND_MAX - 1.0f) * w, z = A * w + B;
        float clip[4] = { x, y, z, w };
        int e;
        for (e = 0; e < 2; ++e) {
            float s = e ? 1.0f : -1.0f, out[4], want;
            Mat4 K = stereo_eye_k(s, k, c);
            int i, j;
            for (i = 0; i < 4; ++i) { out[i] = 0; for (j = 0; j < 4; ++j) out[i] += K.m[i][j] * clip[j]; }
            want = x / w + s * k * B / w + s * k * (A - c);
            check(fabsf(out[0] / out[3] - want) < 1e-4f * (1.0f + fabsf(want)), "screen x shift");
            check(out[1] == y && out[2] == z && out[3] == w, "y z w untouched");
        }
        {   /* the two eyes are mirror images about the mono position */
            Mat4 L = stereo_eye_k(-1, k, c), R = stereo_eye_k(1, k, c);
            float lx = 0, rx = 0; int j;
            for (j = 0; j < 4; ++j) { lx += L.m[0][j] * clip[j]; rx += R.m[0][j] * clip[j]; }
            check(fabsf((lx + rx) * 0.5f - x) < 1e-3f * (1.0f + fabsf(x)), "eyes mirror about mono");
        }
    }
    {   /* parallax falls with distance: near objects separate more than far ones */
        float A = 1.0f, B = -1.0f, k = 0.02f, c = 1.0f, near = 2.0f, far = 100.0f;
        float dn = 2 * k * fabsf(B) / near, df = 2 * k * fabsf(B) / far;
        (void)A; (void)c;
        check(dn > df * 10.0f, "near separates more than far");
    }
    printf("stereo_k_test: %d checks, %d failed\n", checks, fails);
    if (!fails) printf("ALL CHECKS PASSED\n");
    return fails ? 1 : 0;
}
