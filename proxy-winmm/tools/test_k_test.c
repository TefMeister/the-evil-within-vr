/*
 * test_k_test.c - checks the test matrices K against the SHIPPED maths
 * (it compiles ../src/camera.c itself, not a transcription).
 *
 *   gcc -O2 -Wall -Wextra -Wpedantic -o test_k_test.exe tools/test_k_test.c src/camera.c -lm
 *
 * What it shows:
 *   1. TEST_YAW=90 applied after projection squeezes every visible point of a
 *      normal 3D scene into a narrow vertical strip and throws much of it out
 *      of the depth range - the 2026-09-08 "slivers" and black world.
 *   2. A flat menu quad (clip z constant) under TEST_YAW=90 lands in a
 *      single column.
 *   3. TEST_ROLL tilts rigidly on a 16:9 screen (pixel distances kept),
 *      and leaves depth and w untouched, so nothing new is clipped.
 *   4. TEST_SHIFT moves every point by exactly the same screen amount,
 *      near or far, and leaves y, depth and w untouched.
 *   5. The test_k_build order: yaw first, then roll, then shift.
 *
 * test_k_build() is re-stated here from src/test_k.c because that file also
 * pulls in the proxy's logging and config; the builders it calls are the
 * shipped ones.
 */
#include <math.h>
#include <stdio.h>

#include "../src/camera.h"

static int g_fail = 0;
static int g_pass = 0;

static void check(int ok, const char *what) {
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

typedef struct { float v[4]; } Vec4;

static Vec4 xform(const Mat4 *m, Vec4 p) {
    Vec4 r;
    int i;
    for (i = 0; i < 4; i++) {
        r.v[i] = m->m[i][0] * p.v[0] + m->m[i][1] * p.v[1] + m->m[i][2] * p.v[2] + m->m[i][3] * p.v[3];
    }
    return r;
}

/* D3D-style left-handed perspective, depth 0..1, column-vector rows. */
static Mat4 persp(float fovy_deg, float aspect, float n, float f, int reversed) {
    Mat4 p;
    float ys = 1.0f / (float)tan((double)fovy_deg * 3.14159265358979323846 / 360.0);
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            p.m[i][j] = 0.0f;
        }
    }
    p.m[0][0] = ys / aspect;
    p.m[1][1] = ys;
    if (!reversed) {
        p.m[2][2] = f / (f - n);
        p.m[2][3] = -n * f / (f - n);
    } else {
        p.m[2][2] = n / (n - f);
        p.m[2][3] = -n * f / (n - f);
    }
    p.m[3][2] = 1.0f;
    return p;
}

static int visible(Vec4 c) {
    return c.v[3] > 0.0f && fabsf(c.v[0]) <= c.v[3] && fabsf(c.v[1]) <= c.v[3] && c.v[2] >= 0.0f &&
           c.v[2] <= c.v[3];
}

/* Mirrors src/test_k.c's test_k_build(). */
static Mat4 build(float yaw, float roll, float shift, float aspect) {
    Mat4 k = mat4_identity();
    if (yaw != 0.0f) k = mat4_mul(mat4_rotation_y(yaw), k);
    if (roll != 0.0f) k = mat4_mul(mat4_clip_roll(roll, aspect), k);
    if (shift != 0.0f) k = mat4_mul(mat4_clip_shift_x(shift), k);
    return k;
}

static void yaw90_scene(int reversed) {
    const float aspect = 16.0f / 9.0f;
    Mat4 p = persp(60.0f, aspect, 0.1f, 1000.0f, reversed);
    Mat4 k = mat4_rotation_y(90.0f);
    int total = 0, before = 0, after = 0;
    float minx = 1e9f, maxx = -1e9f;
    int ix, iy, iz;
    char msg[160];

    /* A grid of points spread over the view: 2 m to 200 m ahead, across the
     * full field of view. */
    for (iz = 0; iz < 40; iz++) {
        float z = 2.0f + (float)iz * 5.0f;
        for (iy = -10; iy <= 10; iy++) {
            for (ix = -10; ix <= 10; ix++) {
                Vec4 v = {{(float)ix / 10.0f * z * 1.0f, (float)iy / 10.0f * z * 0.55f, z, 1.0f}};
                Vec4 c = xform(&p, v);
                Vec4 c2 = xform(&k, c);
                total++;
                if (visible(c)) before++;
                if (visible(c2)) {
                    float sx = c2.v[0] / c2.v[3];
                    after++;
                    if (sx < minx) minx = sx;
                    if (sx > maxx) maxx = sx;
                }
            }
        }
    }
    printf("yaw90 %s: %d points, %d on screen before, %d after; surviving x/w spans %.4f..%.4f "
           "(%.2f%% of the screen width)\n",
           reversed ? "reversed-Z" : "normal-Z", total, before, after, (double)minx, (double)maxx,
           (double)((maxx - minx) / 2.0f * 100.0f));
    snprintf(msg, sizeof msg, "yaw90 (%s) loses at least a third of the visible points",
             reversed ? "reversed" : "normal");
    check(after * 3 <= before * 2, msg);
    snprintf(msg, sizeof msg, "yaw90 (%s) squeezes the survivors into under 5%% of the width",
             reversed ? "reversed" : "normal");
    check(after == 0 || (maxx - minx) / 2.0f < 0.05f, msg);
}

static void yaw90_menu(void) {
    Mat4 k = mat4_rotation_y(90.0f);
    float first = 0.0f;
    int i, same = 1;
    /* A flat menu: clip z constant (0.5), w = 1, x/y spread over the screen. */
    for (i = 0; i < 25; i++) {
        Vec4 c = {{-0.9f + 0.075f * (float)i, 0.3f, 0.5f, 1.0f}};
        Vec4 c2 = xform(&k, c);
        float sx = c2.v[0] / c2.v[3];
        if (i == 0) first = sx;
        else if (fabsf(sx - first) > 1e-5f) same = 0;
    }
    printf("yaw90 menu: every x lands at x/w = %.4f\n", (double)first);
    check(same, "yaw90 puts a whole flat menu row in one screen column");
}

static void roll_checks(void) {
    const float aspect = 16.0f / 9.0f;
    Mat4 k = mat4_clip_roll(15.0f, aspect);
    Vec4 a = {{0.4f, 0.2f, 0.7f, 2.0f}}, b = {{-0.3f, -0.5f, 0.3f, 1.5f}};
    Vec4 a2 = xform(&k, a), b2 = xform(&k, b);
    /* pixel-proportional coordinates: X = (x/w) * aspect, Y = y/w */
    float ax = a.v[0] / a.v[3] * aspect, ay = a.v[1] / a.v[3];
    float bx = b.v[0] / b.v[3] * aspect, by = b.v[1] / b.v[3];
    float a2x = a2.v[0] / a2.v[3] * aspect, a2y = a2.v[1] / a2.v[3];
    float b2x = b2.v[0] / b2.v[3] * aspect, b2y = b2.v[1] / b2.v[3];
    float d1 = hypotf(ax - bx, ay - by), d2 = hypotf(a2x - b2x, a2y - b2y);
    float ang1 = atan2f(ay, ax), ang2 = atan2f(a2y, a2x);
    float turned = (ang2 - ang1) * 180.0f / 3.14159265f;

    printf("roll15: on-screen distance %.5f -> %.5f, point turned %.4f deg\n", (double)d1, (double)d2,
           (double)turned);
    check(fabsf(d1 - d2) < 1e-5f, "roll keeps on-screen distances on a 16:9 screen (rigid tilt)");
    check(fabsf(turned - 15.0f) < 1e-3f, "roll turns the picture by the asked angle, counter-clockwise");
    check(a2.v[2] == a.v[2] && a2.v[3] == a.v[3], "roll leaves depth and w untouched");
}

static void shift_checks(void) {
    Mat4 k = mat4_clip_shift_x(0.1f);
    Vec4 nearp = {{0.2f, 0.1f, 0.05f, 0.5f}}, farp = {{-30.0f, 12.0f, 99.0f, 100.0f}};
    Vec4 n2 = xform(&k, nearp), f2 = xform(&k, farp);
    float dn = n2.v[0] / n2.v[3] - nearp.v[0] / nearp.v[3];
    float df = f2.v[0] / f2.v[3] - farp.v[0] / farp.v[3];
    printf("shift0.1: near moved %.6f, far moved %.6f\n", (double)dn, (double)df);
    check(fabsf(dn - 0.1f) < 1e-5f && fabsf(df - 0.1f) < 1e-5f, "shift moves near and far by the same amount");
    check(n2.v[1] == nearp.v[1] && n2.v[2] == nearp.v[2] && n2.v[3] == nearp.v[3],
          "shift leaves y, depth and w untouched");
}

static void build_checks(void) {
    const float aspect = 16.0f / 9.0f;
    Mat4 id = build(0.0f, 0.0f, 0.0f, aspect), k, want;
    int i, j, is_id = 1, same = 1;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            if (id.m[i][j] != (i == j ? 1.0f : 0.0f)) is_id = 0;
    check(is_id, "all knobs 0 gives identity");

    k = build(30.0f, 10.0f, 0.05f, aspect);
    want = mat4_mul(mat4_clip_shift_x(0.05f), mat4_mul(mat4_clip_roll(10.0f, aspect), mat4_rotation_y(30.0f)));
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            if (fabsf(k.m[i][j] - want.m[i][j]) > 1e-6f) same = 0;
    check(same, "K = Shift * Roll * Yaw (yaw applied first)");
}

int main(void) {
    yaw90_scene(0);
    yaw90_scene(1);
    yaw90_menu();
    roll_checks();
    shift_checks();
    build_checks();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
