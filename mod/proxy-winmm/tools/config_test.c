/*
 * Offline test for src/config.c - the tewvr.ini reader - so the parser and
 * the environment-over-file precedence are verified without a running game.
 *
 * Links the SHIPPED config.c (not a transcription of it) against a stub
 * logger that prints to stdout. Writes a scratch tewvr.ini under %TEMP%,
 * loads it, and checks every rule config.h promises:
 *
 *   - '#' and ';' comments, blank lines, CRLF line ends, a UTF-8 BOM
 *   - key case-insensitivity and the optional TEWVR_ prefix
 *   - whitespace trimming around key and value
 *   - "KEY=" (empty value) counts as unset
 *   - first definition wins, within a file and across two files
 *   - a line without '=' is skipped, not fatal
 *   - the environment beats the file
 *   - the GetEnvironmentVariableA contract: 0 when unset, the needed size
 *     when the buffer is too small, the length otherwise
 *
 * Build (from proxy-winmm/, same toolchain as build.ps1):
 *   gcc -Wall -Wextra -O2 -o build\config_test.exe tools\config_test.c src\config.c
 * Run:
 *   build\config_test.exe        -> prints one line per check, exit 0 = all pass
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "../src/config.h"

/* stub logger - the real log.c writes into %LOCALAPPDATA%\TEWVR\tewvr.log,
 * which a test must not pollute */
void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("   [log] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static int g_fail = 0, g_pass = 0;

static void check(int cond, const char *what) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", what);
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
    }
}

static void write_file(const wchar_t *path, const char *content) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    if (h == INVALID_HANDLE_VALUE) {
        printf("cannot create %ls\n", path);
        exit(2);
    }
    WriteFile(h, content, (DWORD)strlen(content), &w, NULL);
    CloseHandle(h);
}

int main(void) {
    wchar_t dir[MAX_PATH], f1[MAX_PATH], f2[MAX_PATH], missing[MAX_PATH];
    char buf[64];
    DWORD n;
    int r;

    GetTempPathW(MAX_PATH, dir);
    swprintf(f1, MAX_PATH, L"%stewvr-config-test-1.ini", dir);
    swprintf(f2, MAX_PATH, L"%stewvr-config-test-2.ini", dir);
    swprintf(missing, MAX_PATH, L"%stewvr-config-test-does-not-exist.ini", dir);

    /* file 1: BOM, CRLF, comments, both prefix forms, mixed case, spacing,
     * an empty value, a duplicate, and a junk line */
    write_file(f1,
               "\xEF\xBB\xBF# The Evil Within VR proxy settings\r\n"
               "; semicolon comment too\r\n"
               "\r\n"
               "  test_yaw   =   90  \r\n"
               "TEWVR_DUMP=1\r\n"
               "Tewvr_FindCam = 1\r\n"
               "TARGETSIZE=\r\n"          /* empty value: unset */
               "TEST_YAW=45\r\n"          /* duplicate: first wins */
               "this line has no equals sign\r\n"
               "SPACED = a value with spaces\r\n"
               "LONGKEYNAME_THAT_IS_FAR_TOO_LONG_FOR_THE_TABLE_TO_ACCEPT_AT_ALL=1\r\n");
    /* file 2: lower priority - one new key, one that file 1 already set */
    write_file(f2,
               "SEQDUMP=1\n"
               "TEST_YAW=180\n");

    SetEnvironmentVariableA("TEWVR_TEST_YAW", NULL);
    SetEnvironmentVariableA("TEWVR_CBPEEK", NULL);
    SetEnvironmentVariableA("TEWVR_DUMP", NULL);
    SetEnvironmentVariableA("TEWVR_SEQDUMP", NULL);

    config_reset_for_test();
    r = config_load_file(missing);
    check(r == -1, "a missing file returns -1 and is not fatal");
    r = config_load_file(f1);
    check(r == 4, "file 1 contributes exactly 4 settings (TEST_YAW, DUMP, FINDCAM, SPACED)");
    r = config_load_file(f2);
    check(r == 1, "file 2 contributes exactly 1 (SEQDUMP; its TEST_YAW loses to file 1)");

    n = tewvr_getenv("TEWVR_TEST_YAW", buf, sizeof(buf));
    check(n == 2 && strcmp(buf, "90") == 0, "TEST_YAW = 90: lower-case key, spaces trimmed, first definition wins");
    n = tewvr_getenv("TEWVR_DUMP", buf, sizeof(buf));
    check(n == 1 && strcmp(buf, "1") == 0, "DUMP = 1: TEWVR_ prefix in the file is accepted");
    n = tewvr_getenv("TEWVR_FINDCAM", buf, sizeof(buf));
    check(n == 1 && strcmp(buf, "1") == 0, "FINDCAM = 1: mixed-case key and prefix");
    n = tewvr_getenv("TEWVR_TARGETSIZE", buf, sizeof(buf));
    check(n == 0, "TARGETSIZE= (empty value) reads as unset");
    n = tewvr_getenv("TEWVR_SPACED", buf, sizeof(buf));
    check(n == 19 && strcmp(buf, "a value with spaces") == 0, "a value keeps its interior spaces");
    n = tewvr_getenv("TEWVR_SEQDUMP", buf, sizeof(buf));
    check(n == 1 && strcmp(buf, "1") == 0, "SEQDUMP = 1 comes from the second file");
    n = tewvr_getenv("TEWVR_CBPEEK", buf, sizeof(buf));
    check(n == 0, "a key set nowhere returns 0");
    n = tewvr_getenv("TEWVR_SPACED", buf, 4);
    check(n == 20, "too-small buffer returns the size needed (len+1), like GetEnvironmentVariableA");

    /* environment beats the file */
    SetEnvironmentVariableA("TEWVR_TEST_YAW", "7");
    n = tewvr_getenv("TEWVR_TEST_YAW", buf, sizeof(buf));
    check(n == 1 && strcmp(buf, "7") == 0, "the environment overrides the file for the same key");
    SetEnvironmentVariableA("TEWVR_TEST_YAW", NULL);
    n = tewvr_getenv("TEWVR_TEST_YAW", buf, sizeof(buf));
    check(n == 2 && strcmp(buf, "90") == 0, "...and the file value is back once the variable is cleared");

    /* the exact call shape the proxy uses for the yaw */
    {
        char flagbuf[32];
        DWORD len = tewvr_getenv("TEWVR_TEST_YAW", flagbuf, sizeof(flagbuf));
        float deg = (len > 0 && len < sizeof(flagbuf)) ? (float)atof(flagbuf) : 0.0f;
        check(deg == 90.0f, "mvp_patch's own read shape yields 90.0 degrees");
    }

    DeleteFileW(f1);
    DeleteFileW(f2);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
