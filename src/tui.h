#ifndef CODY_TUI_H
#define CODY_TUI_H

/*
 * tui.h — Full-screen Text User Interface abstraction for Cody
 *
 * Targets: Linux (g++ -std=c++98), IRIX 6.5 (MIPSPro CC -n32),
 *          DOS (Open Watcom wpp -ml).
 *
 * Backends:
 *   UNIX/tui_unix.cpp  — terminfo (no curses) + shadow buffer + direct stdout
 *   DOS/tui_dos.cpp    — direct BIOS INT 10h + B800:0000 video memory
 *
 * All public functions use C linkage so the backends can be compiled by any
 * of the three compilers without name-mangling issues.
 *
 * Coordinate convention: 0-based (x, y), (0,0) = top-left corner.
 * The DOS backend adds 1 internally before BIOS calls (which are 1-based).
 *
 * Drawing on Unix goes into a shadow buffer; call tui_flush() to push
 * changes to the terminal.  On DOS all drawing is immediate (no flush step).
 */

#include "codytypes.h"  /* uint8_t, uint16_t, uint32_t — Watcom/stdint shim */

/* =========================================================================
 * Compile-time screen size limits
 *
 * Static allocation avoids any heap use after tui_init().
 * DOS: 80x50 = 4000 cells = 8 KB of shadow storage.
 * Unix: 256x256 = 65536 cells = 512 KB — fine for a static global.
 * Override via -DTUI_MAX_COLS=N in the platform Makefile if needed.
 * ========================================================================= */
#ifndef TUI_MAX_COLS
#  ifdef __WATCOMC__
#    define TUI_MAX_COLS  80
#  else
#    define TUI_MAX_COLS  256
#  endif
#endif

#ifndef TUI_MAX_ROWS
#  ifdef __WATCOMC__
#    define TUI_MAX_ROWS  50
#  else
#    define TUI_MAX_ROWS  256
#  endif
#endif

/* =========================================================================
 * Attribute word  —  uint32_t tui_attr_t
 *
 * Bit layout (identical on all platforms; backends ignore unsupported bits):
 *
 *   Bits  0– 7   Foreground color index  (0–255, see TUI_* color constants)
 *   Bits  8–15   Background color index
 *   Bit  16      Bold / bright intensity
 *   Bit  17      Italic  (Unix only; silently ignored on DOS)
 *   Bit  18      Underline
 *   Bit  19      Blink
 *   Bits 20–31   Reserved, must be zero
 *
 * DOS blink/bright-background note:
 *   CGA text-mode bit 7 of the attribute byte is either blink OR
 *   bright-background — they share the same hardware bit.  The DOS backend
 *   resolves the conflict by preferring bright-background: if bg >= 8 the
 *   bright-bg nibble is set and blink is silently dropped.
 * ========================================================================= */
typedef uint32_t tui_attr_t;

#define TUI_ATTR_FG_SHIFT   (0)
#define TUI_ATTR_BG_SHIFT   (8)
#define TUI_ATTR_FG_MASK    (0x000000FFu)
#define TUI_ATTR_BG_MASK    (0x0000FF00u)
#define TUI_ATTR_BOLD       (0x00010000u)
#define TUI_ATTR_ITALIC     (0x00020000u)  /* Unix only */
#define TUI_ATTR_UNDERLINE  (0x00040000u)
#define TUI_ATTR_BLINK      (0x00080000u)
#define TUI_ATTR_REVERSE    (0x00100000u)  /* reverse video (swap fg/bg) */

/* Compose an attribute word — usable as a compile-time constant. */
#define TUI_ATTR(fg, bg, flags) \
    ((tui_attr_t)( (uint32_t)(fg) \
                 | ((uint32_t)(bg) << TUI_ATTR_BG_SHIFT) \
                 | (uint32_t)(flags) ))

/* Extract fg/bg color index from a packed attr. */
#define TUI_ATTR_GET_FG(a)  ((uint8_t)((a) & TUI_ATTR_FG_MASK))
#define TUI_ATTR_GET_BG(a)  ((uint8_t)(((a) & TUI_ATTR_BG_MASK) >> TUI_ATTR_BG_SHIFT))

/* Default: white-on-black, no flags — matches DOS reset and plain xterm. */
#define TUI_ATTR_DEFAULT    TUI_ATTR(TUI_WHITE, TUI_BLACK, 0)

/* =========================================================================
 * Color constants
 *
 * Values 0–7 match the DOS CGA 4-bit foreground/background nibbles and the
 * ANSI/VT100 SGR color order (SGR 30–37 / 40–47).
 * Values 8–15 are the bright variants (SGR 90–97 / 100–107 or bold+0–7).
 *
 * Plain #defines rather than an enum: they are usable as array indices and
 * in integer arithmetic without casts in C++98 and in C translation units.
 * ========================================================================= */
#define TUI_BLACK           (0)
#define TUI_RED             (1)
#define TUI_GREEN           (2)
#define TUI_YELLOW          (3)
#define TUI_BLUE            (4)
#define TUI_MAGENTA         (5)
#define TUI_CYAN            (6)
#define TUI_WHITE           (7)
#define TUI_BRIGHT_BLACK    (8)   /* "dark gray" on DOS */
#define TUI_BRIGHT_RED      (9)
#define TUI_BRIGHT_GREEN    (10)
#define TUI_BRIGHT_YELLOW   (11)
#define TUI_BRIGHT_BLUE     (12)
#define TUI_BRIGHT_MAGENTA  (13)
#define TUI_BRIGHT_CYAN     (14)
#define TUI_BRIGHT_WHITE    (15)

/* =========================================================================
 * Box-drawing characters
 *
 * Plain ASCII for maximum portability.  A future tui_has_acs() query can
 * guard upgraded line-drawing if desired.
 * ========================================================================= */
#define TUI_BOX_HORIZ  ('-')
#define TUI_BOX_VERT   ('|')
#define TUI_BOX_TL     ('+')
#define TUI_BOX_TR     ('+')
#define TUI_BOX_BL     ('+')
#define TUI_BOX_BR     ('+')

/* =========================================================================
 * Return codes
 * ========================================================================= */
#define TUI_OK          ( 0)
#define TUI_ERR_INIT    (-1)   /* terminal / video init failed */
#define TUI_ERR_SIZE    (-2)   /* requested size not achievable */
#define TUI_ERR_BOUNDS  (-3)   /* coordinate out of range (debug builds) */

/* =========================================================================
 * Resize callback
 *
 * Fired from the SIGWINCH signal handler on Unix after the shadow buffer
 * dimensions have been updated.  Must be async-signal-safe.
 * Recommended pattern: set a flag and handle resize in the main loop via
 * tui_resize_pending().  On DOS: accepted but never called.
 * ========================================================================= */
typedef void (*tui_resize_fn)(uint8_t new_w, uint8_t new_h);

/* =========================================================================
 * Public API
 * ========================================================================= */
#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/*
 * tui_init — initialize the TUI layer.
 *
 * desired_w, desired_h: caller's preferred dimensions; pass 0 for either to
 *   mean "use whatever the terminal/video mode reports".
 * actual_w, actual_h: on return, the dimensions actually configured.
 *   Both output pointers must be non-NULL.
 *
 * Unix: calls setupterm(), queries TIOCGWINSZ, registers SIGWINCH.
 *   Clamps actual size to the real terminal window if desired > actual.
 *   Emits smcup (alternate screen buffer) if available.
 *
 * DOS: queries INT 10h AH=0Fh for current video mode and dimensions.
 *   If desired_h == 50 attempts 80x50 via EGA character generator call
 *   (INT 10h AH=11h AL=12h BL=00h); falls back to 80x25 if unsupported.
 *   Other sizes silently fall back to 80x25.
 *
 * Returns TUI_OK on success, TUI_ERR_INIT on failure.
 */
int8_t tui_init(uint8_t desired_w, uint8_t desired_h,
                uint8_t *actual_w,  uint8_t *actual_h);

/*
 * tui_shutdown — restore the terminal to its pre-init state.
 *
 * Unix: emits rmcup if smcup was used, restores original termios via
 *   tcsetattr(), shows cursor if it was hidden.
 * DOS: re-enables cursor if hidden (INT 10h AH=01h); leaves video mode as-is.
 */
void tui_shutdown(void);

/* -------------------------------------------------------------------------
 * Resize handling
 * ------------------------------------------------------------------------- */

/*
 * tui_set_resize_callback — register a function called on terminal resize.
 *
 * Pass NULL to remove an existing callback.
 * On DOS: no-op (accepted without error, never called).
 */
void tui_set_resize_callback(tui_resize_fn fn);

/*
 * tui_resize_pending — returns non-zero if SIGWINCH fired since the last
 * tui_flush() or call to this function.
 *
 * Lets the main loop poll for resize without relying on async-signal-safe
 * callback code.  Pattern mirrors Console::ctrlBreak.
 * On DOS: always returns 0.
 */
int tui_resize_pending(void);

/* -------------------------------------------------------------------------
 * Screen size query
 * ------------------------------------------------------------------------- */

/* Return current width/height (set by tui_init or the last SIGWINCH). */
uint8_t tui_width(void);
uint8_t tui_height(void);

/* -------------------------------------------------------------------------
 * Color capability queries
 * ------------------------------------------------------------------------- */

/* Returns non-zero if only monochrome attributes are available.
   Unix: tigetnum("colors") <= 1.  DOS: video mode 7 (MDA mono). */
int tui_is_mono(void);

/* Number of usable foreground / background color indices.
   Typical values: 1 (mono), 8 (basic xterm), 16 (xterm-16color / IRIX
   iris-ansi), 256 (xterm-256color).
   DOS text mode always returns 16 fg / 8 bg. */
uint16_t tui_fg_colors(void);
uint16_t tui_bg_colors(void);

/* -------------------------------------------------------------------------
 * Cursor
 * ------------------------------------------------------------------------- */

/*
 * tui_move — position the hardware cursor at (x, y), 0-based.
 *
 * Unix: records the desired cursor position; the physical cursor moves on
 *   the next tui_flush().
 * DOS: fires INT 10h AH=02h immediately (synchronous BIOS call).
 *
 * Coordinates outside [0,width-1]x[0,height-1] are clamped silently in
 * release builds.
 */
void tui_move(uint8_t x, uint8_t y);

/*
 * tui_cursor_visible — show (visible != 0) or hide (visible == 0) the cursor.
 *
 * Unix: emits "cnorm" or "civis" terminfo capabilities.
 * DOS: INT 10h AH=01h with scan-line values for normal / hidden cursor.
 */
void tui_cursor_visible(int visible);

/* -------------------------------------------------------------------------
 * Drawing — all coordinates 0-based, clipped at screen edges (no wrap)
 * ------------------------------------------------------------------------- */

/* Write a single character at (x, y) with the given attribute. */
void tui_putch(uint8_t x, uint8_t y, tui_attr_t attr, char c);

/* Write a NUL-terminated string starting at (x, y).
   Characters past the right edge are silently discarded. */
void tui_putstr(uint8_t x, uint8_t y, tui_attr_t attr, const char *text);

/* Draw ch repeated len times horizontally / vertically from (x, y). */
void tui_hline(uint8_t x, uint8_t y, tui_attr_t attr, char ch, uint8_t len);
void tui_vline(uint8_t x, uint8_t y, tui_attr_t attr, char ch, uint8_t len);

/* -------------------------------------------------------------------------
 * Screen-level operations
 * ------------------------------------------------------------------------- */

/* Fill the entire screen with spaces using attr. */
void tui_clear(tui_attr_t attr);

/* Fill a rectangular region with character ch and attr.
   Region is clipped to screen bounds. */
void tui_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   tui_attr_t attr, char ch);

/* -------------------------------------------------------------------------
 * Shadow buffer and flush
 *
 * On Unix all drawing functions write into the shadow buffer (s_back).
 * The physical terminal is updated only when tui_flush() is called.
 * tui_flush() diffs s_back against the on-screen state buffer (s_screen),
 * emits the minimal set of terminfo sequences for changed cells, then
 * calls fflush(stdout).
 *
 * On DOS all drawing is immediate (direct VRAM write); these functions
 * exist as no-ops so caller code compiles identically on all platforms.
 * ------------------------------------------------------------------------- */

/* Sync shadow buffer to terminal (diff-based, only changed cells). */
void tui_flush(void);

/* Force a full repaint of every cell regardless of dirty state.
   Use after a resize or when terminal state is unknown. */
void tui_flush_all(void);

/* Mark a single cell dirty so it is included in the next tui_flush().
   Normally unnecessary — tui_putch et al. track dirtiness automatically.
   On DOS: no-op. */
void tui_dirty_cell(uint8_t x, uint8_t y);

/* Invalidate rows [y0, y0+count) in the screen-state buffer so tui_flush
   repaints them unconditionally even if s_back hasn't changed.
   Use when the logical content of a region shifts (e.g. scroll).
   On DOS: no-op. */
void tui_invalidate_rows(uint8_t y0, uint8_t count);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* CODY_TUI_H */
