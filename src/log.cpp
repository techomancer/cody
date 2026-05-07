#include "log.h"
#include "ui.h"
#include "tui.h"
#include <stdio.h>
#include <stdarg.h>

/* Dim dark-gray-on-black attribute for log lines */
#define ATTR_LOG  TUI_ATTR(TUI_BRIGHT_BLACK, TUI_BLACK, 0)

static int s_tui_ready = 0;

void logSetTuiReady(int ready) {
    s_tui_ready = ready;
}

void codylog(const char *fmt, ...) {
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (!s_tui_ready) {
        fprintf(stderr, "%s\n", buf);
        return;
    }

    char tagged[LOG_LINE_MAX + 8];
    snprintf(tagged, sizeof(tagged), "[DBG] %s", buf);
    UI::appendLogLine(tagged, ATTR_LOG);
}
