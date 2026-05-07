#include "../src/tui.h"
#include <string.h>
#include <conio.h>
#include <i86.h>     /* MK_FP */

/*
 * DOS TUI backend — Open Watcom large-model (wpp -ml).
 *
 * All drawing is done via direct writes to CGA/EGA text-mode video RAM at
 * segment B800h (or B000h for MDA mono).  This is faster and more reliable
 * than INT 10h AH=09h per-character calls across all BIOS versions.
 *
 * Each VRAM cell is a 16-bit word:
 *   Low  byte: ASCII character code
 *   High byte: DOS attribute byte (see dos_attr() below)
 *
 * No shadow buffer — all drawing is immediately visible.
 * tui_flush(), tui_flush_all(), tui_dirty_cell() are no-ops.
 *
 * Coordinate convention: 0-based (x, y).  BIOS calls add 1 internally.
 */

/* =========================================================================
 * DOS attribute byte layout
 *
 *   Bit 7:    Blink  (or bright-background — same hardware bit)
 *   Bits 6–4: Background color (0–7; bit 7 handles bright/blink)
 *   Bit 3:    Foreground intensity (bold maps here)
 *   Bits 2–0: Foreground color (0–7)
 *
 * Bright-background vs. blink: if bg >= 8 the high-intensity background
 * bit is set and blink is silently dropped (they share bit 7).
 * ========================================================================= */

static uint8_t dos_attr(tui_attr_t attr) {
    uint8_t fg   = TUI_ATTR_GET_FG(attr);
    uint8_t bg   = TUI_ATTR_GET_BG(attr);
    int bold     = (attr & TUI_ATTR_BOLD)  ? 1 : 0;
    int blink    = (attr & TUI_ATTR_BLINK) ? 1 : 0;

    uint8_t fg_nibble = fg & 7;
    uint8_t fg_bright = (fg >= 8 || bold) ? 1 : 0;

    uint8_t bg_nibble;
    uint8_t hi_bit;

    if (bg >= 8) {
        /* Bright background: use bit 7 for intensity, blink not available */
        bg_nibble = bg & 7;
        hi_bit    = 1;
    } else {
        bg_nibble = bg & 7;
        hi_bit    = blink ? 1 : 0;
    }

    return (uint8_t)( (hi_bit    << 7)
                    | (bg_nibble << 4)
                    | (fg_bright << 3)
                    | fg_nibble );
}

/* =========================================================================
 * Static state
 * ========================================================================= */

static uint8_t         s_width       = 80;
static uint8_t         s_height      = 25;
static uint8_t         s_video_mode  = 0x03;
static int             s_is_mono     = 0;
static uint8_t         s_cursor_vis  = 1;
static int             s_initialized = 0;

/* Far pointer to video RAM — Open Watcom large model supports far pointers */
static uint16_t far   *s_vram        = NULL;

/* =========================================================================
 * VRAM helpers
 * ========================================================================= */

/* Write a character+attribute cell directly to VRAM. */
static void vram_put(uint8_t x, uint8_t y, tui_attr_t attr, char c) {
    uint16_t offset = (uint16_t)((uint16_t)y * s_width + x);
    uint8_t  da     = dos_attr(attr);
    s_vram[offset]  = (uint16_t)((uint16_t)(unsigned char)c | ((uint16_t)da << 8));
}

/* =========================================================================
 * Public API
 * ========================================================================= */

extern "C" {

int8_t tui_init(uint8_t desired_w, uint8_t desired_h,
                uint8_t *actual_w, uint8_t *actual_h) {

    /* Query current video mode via INT 10h AH=0Fh.
       Returns: AL=mode, AH=columns, BH=active page. */
    uint8_t mode, cols;
    __asm {
        mov ah, 0x0F
        int 0x10
        mov mode, al
        mov cols, ah
    }
    s_video_mode = mode;

    /* Determine mono vs color */
    if (mode == 0x07) {
        s_is_mono = 1;
        s_vram    = (uint16_t far *)MK_FP(0xB000, 0);
    } else {
        s_is_mono = 0;
        s_vram    = (uint16_t far *)MK_FP(0xB800, 0);
    }

    s_width = cols;

    /* Read actual row count from BIOS data area: byte at 0040:0084 = rows - 1.
       Present on EGA/VGA BIOSes; on plain CGA/MDA this byte may be 0. */
    uint8_t bios_rows = *((uint8_t far *)MK_FP(0x0040, 0x0084));
    s_height = (bios_rows > 0) ? (uint8_t)(bios_rows + 1) : 25;

    /* Optionally request 80x50 via EGA character generator.
       INT 10h AH=11h AL=12h BL=00h loads the 8x8 font halving row height,
       then we re-read the BIOS row count. */
    if (desired_h == 50 && !s_is_mono) {
        __asm {
            mov ax, 0x1112
            mov bx, 0x0000
            int 0x10
        }
        bios_rows = *((uint8_t far *)MK_FP(0x0040, 0x0084));
        s_height  = (bios_rows > 0) ? (uint8_t)(bios_rows + 1) : 25;
    }

    /* Clamp to requested size if smaller */
    if (desired_w != 0 && desired_w < s_width)  s_width  = desired_w;
    if (desired_h != 0 && desired_h < s_height) s_height = desired_h;

    *actual_w = s_width;
    *actual_h = s_height;

    s_cursor_vis  = 1;
    s_initialized = 1;
    return TUI_OK;
}

void tui_shutdown(void) {
    if (!s_initialized) return;
    /* Restore cursor visibility */
    if (!s_cursor_vis) tui_cursor_visible(1);
    s_initialized = 0;
}

/* DOS has no resize concept — these are no-ops with safe return values. */
void tui_set_resize_callback(tui_resize_fn /*fn*/) { }
int  tui_resize_pending(void) { return 0; }

uint8_t  tui_width(void)    { return s_width;  }
uint8_t  tui_height(void)   { return s_height; }
int      tui_is_mono(void)  { return s_is_mono; }
uint16_t tui_fg_colors(void) { return s_is_mono ? 1 : 16; }
uint16_t tui_bg_colors(void) { return s_is_mono ? 1 :  8; }

void tui_move(uint8_t x, uint8_t y) {
    if (x >= s_width)  x = (uint8_t)(s_width  - 1);
    if (y >= s_height) y = (uint8_t)(s_height - 1);
    /* INT 10h AH=02h: BH=page 0, DH=row (1-based), DL=col (1-based).
       We store 0-based externally; add 1 for BIOS. */
    __asm {
        mov ah, 0x02
        mov bh, 0x00
        mov dh, y
        mov dl, x
        int 0x10
    }
}

void tui_cursor_visible(int visible) {
    if (visible) {
        /* Normal cursor: scan lines 6–7 */
        __asm {
            mov ah, 0x01
            mov ch, 0x06
            mov cl, 0x07
            int 0x10
        }
    } else {
        /* Hidden cursor: set top scan line bit 5 (cursor off) */
        __asm {
            mov ah, 0x01
            mov ch, 0x20
            mov cl, 0x00
            int 0x10
        }
    }
    s_cursor_vis = (uint8_t)(visible ? 1 : 0);
}

void tui_putch(uint8_t x, uint8_t y, tui_attr_t attr, char c) {
    if (x >= s_width || y >= s_height) return;
    vram_put(x, y, attr, c);
}

void tui_putstr(uint8_t x, uint8_t y, tui_attr_t attr, const char *text) {
    for (; *text && x < s_width; text++, x++) {
        vram_put(x, y, attr, *text);
    }
}

void tui_hline(uint8_t x, uint8_t y, tui_attr_t attr, char ch, uint8_t len) {
    uint8_t i;
    for (i = 0; i < len && x + i < s_width; i++) {
        vram_put((uint8_t)(x + i), y, attr, ch);
    }
}

void tui_vline(uint8_t x, uint8_t y, tui_attr_t attr, char ch, uint8_t len) {
    uint8_t i;
    for (i = 0; i < len && y + i < s_height; i++) {
        vram_put(x, (uint8_t)(y + i), attr, ch);
    }
}

void tui_clear(tui_attr_t attr) {
    uint8_t r, c;
    for (r = 0; r < s_height; r++) {
        for (c = 0; c < s_width; c++) {
            vram_put(c, r, attr, ' ');
        }
    }
}

void tui_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   tui_attr_t attr, char ch) {
    uint8_t r, c;
    for (r = y; r < y + h && r < s_height; r++) {
        for (c = x; c < x + w && c < s_width; c++) {
            vram_put(c, r, attr, ch);
        }
    }
}

/* No shadow buffer on DOS — flush operations are no-ops. */
void tui_flush(void)                                        { }
void tui_flush_all(void)                                    { }
void tui_dirty_cell(uint8_t /*x*/, uint8_t /*y*/)          { }
void tui_invalidate_rows(uint8_t /*y0*/, uint8_t /*count*/) { }

}  /* extern "C" */
