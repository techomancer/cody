#ifndef CODY_UI_H
#define CODY_UI_H

/*
 * ui.h — Three-region full-screen layout built on tui.h
 *
 * Layout (screen height = H, width = W):
 *
 *   Rows 0 .. H-3  : output area  — scrolling text lines
 *   Row  H-2       : status bar   — inverse video, shows context stats
 *   Row  H-1       : input line   — "> " prefix + user text, scrolls left/right
 *
 * Input line scrolling: if the text is wider than the usable input area
 * (W - 2 columns, one each reserved for the ">" prompt and a possible "<"/">"
 * scroll indicator), the view slides so the cursor stays visible.  A "<"
 * marker is drawn in column 0 when content is hidden to the left, and ">"
 * is drawn in the last column when content is hidden to the right.
 *
 * Call sequence:
 *   UI::init()              — call once; replaces Console::init() for output
 *   UI::appendLine(text)    — add a line to the output area (auto-scroll)
 *   UI::putToken(tok)       — append streaming text to the current output line
 *   UI::putSpinner()        — animate spinner on current output line
 *   UI::setStatus(text)     — update the status bar text
 *   UI::syncInput()         — redraw the input line to match Console::inputBuf
 *   UI::shutdown()          — call once at exit
 *
 * Thread/re-entrancy: all functions must be called from the main loop only.
 */

#include "codytypes.h"
#include "tui.h"

#ifdef __WATCOMC__
#  define UI_BUF_SIZE  (8192)   /* circular text store, DOS     */
#  define UI_LINE_MAX  (128)    /* max chars in one output line */
#else
#  define UI_BUF_SIZE  (32768)  /* circular text store, Unix    */
#  define UI_LINE_MAX  (256)    /* max chars in one output line */
#endif
#define UI_STATUS_MAX  (128)    /* max chars in status bar      */

class UI {
public:
    static void init();
    static void shutdown();

    /* Append a complete line to the output area.  Triggers redraw. */
    static void appendLine(const char *text);

    /* Append a token fragment to the current (last) output line.
       Flushes to screen after each call. */
    static void putToken(const char *tok);

    /* Advance the spinner one frame on the current output line. */
    static void putSpinner();

    /* Replace the status bar text and redraw it. */
    static void setStatus(const char *text);

    /* Redraw the input line to mirror Console::inputBuf + cursor position.
       Call whenever inputBuf changes (i.e. every poll() iteration or after
       a line is consumed). */
    static void syncInput();

    /* Clear the current in-progress output line (called when spinner ends
       and real content follows). */
    static void clearCurrentLine();

    /* Append a line with an explicit tui_attr_t (used by the log facility). */
    static void appendLogLine(const char *text, tui_attr_t attr);

    /* Append a user prompt line in a distinct colour. */
    static void appendUserLine(const char *text);
};

#endif /* CODY_UI_H */
