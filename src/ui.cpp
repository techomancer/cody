#include "ui.h"
#include "tui.h"
#include "console.h"
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Attributes
 * ========================================================================= */

#define ATTR_OUTPUT   TUI_ATTR(TUI_WHITE,        TUI_BLACK,  0)
#ifdef __sgi
#define ATTR_STATUS   TUI_ATTR(TUI_BLACK,        TUI_WHITE,  0)
#define ATTR_STATUS_FILL '-'
#else
#define ATTR_STATUS   TUI_ATTR(TUI_BLACK,        TUI_WHITE,  TUI_ATTR_BOLD)
#define ATTR_STATUS_FILL ' '
#endif
#define ATTR_PROMPT   TUI_ATTR(TUI_BRIGHT_GREEN, TUI_BLACK,  TUI_ATTR_BOLD)
#define ATTR_INPUT    TUI_ATTR(TUI_BRIGHT_WHITE, TUI_BLACK,  0)
#define ATTR_SCROLL   TUI_ATTR(TUI_BRIGHT_CYAN,  TUI_BLACK,  TUI_ATTR_BOLD)
#define ATTR_USER     TUI_ATTR(TUI_BRIGHT_CYAN,  TUI_BLACK,  TUI_ATTR_BOLD)

/* =========================================================================
 * Circular output buffer
 *
 * Flat byte ring.  Records are stored with byte-granular circular wrap —
 * buf_read/buf_write handle the modular indexing, so a record is never
 * physically split (logically it is always contiguous in the ring).
 *
 * Record layout (bytes):
 *   [uint16_t len][tui_attr_t attr (4 B)][char * len][uint16_t len]
 *
 * The trailing len copy lets us walk backward: read 2 bytes before the
 * current record's header to get the previous record's body length, then
 * step back (len + REC_OVER) bytes to reach that header.
 *
 * s_head : position of the next byte to write
 * s_used : bytes currently occupied (0 .. UI_BUF_SIZE)
 * tail   : (s_head - s_used + UI_BUF_SIZE) % UI_BUF_SIZE  (oldest byte)
 * ========================================================================= */

#define REC_HDR  (6u)   /* uint16_t len + tui_attr_t (4) */
#define REC_TRL  (2u)   /* trailing uint16_t len */
#define REC_OVER (8u)   /* REC_HDR + REC_TRL */

static uint8_t  s_buf[UI_BUF_SIZE];
static uint16_t s_head = 0;
static uint16_t s_used = 0;

/* Position of the current in-progress record's header; 0xFFFF = none */
static uint16_t s_cur  = 0xFFFFu;

/* =========================================================================
 * Screen geometry
 * ========================================================================= */

static uint8_t s_w       = 0;
static uint8_t s_h       = 0;
static uint8_t s_outRows = 0;

/* =========================================================================
 * Status / input / spinner state
 * ========================================================================= */

static char     s_status[UI_STATUS_MAX];
static uint16_t s_inputOff  = 0;
static uint8_t  s_spinnerIdx = 0;
static uint8_t  s_spinnerOn  = 0;
static uint16_t s_spinnerCol = 0;

/* =========================================================================
 * Buffer primitives
 * ========================================================================= */

static void buf_write(uint16_t pos, const void *src, uint16_t n) {
    const uint8_t *p = (const uint8_t *)src;
    uint16_t i;
    for (i = 0; i < n; i++)
        s_buf[(uint16_t)((pos + i) % UI_BUF_SIZE)] = p[i];
}

static void buf_read(uint16_t pos, void *dst, uint16_t n) {
    uint8_t *p = (uint8_t *)dst;
    uint16_t i;
    for (i = 0; i < n; i++)
        p[i] = s_buf[(uint16_t)((pos + i) % UI_BUF_SIZE)];
}

static uint16_t buf_u16(uint16_t pos) {
    uint16_t v; buf_read(pos, &v, 2); return v;
}
static void buf_put_u16(uint16_t pos, uint16_t v) {
    buf_write(pos, &v, 2);
}
static tui_attr_t buf_attr(uint16_t pos) {
    tui_attr_t a; buf_read(pos, &a, 4); return a;
}
static void buf_put_attr(uint16_t pos, tui_attr_t a) {
    buf_write(pos, &a, 4);
}

/* =========================================================================
 * Record management
 * ========================================================================= */

/* Evict oldest records until `need` free bytes exist.
   Never evicts s_cur (the in-progress record). */
static void evict(uint16_t need) {
    while ((uint16_t)(s_used + need) > (uint16_t)UI_BUF_SIZE) {
        uint16_t tail = (uint16_t)((s_head + UI_BUF_SIZE - s_used) % UI_BUF_SIZE);
        if (tail == s_cur) break;  /* can't evict current record */
        uint16_t len  = buf_u16(tail);
        uint16_t rec  = (uint16_t)(REC_OVER + len);
        s_used = (uint16_t)(s_used - rec);
    }
}

/* Allocate n bytes at s_head and return starting position */
static uint16_t buf_alloc(uint16_t n) {
    evict(n);
    uint16_t pos = s_head;
    s_head = (uint16_t)((s_head + n) % UI_BUF_SIZE);
    s_used = (uint16_t)(s_used + n);
    return pos;
}

/* Start a new empty record; sets s_cur */
static void rec_start(tui_attr_t attr) {
    uint16_t pos = buf_alloc((uint16_t)REC_OVER);
    buf_put_u16(pos, 0);
    buf_put_attr((uint16_t)(pos + 2), attr);
    buf_put_u16((uint16_t)(pos + 6), 0);
    s_cur = pos;
}

/* Append len bytes to current record body (extends allocation) */
static void rec_append(const char *text, uint16_t len) {
    if (s_cur == 0xFFFFu || len == 0) return;
    uint16_t old_len  = buf_u16(s_cur);
    /* Allocate space for new bytes (old trailer will be overwritten) */
    evict(len);
    uint16_t body_end = (uint16_t)((s_cur + REC_HDR + old_len) % UI_BUF_SIZE);
    buf_write(body_end, text, len);
    s_head = (uint16_t)((s_head + len) % UI_BUF_SIZE);
    s_used = (uint16_t)(s_used + len);
    uint16_t new_len  = (uint16_t)(old_len + len);
    buf_put_u16(s_cur, new_len);
    buf_put_u16((uint16_t)((s_cur + REC_HDR + new_len) % UI_BUF_SIZE), new_len);
}

static void rec_set_attr(tui_attr_t attr) {
    if (s_cur != 0xFFFFu)
        buf_put_attr((uint16_t)(s_cur + 2), attr);
}

/* Shrink current record body to new_len */
static void rec_truncate(uint16_t new_len) {
    if (s_cur == 0xFFFFu) return;
    uint16_t old_len = buf_u16(s_cur);
    if (new_len >= old_len) return;
    uint16_t delta = (uint16_t)(old_len - new_len);
    s_head = (uint16_t)((s_head + UI_BUF_SIZE - delta) % UI_BUF_SIZE);
    s_used = (uint16_t)(s_used - delta);
    buf_put_u16(s_cur, new_len);
    buf_put_u16((uint16_t)((s_cur + REC_HDR + new_len) % UI_BUF_SIZE), new_len);
}

static uint16_t rec_len() {
    return (s_cur == 0xFFFFu) ? 0 : buf_u16(s_cur);
}

/* Close current line and start a fresh empty one */
static void finish_line() {
    s_cur = 0xFFFFu;
    rec_start(ATTR_OUTPUT);
}

/* =========================================================================
 * Backward iterator
 * ========================================================================= */

typedef struct { uint16_t pos; uint16_t remaining; } RecIter;

static bool iter_begin(RecIter *it) {
    if (s_used < REC_OVER) return false;
    /* newest record: trailer ends at s_head, so read trailer just before */
    uint16_t trl = (uint16_t)((s_head + UI_BUF_SIZE - REC_TRL) % UI_BUF_SIZE);
    uint16_t len = buf_u16(trl);
    if ((uint16_t)(REC_OVER + len) > s_used) return false;
    it->pos       = (uint16_t)((s_head + UI_BUF_SIZE - REC_OVER - len) % UI_BUF_SIZE);
    it->remaining = s_used;
    return true;
}

static bool iter_prev(RecIter *it) {
    uint16_t len      = buf_u16(it->pos);
    uint16_t rec_size = (uint16_t)(REC_OVER + len);
    if (rec_size > it->remaining) return false;
    it->remaining = (uint16_t)(it->remaining - rec_size);
    if (it->remaining < REC_OVER) return false;
    /* trailer of previous record is immediately before this record's header */
    uint16_t trl      = (uint16_t)((it->pos + UI_BUF_SIZE - REC_TRL) % UI_BUF_SIZE);
    uint16_t prev_len = buf_u16(trl);
    uint16_t prev_rec = (uint16_t)(REC_OVER + prev_len);
    if (prev_rec > it->remaining) return false;
    it->pos = (uint16_t)((it->pos + UI_BUF_SIZE - prev_rec) % UI_BUF_SIZE);
    return true;
}

/* =========================================================================
 * Screen drawing
 * ========================================================================= */

/* Count screen rows a record occupies (wrapping at s_w) */
static uint8_t rec_rows(uint16_t rec_pos) {
    uint16_t len = buf_u16(rec_pos);
    if (len == 0 || s_w == 0) return 1;
    uint8_t  rows = 1;
    uint8_t  col  = 0;
    uint16_t i;
    for (i = 0; i < len; i++) {
        char c = (char)s_buf[(uint16_t)((rec_pos + REC_HDR + i) % UI_BUF_SIZE)];
        if (c == '\n') { rows++; col = 0; }
        else { col++; if (col >= s_w) { rows++; col = 0; } }
    }
    return rows;
}

/* Draw one record onto screen starting at row `row`, clipped to max_rows.
   Returns number of rows drawn. */
static uint8_t rec_draw(uint16_t rec_pos, uint8_t row, uint8_t max_rows) {
    uint16_t   len   = buf_u16(rec_pos);
    tui_attr_t attr  = buf_attr((uint16_t)(rec_pos + 2));
    uint8_t    total = rec_rows(rec_pos);
    uint8_t    skip  = (total > max_rows) ? (uint8_t)(total - max_rows) : 0;
    uint8_t    cur_r = 0, drawn = 0, col = 0;

    static char rb[257];
    uint16_t rlen = 0;

    uint16_t i;
    for (i = 0; i <= len; i++) {
        char c = (i < len)
            ? (char)s_buf[(uint16_t)((rec_pos + REC_HDR + i) % UI_BUF_SIZE)]
            : '\n';  /* sentinel flush at end */

        if (c == '\n' || col >= s_w) {
            if (c != '\n') {
                /* width wrap: put c into next row */
                if (cur_r >= skip && drawn < max_rows) {
                    rb[rlen] = '\0';
                    tui_hline(0, (uint8_t)(row + drawn), ATTR_OUTPUT, ' ', s_w);
                    tui_putstr(0, (uint8_t)(row + drawn), attr, rb);
                    drawn++;
                }
                cur_r++; col = 0; rlen = 0;
                /* now handle c in new row */
                if (rlen < 256) rb[rlen++] = c;
                col++;
            } else {
                if (cur_r >= skip && drawn < max_rows) {
                    rb[rlen] = '\0';
                    tui_hline(0, (uint8_t)(row + drawn), ATTR_OUTPUT, ' ', s_w);
                    tui_putstr(0, (uint8_t)(row + drawn), attr, rb);
                    drawn++;
                }
                cur_r++; col = 0; rlen = 0;
            }
        } else {
            if (rlen < 256) rb[rlen++] = c;
            col++;
        }
    }
    return drawn;
}

static void redraw_output_all() {
    static uint16_t stk[255];  /* s_outRows is uint8_t, max 253 */
    uint8_t  stk_top  = 0;
    uint16_t rows_acc = 0;

    RecIter it;
    bool    ok = iter_begin(&it);
    while (ok && stk_top < s_outRows) {
        uint8_t r = rec_rows(it.pos);
        stk[stk_top] = it.pos;
        stk_top++;
        rows_acc = (uint16_t)(rows_acc + r);
        if (rows_acc >= s_outRows) break;
        ok = iter_prev(&it);
    }

    /* Force repaint of all output rows on Unix (no-op on DOS) */
    tui_invalidate_rows(0, s_outRows);

    /* Compute start row so newest content sits at the bottom */
    uint8_t screen_row = (rows_acc < s_outRows)
                         ? (uint8_t)(s_outRows - rows_acc)
                         : 0;

    /* Blank rows above the first record (empty space at top) */
    uint8_t r;
    for (r = 0; r < screen_row; r++)
        tui_hline(0, r, ATTR_OUTPUT, ' ', s_w);

    /* Draw oldest-first; rec_draw clears each row before writing */
    int i;
    for (i = (int)stk_top - 1; i >= 0; i--) {
        uint8_t avail = (uint8_t)(s_outRows - screen_row);
        if (avail == 0) break;
        uint8_t used = rec_draw(stk[(uint8_t)i], screen_row, avail);
        if (used == 0) used = 1;
        screen_row = (uint8_t)(screen_row + used);
    }

    /* Blank any rows below the last record (shouldn't happen, but be safe) */
    for (r = screen_row; r < s_outRows; r++)
        tui_hline(0, r, ATTR_OUTPUT, ' ', s_w);
}

static void redraw_status() {
    tui_hline(0, s_outRows, ATTR_STATUS, ATTR_STATUS_FILL, s_w);
    tui_putstr(1, s_outRows, ATTR_STATUS, s_status);
}

static void redraw_input() {
    uint8_t  iy     = (uint8_t)(s_h - 1);
    uint16_t len    = (uint16_t)strlen(Console::inputBuf);
    uint16_t cursor = len;
    uint8_t  text_w = (uint8_t)(s_w > 2 ? s_w - 2 : 0);

    if (cursor < s_inputOff) s_inputOff = cursor;
    if (s_w > 1 && cursor > (uint16_t)(s_inputOff + text_w))
        s_inputOff = (uint16_t)(cursor - text_w);

    bool has_left  = (s_inputOff > 0);
    bool has_right = (len > (uint16_t)(s_inputOff + text_w));
    uint8_t text_start = has_left ? 2 : 1;
    uint8_t text_cols  = (uint8_t)(s_w - text_start - (has_right ? 1 : 0));

    tui_hline(0, iy, ATTR_INPUT, ' ', s_w);
    tui_putch(0, iy, ATTR_PROMPT, '>');
    if (has_left) tui_putch(1, iy, ATTR_SCROLL, '<');

    const char *buf = Console::inputBuf + s_inputOff;
    uint8_t col = text_start, vis = 0;
    while (*buf && vis < text_cols) {
        tui_putch(col, iy, ATTR_INPUT, *buf++);
        col++; vis++;
    }
    if (has_right) tui_putch((uint8_t)(s_w - 1), iy, ATTR_SCROLL, '>');

    uint8_t cur_col = (uint8_t)(text_start + (cursor - s_inputOff));
    if (cur_col >= s_w) cur_col = (uint8_t)(s_w - 1);
    tui_move(cur_col, iy);
}

static void redraw_all() {
    redraw_output_all();
    redraw_status();
    redraw_input();
    tui_flush();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void UI::init() {
    uint8_t actual_w, actual_h;
    tui_init(0, 0, &actual_w, &actual_h);
    s_w       = actual_w;
    s_h       = actual_h;
    s_outRows = (uint8_t)(s_h > 2 ? s_h - 2 : 1);
    s_head = 0; s_used = 0; s_cur = 0xFFFFu;
    s_inputOff = 0; s_spinnerOn = 0; s_spinnerIdx = 0;
    s_status[0] = '\0';
    rec_start(ATTR_OUTPUT);
    tui_cursor_visible(1);
    redraw_all();
}

void UI::shutdown() { tui_shutdown(); }

static void append_line_attr(const char *text, tui_attr_t attr) {
    if (s_spinnerOn) { rec_truncate(s_spinnerCol); s_spinnerOn = 0; }
    if (rec_len() > 0) finish_line();
    rec_set_attr(attr);
    uint16_t len = (uint16_t)strlen(text);
    if (len > (uint16_t)(UI_LINE_MAX - 1)) len = (uint16_t)(UI_LINE_MAX - 1);
    rec_append(text, len);
    finish_line();
    redraw_output_all();
    redraw_input();
    tui_flush();
}

void UI::appendLine(const char *text)                       { append_line_attr(text, ATTR_OUTPUT); }
void UI::appendLogLine(const char *text, tui_attr_t attr)   { append_line_attr(text, attr); }
void UI::appendUserLine(const char *text)                   { append_line_attr(text, ATTR_USER); }

void UI::putToken(const char *tok) {
    if (s_spinnerOn) { rec_truncate(s_spinnerCol); s_spinnerOn = 0; }
    rec_set_attr(ATTR_OUTPUT);

    const char *seg = tok;
    const char *p   = tok;
    while (*p) {
        if (*p == '\n') {
            uint16_t seglen = (uint16_t)(p - seg);
            if (seglen > 0) {
                uint16_t avail = (uint16_t)(UI_LINE_MAX - 1 - rec_len());
                if (seglen > avail) seglen = avail;
                rec_append(seg, seglen);
            }
            finish_line();
            p++; seg = p;
        } else { p++; }
    }
    if (p > seg) {
        uint16_t seglen = (uint16_t)(p - seg);
        uint16_t avail  = (uint16_t)(UI_LINE_MAX - 1 - rec_len());
        if (seglen > avail) seglen = avail;
        rec_append(seg, seglen);
    }
    redraw_output_all();
    redraw_input();
    tui_flush();
}

/* Repaint only the bottom output row (where the current record lives).
   Much cheaper than redraw_output_all — used by the spinner. */
static void redraw_bottom_row() {
    uint8_t row = (uint8_t)(s_outRows - 1);
    tui_hline(0, row, ATTR_OUTPUT, ' ', s_w);
    if (s_cur != 0xFFFFu) {
        uint16_t   len  = rec_len();
        tui_attr_t attr = buf_attr((uint16_t)(s_cur + 2));
        static char tmp[257];
        uint16_t copy = (len < 256) ? len : 256;
        uint16_t i;
        for (i = 0; i < copy; i++)
            tmp[i] = (char)s_buf[(uint16_t)((s_cur + REC_HDR + i) % UI_BUF_SIZE)];
        tmp[copy] = '\0';
        tui_putstr(0, row, attr, tmp);
    }
}

void UI::putSpinner() {
    static const char frames[] = "|/-\\";
    if (!s_spinnerOn) {
        /* Start spinner on a fresh empty line */
        if (rec_len() > 0) finish_line();
        s_spinnerCol = 0;
        s_spinnerOn  = 1;
    }
    rec_truncate(s_spinnerCol);
    char fc = frames[s_spinnerIdx & 3];
    rec_append(&fc, 1);
    s_spinnerIdx = (uint8_t)((s_spinnerIdx + 1) & 3);
    redraw_bottom_row();
    redraw_input();
    tui_flush();
}

void UI::clearCurrentLine() {
    rec_truncate(0);
    s_spinnerOn = 0;
}

void UI::setStatus(const char *text) {
    strncpy(s_status, text, UI_STATUS_MAX - 1);
    s_status[UI_STATUS_MAX - 1] = '\0';
    redraw_status();
    redraw_input();
    tui_flush();
}

void UI::syncInput() {
    if (!Console::inputDirty) return;
    Console::inputDirty = false;
    uint16_t len = (uint16_t)strlen(Console::inputBuf);
    if (s_inputOff > len) s_inputOff = len;
    redraw_input();
    tui_flush();
}
