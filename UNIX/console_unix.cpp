#include "console.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <signal.h>
#include <termios.h>

char Console::inputBuf[CONSOLE_INPUT_LEN];
bool Console::ctrlBreak   = false;
bool Console::inputDirty  = false;

static uint16_t      s_editLen = 0;
static struct termios s_orig;

static void sigintHandler(int /*sig*/) {
    Console::ctrlBreak = true;
}

void Console::init() {
    s_editLen    = 0;
    inputBuf[0]  = '\0';
    ctrlBreak    = false;
    signal(SIGINT, sigintHandler);

    /* Switch to cbreak: char-at-a-time, no echo, signals still work */
    tcgetattr(0, &s_orig);
    struct termios raw = s_orig;
    raw.c_lflag &= ~(unsigned long)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;   /* non-blocking read */
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &raw);
}

void Console::shutdown() {
    tcsetattr(0, TCSANOW, &s_orig);
    signal(SIGINT, SIG_DFL);
}

bool Console::poll() {
    if (ctrlBreak) return false;

    /* Non-blocking check */
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    if (select(1, &fds, NULL, NULL, &tv) <= 0) return false;

    char ch;
    if (read(0, &ch, 1) <= 0) return false;

    if (ch == '\n' || ch == '\r') {
        inputBuf[s_editLen] = '\0';
        s_editLen   = 0;
        inputDirty  = true;
        return true;
    }
    if (ch == 3) {   /* Ctrl-C */
        ctrlBreak = true;
        return false;
    }
    if (ch == 127 || ch == '\b') {   /* backspace */
        if (s_editLen > 0) {
            s_editLen--;
            inputBuf[s_editLen] = '\0';
            inputDirty = true;
        }
        return false;
    }
    if (ch >= 0x20 && ch < 0x7F && s_editLen + 1 < CONSOLE_INPUT_LEN) {
        inputBuf[s_editLen++] = ch;
        inputBuf[s_editLen]   = '\0';
        inputDirty = true;
    }
    return false;
}

void Console::checkBreak() { /* SIGINT handler sets ctrlBreak */ }

void Console::putSpinner() {
    static const char frames[] = "|/-\\";
    static int idx = 0;
    fprintf(stdout, "%c\r", frames[idx]);
    fflush(stdout);
    idx = (idx + 1) & 3;
}

void Console::putToken(const char *tok) {
    fputs(tok, stdout);
    fflush(stdout);
}

void Console::putLine(const char *line) {
    puts(line);
    fflush(stdout);
}

void Console::putPrompt() {
    fputs("\n> ", stdout);
    fflush(stdout);
}
