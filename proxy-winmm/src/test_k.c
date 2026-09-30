#include "test_k.h"

#include <stdlib.h>
#include <windows.h>

#include "config.h"
#include "log.h"

/* Default aspect for TEST_ROLL: the 16:9 the dev PC and the 1280x720
 * window both use. */
#define TEST_K_DEFAULT_ASPECT (16.0f / 9.0f)

Mat4 test_k_build(float yaw_deg, float roll_deg, float shift_ndc, float aspect) {
    Mat4 k = mat4_identity();

    if (yaw_deg != 0.0f) {
        k = mat4_mul(mat4_rotation_y(yaw_deg), k);
    }
    if (roll_deg != 0.0f) {
        k = mat4_mul(mat4_clip_roll(roll_deg, aspect), k);
    }
    if (shift_ndc != 0.0f) {
        k = mat4_mul(mat4_clip_shift_x(shift_ndc), k);
    }
    return k;
}

/* One knob as a float; 0 when unset, empty or too long. */
static float read_knob(const char *name, float fallback) {
    char buf[64];
    DWORD len = tewvr_getenv(name, buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) {
        return fallback;
    }
    return (float)atof(buf);
}

Mat4 test_k_from_config(void) {
    float yaw = read_knob("TEWVR_TEST_YAW", 0.0f);
    float roll = read_knob("TEWVR_TEST_ROLL", 0.0f);
    float shift = read_knob("TEWVR_TEST_SHIFT", 0.0f);
    float aspect = read_knob("TEWVR_TEST_ASPECT", TEST_K_DEFAULT_ASPECT);

    if (!(aspect > 0.0f)) {
        aspect = TEST_K_DEFAULT_ASPECT;
    }

    if (yaw == 0.0f && roll == 0.0f && shift == 0.0f) {
        log_msg("mvp_patch: TEST_YAW/TEST_ROLL/TEST_SHIFT all unset/0 (neither the environment nor "
                "tewvr.ini set them) -> K = identity (read/patch/rebind mechanism still exercised "
                "every patchable draw; no visible change expected)");
        return mat4_identity();
    }

    log_msg("mvp_patch: test matrix K ACTIVE (every patchable draw moved): yaw=%.3f deg, "
            "roll=%.3f deg (aspect %.4f), shift=%.4f ndc",
            (double)yaw, (double)roll, (double)aspect, (double)shift);
    if (yaw != 0.0f) {
        log_msg("mvp_patch: note - TEST_YAW is a clip-space yaw: it squeezes the frame into "
                "vertical slivers and clips much of the world. Use TEST_ROLL or TEST_SHIFT for a "
                "readable picture.");
    }
    return test_k_build(yaw, roll, shift, aspect);
}
