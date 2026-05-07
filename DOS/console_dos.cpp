#include "../src/console.h"
#include <stdio.h>
#include <string.h>
#include <conio.h>       /* cprintf, cputs */

/* mTCP BIOS key helpers */
#include "inlines.h"

/* -----------------------------------------------------------------------
   Static storage declared in console.h
   ----------------------------------------------------------------------- */

char Console::inputBuf[256];
bool Console::ctrlBreak  = false;
bool Console::inputDirty = false;

static uint8_t s_pos = 0;   /* current cursor position in inputBuf */

/* -----------------------------------------------------------------------
   Console::init / shutdown
   ----------------------------------------------------------------------- */

void Console::init() {
    s_pos = 0;
    inputBuf[0] = '\0';
    ctrlBreak   = false;
}

void Console::shutdown() { /* nothing to restore on DOS */ }

/* -----------------------------------------------------------------------
   Console::poll — non-blocking; returns true when a full line is ready
   ----------------------------------------------------------------------- */

bool Console::poll() {
    if (!biosIsKeyReady()) return false;

    uint16_t key = biosKeyRead();
    uint8_t ch   = (uint8_t)(key & 0xFF);
    uint8_t scan = (uint8_t)(key >> 8);

    /* Ctrl-C (ch=0x03), Ctrl-Break (scan=0x46), or ESC (ch=0x1B) */
    if (ch == 0x03 || ch == 0x1B || (ch == 0x00 && scan == 0x46)) {
        ctrlBreak = true;
        cprintf("\r\n");
        return false;
    }

    if (ch == '\r' || ch == '\n') {
        inputBuf[s_pos] = '\0';
        s_pos      = 0;
        inputDirty = true;
        return true;
    }

    if (ch == '\b') {
        if (s_pos > 0) {
            s_pos--;
            inputBuf[s_pos] = '\0';
            inputDirty = true;
        }
        return false;
    }

    /* Printable characters only */
    if (ch >= 0x20 && ch < 0x7F) {
        if (s_pos < (uint8_t)(sizeof(Console::inputBuf) - 1)) {
            inputBuf[s_pos++] = (char)ch;
            inputBuf[s_pos]   = '\0';
            inputDirty = true;
        }
    }

    return false;
}

/* -----------------------------------------------------------------------
   Console::putToken / putLine / putPrompt
   ----------------------------------------------------------------------- */

/* BIOS INT 16h AH=01 — peek at next keystroke without consuming it.
   Returns key in AX (same format as biosKeyRead), ZF set if no key.
   We only call this when biosIsKeyReady() is true, so ZF will be clear. */
static uint16_t biosPeekKey() {
    uint16_t key;
    __asm {
        mov ah, 01h
        int 16h
        mov key, ax
    }
    return key;
}

void Console::checkBreak() {
    if (!biosIsKeyReady()) return;
    uint16_t key = biosPeekKey();
    uint8_t ch   = (uint8_t)(key & 0xFF);
    uint8_t scan = (uint8_t)(key >> 8);
    if (ch == 0x03 || ch == 0x1B || (ch == 0x00 && scan == 0x46)) {
        biosKeyRead(); /* consume it */
        ctrlBreak = true;
        cprintf("\r\n");
    }
}

void Console::putToken(const char *tok) {
    for (; *tok; tok++) {
        if (*tok == '\n') {
            cprintf("\r\n");
        } else {
            char tmp[2] = { *tok, '\0' };
            cputs(tmp);
        }
    }
}

void Console::putSpinner() {
    static const char frames[] = "|/-\\";
    static uint8_t idx = 0;
    char tmp[3] = { frames[idx], '\r', '\0' };
    cputs(tmp);
    idx = (idx + 1) & 3;
}

void Console::putLine(const char *line) {
    cputs(line);
    cprintf("\r\n");
}

void Console::putPrompt() {
    cprintf("\r\n> ");
}
