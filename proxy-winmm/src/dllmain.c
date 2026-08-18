#include <windows.h>
#include "log.h"
#include "winmm_forward.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        winmm_forward_init();   /* resolve real winmm first, so forwards work immediately */
        log_init();
        log_msg("TEWVR winmm proxy attached (pid=%lu)", GetCurrentProcessId());
    } else if (reason == DLL_PROCESS_DETACH) {
        log_msg("TEWVR winmm proxy detaching");
        log_shutdown();
    }
    return TRUE;
}
