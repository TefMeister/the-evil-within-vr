#pragma once

#include <windows.h>

/*
 * TEWVR configuration: every TEWVR_* knob, from a FILE as well as from the
 * environment.
 *
 * WHY THIS EXISTS (2026-09-04)
 * Every knob the proxy has (TEWVR_TEST_YAW, TEWVR_DUMP, TEWVR_SHADERDUMP,
 * TEWVR_SEQDUMP, TEWVR_SEQDUMP_ARMFILE, TEWVR_CBPEEK, TEWVR_FINDCAM,
 * TEWVR_FRAMECAPTURE, TEWVR_TARGETSIZE, TEWVR_TARGETOFF) used to be read
 * straight from the process environment at start-up, and nowhere else. That
 * made all of them hostage to HOW the game happened to be launched: a
 * launch from Steam - the natural way to start a game - inherits Steam's
 * environment, not the shell's, so the yaw test silently ran as identity
 * three launches in a row. Asking for a fourth specially-launched run is the
 * wrong fix; a file the proxy reads itself is the right one.
 *
 * WHAT IT DOES
 * tewvr_getenv() is a drop-in replacement for GetEnvironmentVariableA with
 * the same contract (returns the value length excluding the terminator, or
 * 0 when the variable is not set; a return >= size means "did not fit"). It
 * consults, in this order, the FIRST source that defines the key winning:
 *
 *   1. the process environment (so launch-yaw-test.bat and every existing
 *      script keep working exactly as before, and can override a file);
 *   2. tewvr.ini beside the game executable (the file a user editing THIS
 *      install expects to find, next to winmm.dll);
 *   3. %LOCALAPPDATA%\TEWVR\tewvr.ini (beside the logs, per user).
 *
 * FILE FORMAT - deliberately minimal, one setting per line:
 *
 *   # comment (';' works too)
 *   TEST_YAW = 90
 *   TEWVR_DUMP=1
 *
 * Keys are case-insensitive and the TEWVR_ prefix is optional in the file
 * (TEST_YAW and TEWVR_TEST_YAW are the same key). Whitespace around key and
 * value is trimmed; the value is everything up to the end of the line, so
 * "1 # on" is the value "1 # on" - put comments on their own line. An
 * empty value ("TEST_YAW=") counts as unset, so a line can be neutralised
 * without deleting it. Lines that do not parse are logged and skipped;
 * they never abort the load.
 *
 * WHEN IT IS READ
 * config_load() runs once, from DllMain(DLL_PROCESS_ATTACH), right after
 * log_init(). Both files are read then, and never again - the semantics are
 * exactly the environment's ("read at process start"), just from a file
 * that survives however the process was started. Uses only kernel32 file
 * APIs, which are safe under the loader lock (the logger already opens its
 * file there). Every tewvr_getenv() call after that is a read-only table
 * lookup, so it is thread-safe from any thread.
 *
 * WHAT IS LOGGED
 * config_load() logs which files were found and how many settings each
 * contributed. tewvr_getenv() logs, once per key, where a FOUND value came
 * from ("config: TEWVR_TEST_YAW=90 (from D:\...\tewvr.ini)") - so a run's
 * tewvr.log states unambiguously which source armed which knob. Keys found
 * nowhere are not logged (most knobs are unset on most runs).
 */

/* Read both config files. Call once, early, single-threaded. Safe to call
 * again (no-op after the first). Never fails: a missing file is the normal
 * case and simply contributes nothing. */
void config_load(void);

/* GetEnvironmentVariableA-compatible lookup across environment + files, see
 * above. `name` is the full TEWVR_* name as the callers already use it. */
DWORD tewvr_getenv(const char *name, char *buf, DWORD size);

/* Parse one file into the table (additive: keys already present from an
 * earlier, higher-priority source are NOT overwritten). Returns the number
 * of settings taken from the file, or -1 if it could not be opened. Public
 * only so tools/config_test.c can exercise the parser on a scratch file
 * without a running game; config_load() is the production entry point. */
int config_load_file(const wchar_t *path);

/* Test hook for tools/config_test.c: forget everything loaded so far. */
void config_reset_for_test(void);
