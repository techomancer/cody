#include "../src/tui.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#ifdef __sgi
extern "C" {
#  include <curses.h>   /* IRIX: chtype, SGTTY etc. must come before term.h */
#  include <term.h>     /* tigetstr, tigetnum, setupterm, tputs, tparm */
}
#else
#include <term.h>       /* tigetstr, tigetnum, setupterm, tputs, tparm */
#endif

/* term.h may not define OK/ERR without curses.h — define them if missing */
#ifndef OK
#  define OK  (0)
#endif
#ifndef ERR
#  define ERR (-1)
#endif

/* =========================================================================
 * Internal types
 * ========================================================================= */

typedef struct {
    char       c;
    tui_attr_t attr;
} TuiCell;

/* =========================================================================
 * Static state
 * ========================================================================= */

/* Terminal configuration */
static struct termios  s_orig_termios;
static int             s_term_fd     = -1;
static uint8_t         s_width       = 0;
static uint8_t         s_height      = 0;
static uint16_t        s_fg_colors   = 8;
static uint16_t        s_bg_colors   = 8;
static int             s_is_mono     = 0;
static int             s_initialized = 0;

/* SIGWINCH */
static volatile int    s_resize_flag = 0;
static tui_resize_fn   s_resize_cb   = NULL;

/* Shadow / screen-sync buffers */
static TuiCell  s_back  [TUI_MAX_ROWS][TUI_MAX_COLS];
static TuiCell  s_screen[TUI_MAX_ROWS][TUI_MAX_COLS];
static uint8_t  s_dirty [TUI_MAX_ROWS];   /* non-zero = row has dirty cells */

/* Desired cursor position (written by tui_move, applied in tui_flush) */
static uint8_t  s_cur_x = 0;
static uint8_t  s_cur_y = 0;

/* Last attribute emitted to the physical terminal */
static tui_attr_t  s_screen_attr = (tui_attr_t)-1;   /* force reset on first flush */

/* Whether smcup was emitted (so we know to emit rmcup at shutdown) */
static int  s_used_alt_screen = 0;

/* Cached terminfo capability strings (loaded once at init) */
static const char *s_cap_cup    = NULL;   /* cursor_address        */
static const char *s_cap_sgr0   = NULL;   /* exit_attribute_mode   */
static const char *s_cap_civis  = NULL;   /* cursor_invisible      */
static const char *s_cap_cnorm  = NULL;   /* cursor_normal         */
static const char *s_cap_smcup  = NULL;   /* enter_ca_mode         */
static const char *s_cap_rmcup  = NULL;   /* exit_ca_mode          */
static const char *s_cap_el     = NULL;   /* clr_eol               */

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* putchar wrapper with the signature tputs() expects */
static int tui_putchar(int c) {
    return putchar(c);
}

/* Emit a terminfo string (if non-NULL) via tputs */
static void emit(const char *cap) {
    if (cap) tputs((char *)cap, 1, tui_putchar);
}

/*
 * Emit an SGR sequence for the given tui_attr_t.
 * We build the sequence manually rather than via tparm() to avoid
 * reentrance issues on older IRIX terminfo implementations.
 *
 * SGR resets first, then applies: bold, italic, underline, blink, fg, bg.
 * For 8-color terminals bright variants are rendered as bold + base color.
 * For 16-color terminals (COLORS >= 16) bright variants use SGR 90-97 / 100-107.
 */
static void emit_sgr(tui_attr_t attr) {
    char buf[64];
    int  pos = 0;

    uint8_t fg    = TUI_ATTR_GET_FG(attr);
    uint8_t bg    = TUI_ATTR_GET_BG(attr);
    int     bold  = (attr & TUI_ATTR_BOLD)      ? 1 : 0;
    int     ital  = (attr & TUI_ATTR_ITALIC)    ? 1 : 0;
    int     uline = (attr & TUI_ATTR_UNDERLINE) ? 1 : 0;
    int     blink = (attr & TUI_ATTR_BLINK)     ? 1 : 0;
    int     rev   = (attr & TUI_ATTR_REVERSE)   ? 1 : 0;

    buf[pos++] = '\033';
    buf[pos++] = '[';
    buf[pos++] = '0';   /* reset */

#ifdef __sgi
    if (bold) { buf[pos++] = ';'; buf[pos++] = '1'; }
#else
    if (bold  || fg >= 8) { buf[pos++] = ';'; buf[pos++] = '1'; }
#endif
    if (ital)              { buf[pos++] = ';'; buf[pos++] = '3'; }
    if (uline)             { buf[pos++] = ';'; buf[pos++] = '4'; }
    if (blink)             { buf[pos++] = ';'; buf[pos++] = '5'; }
    if (rev)               { buf[pos++] = ';'; buf[pos++] = '7'; }

    /* Foreground */
    if (s_fg_colors >= 16 && fg >= 8) {
        /* bright foreground: SGR 90-97 */
        uint8_t code = (uint8_t)(90 + (fg - 8));
        buf[pos++] = ';';
        buf[pos++] = (char)('0' + code / 10);
        buf[pos++] = (char)('0' + code % 10);
    } else {
        /* normal foreground: SGR 30-37 */
        buf[pos++] = ';';
        buf[pos++] = '3';
        buf[pos++] = (char)('0' + (fg & 7));
    }

    /* Background */
    if (s_bg_colors >= 16 && bg >= 8) {
        /* bright background: SGR 100-107 */
        uint8_t code = (uint8_t)(100 + (bg - 8));
        buf[pos++] = ';';
        buf[pos++] = '1';
        buf[pos++] = '0';
        buf[pos++] = (char)('0' + code - 100);
    } else {
        /* normal background: SGR 40-47 */
        buf[pos++] = ';';
        buf[pos++] = '4';
        buf[pos++] = (char)('0' + (bg & 7));
    }

    buf[pos++] = 'm';
    buf[pos]   = '\0';
    fputs(buf, stdout);
}

/* Emit terminfo cup (cursor_address) for 0-based (x, y).
   tparm takes row first, then column. */
static void emit_cup(uint8_t x, uint8_t y) {
    if (s_cap_cup) {
        const char *seq = (const char *)tparm((char *)s_cap_cup, (long)y, (long)x);
        if (seq) tputs((char *)seq, 1, tui_putchar);
    }
}

/* Query terminal window size via ioctl; update s_width / s_height. */
static void query_winsize(void) {
    struct winsize ws;
    if (ioctl(s_term_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        uint16_t w = ws.ws_col;
        uint16_t h = ws.ws_row;
        if (w > TUI_MAX_COLS) w = TUI_MAX_COLS;
        if (h > TUI_MAX_ROWS) h = TUI_MAX_ROWS;
        s_width  = (uint8_t)w;
        s_height = (uint8_t)h;
    }
}

/* SIGWINCH handler: record new size and set the pending flag. */
static void sigwinch_handler(int /*sig*/) {
    query_winsize();
    s_resize_flag = 1;
    if (s_resize_cb) s_resize_cb(s_width, s_height);
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

extern "C" {

int8_t tui_init(uint8_t desired_w, uint8_t desired_h,
                uint8_t *actual_w, uint8_t *actual_h) {
    int err = 0;

    s_term_fd = fileno(stdout);

    if (setupterm(NULL, s_term_fd, &err) != OK) {
        return TUI_ERR_INIT;
    }

    /* Cache capability strings */
    s_cap_cup   = tigetstr((char *)"cup");
    s_cap_sgr0  = tigetstr((char *)"sgr0");
    s_cap_civis = tigetstr((char *)"civis");
    s_cap_cnorm = tigetstr((char *)"cnorm");
    s_cap_smcup = tigetstr((char *)"smcup");
    s_cap_rmcup = tigetstr((char *)"rmcup");
    s_cap_el    = tigetstr((char *)"el");

    /* Treat (char *)-1 (capability not present) as NULL */
    if (s_cap_cup   == (char *)-1) s_cap_cup   = NULL;
    if (s_cap_sgr0  == (char *)-1) s_cap_sgr0  = NULL;
    if (s_cap_civis == (char *)-1) s_cap_civis = NULL;
    if (s_cap_cnorm == (char *)-1) s_cap_cnorm = NULL;
    if (s_cap_smcup == (char *)-1) s_cap_smcup = NULL;
    if (s_cap_rmcup == (char *)-1) s_cap_rmcup = NULL;
    if (s_cap_el    == (char *)-1) s_cap_el    = NULL;

    /* Determine color support */
    {
        int colors = tigetnum((char *)"colors");
        if (colors <= 1) {
            s_is_mono   = 1;
            s_fg_colors = 1;
            s_bg_colors = 1;
        } else {
            s_is_mono   = 0;
            s_fg_colors = (uint16_t)(colors > 65535 ? 65535 : colors);
            /* Cap bg at fg — most terminals allow same count for both */
            s_bg_colors = s_fg_colors;
        }
    }

    /* Query actual terminal size */
    query_winsize();
    if (s_width == 0)  s_width  = 80;   /* fallback if ioctl fails */
    if (s_height == 0) s_height = 24;

    /* Clamp to desired if caller specified a smaller size */
    if (desired_w != 0 && desired_w < s_width)  s_width  = desired_w;
    if (desired_h != 0 && desired_h < s_height) s_height = desired_h;

    *actual_w = s_width;
    *actual_h = s_height;

    /* Save terminal state and register SIGWINCH */
    tcgetattr(s_term_fd, &s_orig_termios);
    signal(SIGWINCH, sigwinch_handler);

    /* Enter alternate screen buffer if supported */
    if (s_cap_smcup) {
        emit(s_cap_smcup);
        s_used_alt_screen = 1;
    }

    /* Clear shadow buffers */
    memset(s_back,   0, sizeof(s_back));
    memset(s_screen, 0, sizeof(s_screen));
    memset(s_dirty,  0, sizeof(s_dirty));

    /* Initialize both buffers to space / default attr so first flush
       emits a clean full-screen clear */
    {
        int r, c;
        for (r = 0; r < TUI_MAX_ROWS; r++) {
            for (c = 0; c < TUI_MAX_COLS; c++) {
                s_back  [r][c].c    = ' ';
                s_back  [r][c].attr = TUI_ATTR_DEFAULT;
                s_screen[r][c].c    = '\0';  /* forces mismatch on first flush */
                s_screen[r][c].attr = (tui_attr_t)-1;
            }
            s_dirty[r] = 1;  /* mark all rows dirty for initial paint */
        }
    }

    s_screen_attr  = (tui_attr_t)-1;
    s_cur_x        = 0;
    s_cur_y        = 0;
    s_resize_flag  = 0;
    s_initialized  = 1;

    return TUI_OK;
}

void tui_shutdown(void) {
    if (!s_initialized) return;

    emit(s_cap_sgr0);   /* reset attributes */
    emit(s_cap_cnorm);  /* show cursor */
    if (s_used_alt_screen) emit(s_cap_rmcup);
    fflush(stdout);

    tcsetattr(s_term_fd, TCSANOW, &s_orig_termios);
    signal(SIGWINCH, SIG_DFL);

    s_initialized    = 0;
    s_used_alt_screen = 0;
    s_resize_flag    = 0;
    s_resize_cb      = NULL;
}

void tui_set_resize_callback(tui_resize_fn fn) {
    s_resize_cb = fn;
}

int tui_resize_pending(void) {
    int f = s_resize_flag;
    s_resize_flag = 0;
    return f;
}

uint8_t tui_width(void)  { return s_width;  }
uint8_t tui_height(void) { return s_height; }

int      tui_is_mono(void)    { return s_is_mono;   }
uint16_t tui_fg_colors(void)  { return s_fg_colors; }
uint16_t tui_bg_colors(void)  { return s_bg_colors; }

void tui_move(uint8_t x, uint8_t y) {
    if (x >= s_width)  x = (uint8_t)(s_width  - 1);
    if (y >= s_height) y = (uint8_t)(s_height - 1);
    s_cur_x = x;
    s_cur_y = y;
}

void tui_cursor_visible(int visible) {
    if (visible) emit(s_cap_cnorm);
    else         emit(s_cap_civis);
    fflush(stdout);
}

void tui_putch(uint8_t x, uint8_t y, tui_attr_t attr, char c) {
    if (x >= s_width || y >= s_height) return;
    if (s_back[y][x].c != c || s_back[y][x].attr != attr) {
        s_back[y][x].c    = c;
        s_back[y][x].attr = attr;
        s_dirty[y]        = 1;
    }
}

void tui_putstr(uint8_t x, uint8_t y, tui_attr_t attr, const char *text) {
    for (; *text && x < s_width; text++, x++) {
        tui_putch(x, y, attr, *text);
    }
}

void tui_hline(uint8_t x, uint8_t y, tui_attr_t attr, char ch, uint8_t len) {
    uint8_t i;
    for (i = 0; i < len && x + i < s_width; i++) {
        tui_putch((uint8_t)(x + i), y, attr, ch);
    }
}

void tui_vline(uint8_t x, uint8_t y, tui_attr_t attr, char ch, uint8_t len) {
    uint8_t i;
    for (i = 0; i < len && y + i < s_height; i++) {
        tui_putch(x, (uint8_t)(y + i), attr, ch);
    }
}

void tui_clear(tui_attr_t attr) {
    uint8_t r, c;
    for (r = 0; r < s_height; r++) {
        for (c = 0; c < s_width; c++) {
            if (s_back[r][c].c != ' ' || s_back[r][c].attr != attr) {
                s_back[r][c].c    = ' ';
                s_back[r][c].attr = attr;
                s_dirty[r]        = 1;
            }
        }
    }
}

void tui_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   tui_attr_t attr, char ch) {
    uint8_t r, c;
    for (r = y; r < y + h && r < s_height; r++) {
        for (c = x; c < x + w && c < s_width; c++) {
            if (s_back[r][c].c != ch || s_back[r][c].attr != attr) {
                s_back[r][c].c    = ch;
                s_back[r][c].attr = attr;
                s_dirty[r]        = 1;
            }
        }
    }
}

void tui_flush(void) {
    uint8_t r, c;
    int     cursor_known = 0;   /* do we know where the physical cursor is? */
    uint8_t phys_x = 255, phys_y = 255;

    for (r = 0; r < s_height; r++) {
        if (!s_dirty[r]) continue;
        s_dirty[r] = 0;

        for (c = 0; c < s_width; c++) {
            TuiCell *bk = &s_back  [r][c];
            TuiCell *sc = &s_screen[r][c];

            if (bk->c == sc->c && bk->attr == sc->attr) continue;

            /* Move cursor if needed */
            if (!cursor_known || phys_x != c || phys_y != r) {
                emit_cup(c, r);
                phys_x = c;
                phys_y = r;
                cursor_known = 1;
            }

            /* Emit attribute change if needed */
            if (bk->attr != s_screen_attr) {
                emit_sgr(bk->attr);
                s_screen_attr = bk->attr;
            }

            putchar(bk->c);
            phys_x++;

            sc->c    = bk->c;
            sc->attr = bk->attr;
        }
    }

    /* Reposition physical cursor to the desired location */
    if (!cursor_known || phys_x != s_cur_x || phys_y != s_cur_y) {
        emit_cup(s_cur_x, s_cur_y);
    }

    fflush(stdout);
}

void tui_flush_all(void) {
    int r, c;

    /* Mark everything as on-screen-unknown so tui_flush repaints all */
    for (r = 0; r < TUI_MAX_ROWS; r++) {
        s_dirty[r] = 1;
        for (c = 0; c < TUI_MAX_COLS; c++) {
            s_screen[r][c].c    = '\0';
            s_screen[r][c].attr = (tui_attr_t)-1;
        }
    }
    s_screen_attr = (tui_attr_t)-1;

    /* Re-query size in case the terminal changed under us */
    query_winsize();

    tui_flush();
}

void tui_dirty_cell(uint8_t x, uint8_t y) {
    if (x < s_width && y < s_height) {
        s_screen[y][x].c    = '\0';
        s_screen[y][x].attr = (tui_attr_t)-1;
        s_dirty[y]          = 1;
    }
}

void tui_invalidate_rows(uint8_t y0, uint8_t count) {
    int r;
    for (r = y0; r < y0 + count && r < s_height; r++) {
        int c;
        for (c = 0; c < s_width; c++) {
            s_screen[r][c].c    = '\0';
            s_screen[r][c].attr = (tui_attr_t)-1;
        }
        s_dirty[r] = 1;
    }
}

}  /* extern "C" */
