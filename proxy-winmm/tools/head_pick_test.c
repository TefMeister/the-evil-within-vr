/* head_pick_test.c - host test for head_lens_pick_main in src/head_k.c, linked as shipped (2026-10-15, /pd).
 *
 * The case it guards: on the home PC (3440x1440) the lens logged in gameplay had shape 2.306 while the first
 * head-tracked frame's picture rect implied 2.421 - more than the 3% that head_track_other_camera allows. If a
 * frame mixes cameras of different shapes and NO camera holds a majority of the samples, the OLD per-field median
 * (median sx, median sy taken separately) can build a shape that is no camera at all, and then every world draw is
 * "another camera": left flat and unturned. With a majority camera the old method was already right (case 1).
 *
 * Ground truth is built here: frames holding a main camera (most samples) and other cameras, each a known lens.
 * The pick must return the main camera's lens, and a world draw of the main camera must fall inside the 3% band.
 * The old method is run beside it as the planted failure: the test also checks that it DOES go wrong on some of
 * the no-majority frames (1,173 of 5,000 on 2026-10-15), so a pass cannot come from a test unable to see the fault.
 * Build (from tools/):  gcc -O2 -I../src -o head_pick_test.exe head_pick_test.c ../src/head_k.c ../src/camera.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "head_k.h"

#define TOL 0.03f   /* the same band head_track.c uses (HT_OTHER_TOL) */

static int fails, checks;
static void check(int ok, const char *what) { ++checks; if (!ok) { ++fails; if (fails < 10) printf("  FAIL %s\n", what); } }
static float rnd(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }

static HeadLens lens_of(float hfov_deg, float aspect) {
    HeadLens l;
    l.sx = 1.0f / tanf(hfov_deg * 0.5f * 3.14159265f / 180.0f);
    l.sy = l.sx * aspect;
    l.cx = l.cy = 0.0f;
    l.a = 1.0f;
    return l;
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static HeadLens old_median(const HeadLens *s, int n) {   /* the method used until 2026-10-15 */
    float v[64];
    HeadLens o;
    int i;
    for (i = 0; i < n; ++i) v[i] = s[i].sx; qsort(v, n, sizeof v[0], cmpf); o.sx = v[n / 2];
    for (i = 0; i < n; ++i) v[i] = s[i].sy; qsort(v, n, sizeof v[0], cmpf); o.sy = v[n / 2];
    o.cx = o.cy = 0.0f; o.a = 1.0f;
    return o;
}
static int inside(const HeadLens *main, const HeadLens *draw) {
    float m = main->sy / main->sx;
    return fabsf(draw->sy / draw->sx - m) <= TOL * m;
}

int main(void) {
    int t, old_wrong = 0;
    srand(11);
    /* 1. The named case: world at 65 deg / 2.421 (17 samples), a narrower second camera at 2.0 (15 samples). */
    {
        HeadLens s[32], got, world = lens_of(65.0f, 2.421f), other = lens_of(40.0f, 2.0f), old;
        int i, g;
        for (i = 0; i < 32; ++i) s[i] = (i % 2 == 0 || i == 31) ? world : other;   /* interleaved, 17 vs 15 */
        g = head_lens_pick_main(s, 32, TOL, &got);
        check(g == 17, "named case: group size 17");
        check(fabsf(got.sx - world.sx) < 1e-5f && fabsf(got.sy - world.sy) < 1e-5f, "named case: picks the world lens");
        check(inside(&got, &world), "named case: world draws are turned");
        check(!inside(&got, &other), "named case: the other camera is left alone");
        old = old_median(s, 32);
        printf("  named case: old method shape %.3f (world 2.421, other 2.000) -> world draws %s\n",
               (double)(old.sy / old.sx), inside(&old, &world) ? "turned" : "LEFT ALONE");
    }
    /* 2. Random frames: the main camera has the most samples but NOT always a majority (2-3 other shapes share the
     *    rest), jittered by measurement noise. Without a majority the old per-field median can miss. */
    for (t = 0; t < 5000; ++t) {
        HeadLens s[32], main_l = lens_of(rnd(40, 110), rnd(1.3f, 2.6f)), got, old;
        int n = 12 + rand() % 21, others = 2 + rand() % 2, n_main = n / others + 2, i, g, k;
        HeadLens oth[3];
        for (k = 0; k < others; ++k) {   /* other shapes at least 8% away from the main one */
            float asp;
            int clash;
            do {
                asp = rnd(1.0f, 3.0f);
                clash = fabsf(asp - main_l.sy / main_l.sx) < 0.08f * (main_l.sy / main_l.sx);
                for (i = 0; i < k; ++i) clash |= fabsf(asp - oth[i].sy / oth[i].sx) < 0.08f * asp;
            } while (clash);
            oth[k] = lens_of(rnd(20, 120), asp);
        }
        for (i = 0; i < n; ++i) {
            HeadLens l = i < n_main ? main_l : oth[(i - n_main) % others];   /* others split evenly: fewer each */
            l.sx *= 1.0f + rnd(-0.002f, 0.002f);
            l.sy *= 1.0f + rnd(-0.002f, 0.002f);
            s[i] = l;
        }
        for (i = n - 1; i > 0; --i) { int j = rand() % (i + 1); HeadLens x = s[i]; s[i] = s[j]; s[j] = x; }
        g = head_lens_pick_main(s, n, TOL, &got);
        check(g >= n_main, "random: group holds every main sample");
        check(fabsf(got.sx / main_l.sx - 1.0f) < 0.01f && fabsf(got.sy / main_l.sy - 1.0f) < 0.01f, "random: main lens");
        check(inside(&got, &main_l), "random: main draws turned");
        old = old_median(s, n);
        if (!inside(&old, &main_l)) ++old_wrong;
    }
    /* 3. One camera only: same as before (nothing changes for a clean frame like the dev PC's). */
    {
        HeadLens s[20], got, l = lens_of(60.0f, 1.778f);
        int i;
        for (i = 0; i < 20; ++i) s[i] = l;
        check(head_lens_pick_main(s, 20, TOL, &got) == 20, "single camera: all in the group");
        check(fabsf(got.sy / got.sx - 1.778f) < 1e-4f, "single camera: shape kept");
        check(head_lens_pick_main(s, 0, TOL, &got) == 0, "no samples: 0");
    }
    printf("  planted: the old method left the world unturned in %d of 5000 random mixed frames\n", old_wrong);
    check(old_wrong > 0, "planted: the old method fails somewhere (the test can see the fault)");
    printf("head_pick_test: %d checks, %d failed\n", checks, fails);
    if (!fails) printf("ALL CHECKS PASSED\n");
    return fails ? 1 : 0;
}
