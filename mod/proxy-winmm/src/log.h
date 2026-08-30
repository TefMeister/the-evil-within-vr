#pragma once

/*
 * TEWVR logger.
 *
 * Writes timestamped, thread-tagged lines to
 *   %LOCALAPPDATA%\TEWVR\tewvr.log
 * in append mode, flushing after every message. Safe to call from any
 * thread once log_init() has completed.
 */

void log_init(void);
void log_msg(const char *fmt, ...);
void log_shutdown(void);
