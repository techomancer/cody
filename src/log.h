#ifndef CODY_LOG_H
#define CODY_LOG_H

/*
 * log.h — diagnostic logging for Cody
 *
 * Before UI::init() is called, codylog() writes to stderr (safe during
 * startup).  After UI::init(), messages appear in the output scroll buffer
 * as dim "[DBG] ..." lines so they don't corrupt the TUI.
 *
 * Usage:
 *   codylog("readLine: fillWork failed rc=%d", rc);
 *
 * The format string and arguments follow printf conventions.
 * Long messages are truncated to LOG_LINE_MAX-1 characters.
 */

#define LOG_LINE_MAX (256)

/* Call UI::tuiReady() to signal that the TUI is up and stderr is no longer safe */
void logSetTuiReady(int ready);

#if defined(__WATCOMC__) || defined(__sgi)
/* Open Watcom and MIPSPro do not support __attribute__((format,...)) */
void codylog(const char *fmt, ...);
#else
void codylog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

#endif /* CODY_LOG_H */
