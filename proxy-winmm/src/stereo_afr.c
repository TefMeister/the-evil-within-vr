#include "stereo_afr.h"

#include <stdlib.h>
#include <windows.h>

#include "config.h"
#include "log.h"

#define STEREO_DEFAULT_K   0.02f   /* strength: about a 2% screen shift between eyes at the near plane scale */
#define STEREO_DEFAULT_C   1.0f    /* convergence: 1 = A for an infinite far plane, i.e. converge at infinity */
#define STEREO_LOG_EVERY   600u    /* Presents between progress lines */

static Mat4 g_k_mono;              /* the test K alone, used while stereo is off */
static Mat4 g_k_eye[2];            /* K_eye * K_test for left [0] and right [1] */
static const Mat4 *volatile g_k_cur = &g_k_mono;
static int g_on;
static int g_right;
static unsigned long g_presents;

Mat4 stereo_eye_k(float eye_sign, float k, float c) {
    Mat4 r = mat4_identity();
    r.m[0][2] = eye_sign * k;          /* x' = x + s*k*z ... */
    r.m[0][3] = -eye_sign * k * c;     /*            ... - s*k*c*w */
    return r;
}

static float read_knob(const char *name, float fallback) {
    char buf[64];
    DWORD len = tewvr_getenv(name, buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) return fallback;
    return (float)atof(buf);
}

void stereo_afr_init(Mat4 test_k) {
    float k, c;
    g_k_mono = test_k;
    g_k_cur = &g_k_mono;
    g_on = read_knob("TEWVR_STEREO", 0.0f) != 0.0f;
    if (!g_on) {
        log_msg("stereo_afr: STEREO unset/0 -> one view (the test K alone)");
        return;
    }
    k = read_knob("TEWVR_STEREO_K", STEREO_DEFAULT_K);
    c = read_knob("TEWVR_STEREO_C", STEREO_DEFAULT_C);
    g_k_eye[0] = mat4_mul(stereo_eye_k(-1.0f, k, c), test_k);
    g_k_eye[1] = mat4_mul(stereo_eye_k(+1.0f, k, c), test_k);
    g_right = 0;
    g_k_cur = &g_k_eye[0];
    log_msg("stereo_afr: ON - alternate frames, k=%.4f c=%.4f; the eye flips at every Present", (double)k, (double)c);
}

void stereo_afr_on_present(void) {
    if (!g_on) return;
    g_right ^= 1;
    InterlockedExchangePointer((PVOID volatile *)&g_k_cur, (PVOID)&g_k_eye[g_right]);
    if ((g_presents++ % STEREO_LOG_EVERY) == 0)
        log_msg("stereo_afr: %lu Present(s); next frame is the %s eye", g_presents, g_right ? "RIGHT" : "LEFT");
}

const Mat4 *stereo_afr_current_k(void) {
    return g_k_cur;
}
