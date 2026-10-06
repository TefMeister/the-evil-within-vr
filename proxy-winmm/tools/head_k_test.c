/* head_k_test.c - host test for src/head_k.c, linked as shipped (2026-10-06, /pd).
 * Ground truth is built independently here: a random lens P, camera V and model M, MVP = P*V*M, and for a head
 * rotation R the expected clip position is P * R^T * V * M * pos. The shipped code only ever sees MVP.
 * Build (from tools/):  gcc -O2 -I../src -o head_k_test.exe head_k_test.c ../src/head_k.c ../src/camera.c -lm
 * PLANT=1 in the environment flips the head rotation (R instead of R^T): the direction checks must then fail. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "head_k.h"

static int fails, checks, plant;
static void check(int ok, const char *what) { ++checks; if (!ok) { ++fails; if (fails < 10) printf("  FAIL %s\n", what); } }
static float rnd(float a, float b) { return a + (b - a) * (float)rand() / (float)RAND_MAX; }

static Mat4 rot_axis(float ax, float ay, float az, float ang) {
    Mat4 m = mat4_identity();
    float n = sqrtf(ax * ax + ay * ay + az * az), c = cosf(ang), s = sinf(ang), t = 1 - c;
    ax /= n; ay /= n; az /= n;
    m.m[0][0] = t * ax * ax + c;      m.m[0][1] = t * ax * ay - s * az; m.m[0][2] = t * ax * az + s * ay;
    m.m[1][0] = t * ax * ay + s * az; m.m[1][1] = t * ay * ay + c;      m.m[1][2] = t * ay * az - s * ax;
    m.m[2][0] = t * ax * az - s * ay; m.m[2][1] = t * ay * az + s * ax; m.m[2][2] = t * az * az + c;
    return m;
}
static Mat4 rand_rot(void) { return rot_axis(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1) + 0.01f, rnd(-3.1f, 3.1f)); }
static void apply(const Mat4 *m, const float *v, float *o) {
    int i; for (i = 0; i < 4; ++i) o[i] = m->m[i][0] * v[0] + m->m[i][1] * v[1] + m->m[i][2] * v[2] + m->m[i][3] * v[3];
}
static Mat4 lens_p(const HeadLens *L, float b) {
    Mat4 p = mat4_identity();
    p.m[0][0] = L->sx; p.m[0][2] = L->cx; p.m[1][1] = L->sy; p.m[1][2] = L->cy;
    p.m[2][2] = L->a; p.m[2][3] = b; p.m[3][2] = 1; p.m[3][3] = 0;
    return p;
}
/* K the shipped way, or the planted mistake. */
static Mat4 build(const HeadLens *L, HeadMat3 r) {
    if (plant) { HeadMat3 t; int i, j; for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) t[i][j] = r[j][i]; return head_k_build(L, t); }
    return head_k_build(L, r);
}
/* Screen position of a world point straight ahead of the game camera, with the head turned by quaternion q. */
static void ahead_ndc(float qx, float qy, float qz, float qw, float px, float py, float *nx, float *ny) {
    HeadLens L = { 1.0f, 1.8f, 0.0f, 0.0f, 1.0f };
    Mat4 mvp = lens_p(&L, -0.1f);    /* V = M = identity: view space = world */
    HeadMat3 r; Mat4 k; float pos[4] = { px, py, 10.0f, 1.0f }, c[4];
    head_rot_from_xr_quat(qx, qy, qz, qw, r);
    k = mat4_mul(build(&L, r), mvp);
    apply(&k, pos, c);
    *nx = c[0] / c[3]; *ny = c[1] / c[3];
}

int main(void) {
    int t, rejected = 0, nonuni = 0;
    plant = getenv("PLANT") != NULL;
    srand(11);
    for (t = 0; t < 3000; ++t) {
        HeadLens truth, got;
        Mat4 p, v, m, vm, mvp, k, rr = mat4_identity(), want_m;
        HeadMat3 r;
        float s = rnd(0.01f, 50.0f), b = -rnd(0.01f, 5.0f);
        int i, j, ok;
        truth.sx = rnd(0.4f, 3.0f); truth.sy = truth.sx * rnd(0.8f, 3.0f);
        truth.cx = rnd(-0.3f, 0.3f); truth.cy = rnd(-0.3f, 0.3f); truth.a = rnd(0.95f, 1.05f);
        p = lens_p(&truth, b);
        v = rand_rot(); v.m[0][3] = rnd(-100, 100); v.m[1][3] = rnd(-100, 100); v.m[2][3] = rnd(-100, 100);
        m = rand_rot();
        for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) m.m[i][j] *= s;
        m.m[0][3] = rnd(-50, 50); m.m[1][3] = rnd(-50, 50); m.m[2][3] = rnd(-50, 50);
        vm = mat4_mul(v, m);
        mvp = mat4_mul(p, vm);
        ok = head_lens_from_mvp(&mvp, &got);
        check(ok, "lens measured from a uniformly scaled draw");
        if (!ok) continue;
        check(fabsf(got.sx - truth.sx) < 1e-3f * truth.sx && fabsf(got.sy - truth.sy) < 1e-3f * truth.sy, "sx, sy");
        check(fabsf(got.cx - truth.cx) < 1e-3f && fabsf(got.cy - truth.cy) < 1e-3f, "cx, cy");
        check(fabsf(got.a - truth.a) < 1e-3f, "A");
        /* head rotation: K * MVP must equal P * R^T * V * M */
        head_rot_from_xr_quat(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1), rnd(-1, 1), r);
        for (i = 0; i < 3; ++i) for (j = 0; j < 3; ++j) rr.m[i][j] = r[j][i];
        want_m = mat4_mul(p, mat4_mul(rr, vm));
        k = mat4_mul(head_k_build(&got, r), mvp);
        for (i = 0; i < 4; ++i) {
            float pos[4] = { rnd(-20, 20), rnd(-20, 20), rnd(-20, 20), 1.0f }, a4[4], w4[4];
            apply(&k, pos, a4); apply(&want_m, pos, w4);
            if (fabsf(w4[3]) > 0.5f) {
                float tol = 2e-3f * (1.0f + fabsf(w4[0] / w4[3]) + fabsf(w4[1] / w4[3]));
                check(fabsf(a4[0] / a4[3] - w4[0] / w4[3]) < tol && fabsf(a4[1] / a4[3] - w4[1] / w4[3]) < tol, "screen x, y match P R^T V M");
                check(fabsf(a4[2] / a4[3] - w4[2] / w4[3]) < 2e-2f * (1.0f + fabsf(w4[2] / w4[3])), "depth matches");
            }
        }
        /* no head turn = no change */
        head_rot_from_xr_quat(0, 0, 0, 1, r);
        k = head_k_build(&got, r);
        for (i = 0; i < 4; ++i) for (j = 0; j < 4; ++j)
            check(fabsf(k.m[i][j] - (i == j ? 1.0f : 0.0f)) < 1e-4f, "identity head gives identity K");
        /* an unevenly scaled model must not be believed (or, if believed, be close to the truth) */
        {
            Mat4 mn = m, mvpn; HeadLens gn;
            for (i = 0; i < 3; ++i) mn.m[i][0] *= 1.7f;
            mvpn = mat4_mul(p, mat4_mul(v, mn));
            ++nonuni;
            if (!head_lens_from_mvp(&mvpn, &gn)) ++rejected;
        }
    }
    check(rejected > nonuni * 9 / 10, "nine in ten unevenly scaled draws are rejected");
    {   /* directions, in OpenXR's own terms (right-handed, y up, -z forward) */
        float nx, ny, h = 0.2618f;   /* 30 degrees */
        ahead_ndc(0, sinf(-h / 2), 0, cosf(h / 2), 0, 0, &nx, &ny);     /* head turned RIGHT (about +y by -30) */
        check(nx < -0.1f && fabsf(ny) < 1e-4f, "head right: what was ahead moves LEFT on screen");
        ahead_ndc(sinf(h / 2), 0, 0, cosf(h / 2), 0, 0, &nx, &ny);      /* head pitched UP (about +x by +30) */
        check(ny < -0.1f && fabsf(nx) < 1e-4f, "head up: what was ahead moves DOWN on screen");
        ahead_ndc(0, 0, sinf(h / 2), cosf(h / 2), 3, 0, &nx, &ny);      /* head rolled LEFT (about +z by +30) */
        check(ny < -0.05f, "head tilted left: a point on the right of the horizon moves DOWN");
    }
    {   /* fov tangents and the letterbox */
        HeadLens L = { 1.0f, 2.35f, 0.0f, 0.0f, 1.0f };
        float l, r, u, d; int rect[4];
        check(head_lens_tangents(&L, &l, &r, &u, &d) && fabsf(l + 1) < 1e-6f && fabsf(r - 1) < 1e-6f &&
              fabsf(u - 1 / 2.35f) < 1e-6f && fabsf(d + 1 / 2.35f) < 1e-6f, "fov tangents");
        head_lens_image_rect(&L, 1280, 720, rect);
        check(rect[0] == 0 && rect[2] == 1280 && abs(rect[3] - 545) <= 1 && abs(rect[1] - 87) <= 1, "2.35:1 letterbox in 1280x720");
        L.sy = L.sx * 1280.0f / 720.0f;
        head_lens_image_rect(&L, 1280, 720, rect);
        check(rect[0] == 0 && rect[1] == 0 && rect[2] == 1280 && rect[3] == 720, "16:9 lens fills the buffer");
    }
    printf("head_k_test: %d checks, %d failed%s\n", checks, fails, plant ? " (PLANTED MISTAKE)" : "");
    if (!fails) printf("ALL CHECKS PASSED\n");
    return fails ? 1 : 0;
}
