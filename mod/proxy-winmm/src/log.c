#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

static CRITICAL_SECTION g_log_cs;
static FILE *g_log_fp = NULL;
static volatile LONG g_log_cs_ready = 0;

void log_init(void) {
    /* Idempotency guard only - log_init() is only ever actually called
       once, single-threaded, from DLL_PROCESS_ATTACH. A plain read here is
       fine; it is NOT what protects log_msg()/log_shutdown() on other
       threads - that protection is g_log_cs_ready being set only after
       InitializeCriticalSection() has fully completed, below. */
    if (g_log_cs_ready) {
        return; /* already initialised */
    }

    /* Critical section must be valid before g_log_cs_ready can ever read
       as non-zero on another thread, so this runs first. */
    InitializeCriticalSection(&g_log_cs);

    wchar_t local_appdata[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local_appdata, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return;
    }

    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%s\\TEWVR", local_appdata);

    /* Create the TEWVR directory if it does not already exist. */
    if (!CreateDirectoryW(dir, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return;
        }
    }

    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\tewvr.log", dir);

    EnterCriticalSection(&g_log_cs);
    g_log_fp = _wfopen(path, L"a");
    LeaveCriticalSection(&g_log_cs);

    /* Publish readiness LAST, after the critical section and file are both
       fully set up, so no other thread can observe g_log_cs_ready != 0
       before g_log_cs is safe to enter. */
    InterlockedExchange(&g_log_cs_ready, 1);
}

void log_msg(const char *fmt, ...) {
    if (!g_log_cs_ready) {
        return;
    }
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) {
        ULONGLONG ms = GetTickCount64();
        DWORD tid = GetCurrentThreadId();
        fprintf(g_log_fp, "[%llu][tid=%lu] ", (unsigned long long)ms, (unsigned long)tid);

        va_list args;
        va_start(args, fmt);
        vfprintf(g_log_fp, fmt, args);
        va_end(args);

        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
    }
    LeaveCriticalSection(&g_log_cs);
}

void log_shutdown(void) {
    /* Flip the ready flag off FIRST (atomically, so exactly one caller
       proceeds even if log_shutdown() were ever called from two threads),
       so any log_msg() call that starts after this point sees ready==0 and
       skips g_log_cs entirely - it must never be entered once teardown has
       begun. */
    if (InterlockedExchange(&g_log_cs_ready, 0) == 0) {
        return; /* never initialised, or already shut down */
    }
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    LeaveCriticalSection(&g_log_cs);
    /* Delete the critical section LAST, once we are certain no in-flight
       log_msg() call (one that had already passed the ready check just
       before this function flipped it) can still be inside it - the
       Enter/LeaveCriticalSection pair above drains any such caller first. */
    DeleteCriticalSection(&g_log_cs);
}
