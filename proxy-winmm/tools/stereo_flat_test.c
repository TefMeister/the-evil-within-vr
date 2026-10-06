/* stereo_flat_test.c - host test for stereo_afr.c's "menus and HUD stay mono" rule (2026-10-06, reader).
 * Ground truth: a perspective MVP (P*V*M, P's last row [0,0,1,0]) must get this frame's eye matrix; an
 * orthographic/2D MVP (last row [0,0,0,1]) must get the mono K alone, i.e. come out untouched when no test K
 * is set. Links the REAL stereo_afr.c with two stubs (log_msg, tewvr_getenv) standing in for the proxy.
 * Build (from tools/):
 *   gcc -O2 -I../src -o stereo_flat_test.exe stereo_flat_test.c ../src/stereo_afr.c ../src/camera.c -lm */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "stereo_afr.h"

#define TEST_K       2.0f    /* the live run's strength (2026-10-06) */
#define TEST_C       0.98f   /* the live run's convergence */
#define TRIALS       2000
#define NEAR_PLANE   0.1f

static int g_stereo_knob = 1;
void log_msg(const char *fmt, ...) { (void)fmt; }
DWORD tewvr_getenv(const char *name, char *buf, DWORD size) {
    const char *v = NULL;
    if (!strcmp(name, "TEWVR_STEREO")) v = g_stereo_knob ? "1" : "0";
    else if (!strcmp(name, "TEWVR_STEREO_K")) v = "2.0";
    else if (!strcmp(name, "TEWVR_STEREO_C")) v = "0.98";
    if (!v) return 0;
    strncpy(buf, v, size); buf[size - 1] = 0;
    return (DWORD)strlen(buf);
}

static int fails, checks;
static void check(int ok, const char *what) { ++checks; if (!ok) { ++fails; if (fails < 10) printf("  FAIL %s\n", what); } }
static float rnd(float lo, float hi) { return lo + (hi - lo) * (float)rand() / RAND_MAX; }

/* random rigid view*model with a uniform scale, as an affine 4x4 (last row 0 0 0 1) */
static Mat4 random_affine(void) {
    Mat4 r = mat4_identity();
    float a = rnd(0, 6.283f), b = rnd(-1.5f, 1.5f), s = rnd(0.01f, 50.0f);
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    float R[3][3] = { { ca, 0, sa }, { sa * sb, cb, -ca * sb }, { -sa * cb, sb, ca * cb } };
    int i, j;
    for (i = 0; i < 3; ++i) { for (j = 0; j < 3; ++j) r.m[i][j] = s * R[i][j]; }
    r.m[0][3] = rnd(-50, 50); r.m[1][3] = rnd(-50, 50); r.m[2][3] = rnd(1, 900);
    return r;
}
static Mat4 perspective(void) {
    Mat4 p; float f = rnd(10, 5000), A = f / (f - NEAR_PLANE), sy = rnd(0.5f, 2.5f);
    memset(&p, 0, sizeof p);
    p.m[0][0] = sy / rnd(1.3f, 2.4f); p.m[1][1] = sy; p.m[2][2] = A; p.m[2][3] = -NEAR_PLANE * A; p.m[3][2] = 1.0f;
    return p;
}
static Mat4 ortho(void) {   /* 2D screen ortho, id Tech GUI style: pixels -> NDC */
    Mat4 p = mat4_identity(); float w = rnd(640, 3840), h = rnd(360, 2160);
    p.m[0][0] = 2.0f / w; p.m[0][3] = -1.0f; p.m[1][1] = -2.0f / h; p.m[1][3] = 1.0f; p.m[2][2] = rnd(0, 1);
    return p;
}
static int same(Mat4 a, Mat4 b) { return !memcmp(&a, &b, sizeof a); }

int main(void) {
    int t, e;
    Mat4 id = mat4_identity();
    srand(11);
    stereo_afr_init(id);
    check(stereo_mvp_is_flat(&id) == 1, "identity MVP is flat");
    for (t = 0; t < TRIALS; ++t) {
        Mat4 vm = random_affine(), persp = mat4_mul(perspective(), vm), flat = mat4_mul(ortho(), vm);
        check(!stereo_mvp_is_flat(&persp), "perspective MVP not flat");
        check(stereo_mvp_is_flat(&flat), "orthographic MVP flat");
        for (e = 0; e < 2; ++e) {
            float s = stereo_afr_current_k()->m[0][2] > 0 ? 1.0f : -1.0f;
            Mat4 want = mat4_mul(stereo_eye_k(s, TEST_K, TEST_C), persp);
            check(same(mat4_mul(*stereo_afr_k_for(&persp), persp), want), "perspective gets this eye's K");
            check(!same(want, persp), "the eye K really moves a perspective draw");
            check(same(mat4_mul(*stereo_afr_k_for(&flat), flat), flat), "orthographic draw left untouched");
            stereo_afr_on_present();
        }
    }
    {   /* planted fault: a perspective draw must NOT be taken for flat even when tiny and far */
        Mat4 vm = mat4_identity(), p = perspective(), m;
        vm.m[0][0] = vm.m[1][1] = vm.m[2][2] = 0.001f; vm.m[2][3] = 5000.0f;
        m = mat4_mul(p, vm);
        check(!stereo_mvp_is_flat(&m), "tiny far perspective object still 3D");
    }
    printf("stereo_flat_test: %d checks, %d failed\n", checks, fails);
    if (!fails) printf("ALL CHECKS PASSED\n");
    return fails ? 1 : 0;
}
