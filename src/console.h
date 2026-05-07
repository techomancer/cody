#ifndef CODY_CONSOLE_H
#define CODY_CONSOLE_H

#include "codytypes.h"

#define CONSOLE_INPUT_LEN (256)

/* Platform-neutral console interface.
   Implemented in DOS/console_dos.cpp and UNIX/console_unix.cpp.

   Call init() once, poll() every main-loop iteration.
   When poll() returns true, inputBuf holds the complete user line (no \n). */
class Console {
public:
    static void  init();
    static void  shutdown();

    /* Non-blocking input poll.  Returns true when a full line is ready
       in inputBuf.  Call once per main-loop iteration. */
    static bool  poll();

    static char  inputBuf[CONSOLE_INPUT_LEN];

    /* Set to true by platform code whenever inputBuf changes.
       Cleared by UI::syncInput() after redrawing the input line. */
    static bool  inputDirty;

    /* Output: write a token without a trailing newline (for streaming) */
    static void  putToken(const char *tok);

    /* Output: advance a thinking spinner (|/-\) in place; no newline */
    static void  putSpinner();

    /* Output: write a line with a trailing newline */
    static void  putLine(const char *line);

    /* Output: show the user prompt */
    static void  putPrompt();

    /* Check for Ctrl-C/ESC/Break without consuming regular input.
       Sets ctrlBreak if detected.  Safe to call during streaming. */
    static void  checkBreak();

    /* Set to true by platform code when Ctrl-C / Ctrl-Break detected */
    static bool  ctrlBreak;
};

#endif
