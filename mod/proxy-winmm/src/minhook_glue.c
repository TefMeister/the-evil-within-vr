#include "minhook_glue.h"
#include "MinHook.h"
#include "log.h"

static int g_mh_initialized = 0;

int mh_glue_init(void) {
    MH_STATUS st;

    if (g_mh_initialized) {
        return 1;
    }

    st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log_msg("minhook_glue: MH_Initialize failed: %s", MH_StatusToString(st));
        return 0;
    }

    g_mh_initialized = 1;
    return 1;
}

int mh_glue_create_and_enable(void *target, void *detour, void **original, const char *name) {
    MH_STATUS st;

    st = MH_CreateHook(target, detour, original);
    if (st != MH_OK) {
        log_msg("minhook_glue: MH_CreateHook(%s) failed: %s", name, MH_StatusToString(st));
        return 0;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK) {
        log_msg("minhook_glue: MH_EnableHook(%s) failed: %s", name, MH_StatusToString(st));
        MH_RemoveHook(target);
        return 0;
    }

    log_msg("minhook_glue: hook installed and enabled: %s", name);
    return 1;
}

void mh_glue_shutdown(void) {
    if (!g_mh_initialized) {
        return;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_mh_initialized = 0;
}
