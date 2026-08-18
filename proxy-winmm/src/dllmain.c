#include <windows.h>
#include "log.h"
#include "winmm_forward.h"
#include "hooks.h"

/*
 * Waits (briefly, with short polling sleeps) for d3d11.dll to be loaded by
 * the game, then installs the Present hook. Runs on its own thread, spawned
 * from DLL_PROCESS_ATTACH - hooks_install() creates a D3D11 device, which
 * is not safe to do while the loader lock is held inside DllMain itself.
 */
static DWORD WINAPI bootstrap_thread(LPVOID param) {
    int i;
    (void)param;

    for (i = 0; i < 300; ++i) { /* ~30s cap at 100ms polls */
        if (GetModuleHandleW(L"d3d11.dll") != NULL) {
            break;
        }
        Sleep(100);
    }

    if (GetModuleHandleW(L"d3d11.dll") == NULL) {
        log_msg("bootstrap: d3d11.dll never loaded after ~30s wait; skipping hook install");
        return 0;
    }

    hooks_install();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        log_init();              /* logger up first, so winmm_forward_init()'s own
                                     failure diagnostics (log_msg calls) are never lost */
        winmm_forward_init();    /* resolve real winmm */
        log_msg("TEWVR winmm proxy attached (pid=%lu)", GetCurrentProcessId());

        {
            HANDLE h = CreateThread(NULL, 0, bootstrap_thread, NULL, 0, NULL);
            if (h) {
                CloseHandle(h); /* detached; we don't need to join it */
            } else {
                log_msg("dllmain: CreateThread for bootstrap failed (gle=%lu)", GetLastError());
            }
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        log_msg("TEWVR winmm proxy detaching");
        hooks_remove();  /* must run before log_shutdown(): stops any further
                             log_msg() calls from the render thread before the
                             logger's critical section goes away */
        log_shutdown();
    }
    return TRUE;
}
