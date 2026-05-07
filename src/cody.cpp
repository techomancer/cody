#include "console.h"
#include "ui.h"
#include "log.h"
#include "http.h"
#include "json.h"
#include "session.h"
#include "tools.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define CODY_DEFAULT_HOST  "192.168.86.3"
#define CODY_DEFAULT_PORT  (1234)
#define CODY_MODELS_PATH   "/v1/models"
#define CODY_API_PATH      "/v1/chat/completions"

#define MODELS_MAX        (16)
#define MODELS_NAME_LEN   SESSION_MODEL_LEN

/* -------------------------------------------------------------------------
   Globals
   ------------------------------------------------------------------------- */

static HttpClient g_http;
static Session    g_session;
static JsonParser g_json;

static char g_lineBuf[HTTP_LINE_BUF];
static char g_toolResult[TOOL_RESULT_LEN];
static char g_toolsJsonBuf[TOOLS_JSON_BUF];
static char g_finishReason[32];

static char    g_pendingToolName[TOOL_NAME_LEN];
static char    g_pendingToolArgs[TOOL_RESULT_LEN];
static bool    g_hasPendingTool;

static char    g_models[MODELS_MAX][MODELS_NAME_LEN];
static uint8_t g_modelCount;
static char    g_modelsBuf[2048];

enum State {
    STATE_IDLE,
    STATE_STREAMING,
    STATE_TOOL_PENDING
};

static State    g_state        = STATE_IDLE;
static void    *g_toolCtx      = NULL;
static bool     g_debug        = false;
static bool     g_reasoning    = false;
static bool     g_logging      = false;
static uint16_t g_logNum       = 0;
static FILE    *g_logRsp       = NULL;
static bool     g_spinnerActive = false;
static time_t   g_streamStart  = 0;
static bool     g_gotData      = false;

#define STREAM_IDLE_TIMEOUT_SECS (300)  /* 5 minutes — LMStudio can be slow to start */

static void logRspLine(const char *line);
static void updateCtxStatus();
static void cmdCompact();

/* -------------------------------------------------------------------------
   updateCtxStatus — refresh the status bar with current context stats
   ------------------------------------------------------------------------- */
static void updateCtxStatus() {
    uint32_t used = g_session.contentUsed();
    uint32_t cap  = g_session.contentCapacity();
    uint32_t pct  = (used * 100u) / cap;
    char tmp[UI_STATUS_MAX];
    snprintf(tmp, sizeof(tmp), " ctx: %lu%% %luB/%luKB  |  model: %s",
             (unsigned long)pct,
             (unsigned long)used,
             (unsigned long)(cap / 1024),
             g_session.model);
    UI::setStatus(tmp);
}

/* -------------------------------------------------------------------------
   processChunk — handle one SSE data line
   ------------------------------------------------------------------------- */
static bool processChunk(const char *line) {
    if (g_debug) codylog("[%s]", line);
    logRspLine(line);

    if (strncmp(line, "data: ", 6) == 0) line += 6;
    if (strcmp(line, "[DONE]") == 0) return true;
    if (line[0] != '{') return false;

    char token[JSON_TOKEN_MAX];
    if (g_json.getDeltaReasoning(line, token, sizeof(token)) == 0 && token[0]) {
        if (g_reasoning) {
            UI::putToken(token);
        } else {
            UI::putSpinner();
            g_spinnerActive = true;
        }
    }

    if (g_json.getDeltaContent(line, token, sizeof(token)) == 0 && token[0]) {
        char *r = token, *w = token;
        while (*r) { if ((unsigned char)*r < 0x80) *w++ = *r; r++; }
        *w = '\0';
        if (!token[0]) return false;
        if (g_spinnerActive) {
            UI::clearCurrentLine();
            g_spinnerActive = false;
        }
        UI::putToken(token);
        g_session.appendAssistant(token);
    }

    char tcName[TOOL_NAME_LEN];
    char tcArgs[TOOL_RESULT_LEN];
    if (g_json.getDeltaToolCall(line, 0, tcName, sizeof(tcName),
                                tcArgs, sizeof(tcArgs)) == 0) {
        if (tcName[0]) {
            strncpy(g_pendingToolName, tcName, TOOL_NAME_LEN - 1);
            g_pendingToolName[TOOL_NAME_LEN - 1] = '\0';
            g_hasPendingTool = true;
        }
        if (tcArgs[0]) {
            uint16_t cur = (uint16_t)strlen(g_pendingToolArgs);
            uint16_t rem = (uint16_t)(sizeof(g_pendingToolArgs) - cur - 1);
            uint16_t add = (uint16_t)strlen(tcArgs);
            if (add > rem) add = rem;
            memcpy(g_pendingToolArgs + cur, tcArgs, add);
            g_pendingToolArgs[cur + add] = '\0';
        }
    }

    if (g_json.getFinishReason(line, g_finishReason, sizeof(g_finishReason)) == 0) {
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
   Logging helpers
   ------------------------------------------------------------------------- */
static void logOpenPair() {
    if (!g_logging) return;
    g_logNum++;
    char fname[16];
    const char *tools = g_toolsJsonBuf[0] ? g_toolsJsonBuf : NULL;
    snprintf(fname, sizeof(fname), "REQ%04u.TXT", (unsigned)g_logNum);
    FILE *f = fopen(fname, "w");
    if (f) {
        uint16_t n;
        n = g_session.serializePrefix(); fwrite(g_session.msgBuf, 1, n, f);
        for (uint16_t i = 0; i < g_session.count; i++) {
            n = g_session.serializeMsgHeader(i); fwrite(g_session.msgBuf, 1, n, f);
            uint32_t pos = 0;
            do {
                n = g_session.serializeMsgContent(i, &pos);
                if (n) fwrite(g_session.msgBuf, 1, n, f);
            } while (pos < g_session.descs[i].len);
        }
        n = g_session.serializeSuffix(tools); fwrite(g_session.msgBuf, 1, n, f);
        fclose(f);
    }
    snprintf(fname, sizeof(fname), "RSP%04u.TXT", (unsigned)g_logNum);
    g_logRsp = fopen(fname, "w");
}

static void logRspLine(const char *line) {
    if (g_logRsp) { fputs(line, g_logRsp); fputc('\n', g_logRsp); }
}

static void logCloseRsp() {
    if (g_logRsp) { fclose(g_logRsp); g_logRsp = NULL; }
}

/* -------------------------------------------------------------------------
   startRequest
   ------------------------------------------------------------------------- */
#define CTX_COMPACT_THRESHOLD (95)

static const char COMPACT_PROMPT[] =
    "Please summarize the entire conversation so far in 3-5 sentences "
    "covering the key topics, decisions, and current state. "
    "Reply with only the summary, no preamble.";

static int8_t startRequest() {
    const char *tools = g_toolsJsonBuf[0] ? g_toolsJsonBuf : NULL;
    uint32_t totalLen = g_session.measureRequest(tools);
    if (g_debug) codylog("req len=%lu", (unsigned long)totalLen);

    if (g_http.postBegin(CODY_API_PATH, totalLen) != 0) {
        UI::appendLine("[error: connection failed]");
        return -1;
    }

    logOpenPair();

    uint16_t n = g_session.serializePrefix();
    if (g_http.postSend(g_session.msgBuf, n) != 0) {
        UI::appendLine("[error: send failed]");
        return -1;
    }

    for (uint8_t i = 0; i < g_session.count; i++) {
        n = g_session.serializeMsgHeader(i);
        if (g_http.postSend(g_session.msgBuf, n) != 0) {
            UI::appendLine("[error: send failed]");
            return -1;
        }
        uint32_t pos = 0;
        do {
            n = g_session.serializeMsgContent(i, &pos);
            if (n && g_http.postSend(g_session.msgBuf, n) != 0) {
                UI::appendLine("[error: send failed]");
                return -1;
            }
        } while (pos < g_session.descs[i].len);
    }

    n = g_session.serializeSuffix(tools);
    if (g_http.postSend(g_session.msgBuf, n) != 0) {
        UI::appendLine("[error: send failed]");
        return -1;
    }

    if (g_http.postEnd() != 0) {
        UI::appendLine("[error: send failed]");
        return -1;
    }

    if (g_debug) codylog("posted ok");
    return 0;
}

/* -------------------------------------------------------------------------
   fetchModels
   ------------------------------------------------------------------------- */
static uint8_t fetchModels() {
    g_modelCount = 0;
    int16_t len = g_http.getAll(CODY_MODELS_PATH, g_modelsBuf, sizeof(g_modelsBuf));
    if (len <= 0) return 0;

    const char *p    = g_modelsBuf;
    const char *data = strstr(p, "\"data\"");
    if (!data) return 0;
    p = data + 6;

    while (*p && g_modelCount < MODELS_MAX) {
        const char *idKey = strstr(p, "\"id\"");
        if (!idKey) break;
        p = idKey + 4;
        while (*p == ' ' || *p == '\t' || *p == ':') p++;
        if (*p != '"') continue;
        p++;
        uint8_t i = 0;
        while (*p && *p != '"' && i < MODELS_NAME_LEN - 1) {
            g_models[g_modelCount][i++] = *p++;
        }
        g_models[g_modelCount][i] = '\0';
        if (i > 0) g_modelCount++;
        if (*p == '"') p++;
    }
    return g_modelCount;
}

/* -------------------------------------------------------------------------
   Command handlers
   ------------------------------------------------------------------------- */
static void cmdModels() {
    if (g_modelCount == 0) {
        UI::appendLine("No models available.");
        return;
    }
    char line[MODELS_NAME_LEN + 8];
    for (uint8_t i = 0; i < g_modelCount; i++) {
        snprintf(line, sizeof(line), "%u: %s", (unsigned)(i + 1), g_models[i]);
        UI::appendLine(line);
    }
}

static bool cmdSelectModel(const char *arg) {
    if (!arg || !*arg) {
        UI::appendLine(g_session.model);
        return false;
    }
    bool isNum = true;
    for (const char *c = arg; *c; c++) {
        if (*c < '0' || *c > '9') { isNum = false; break; }
    }
    const char *name = NULL;
    if (isNum) {
        uint8_t idx = (uint8_t)(atoi(arg) - 1);
        if (idx < g_modelCount) name = g_models[idx];
    } else {
        for (uint8_t i = 0; i < g_modelCount; i++) {
            if (strcmp(g_models[i], arg) == 0) { name = g_models[i]; break; }
        }
    }
    if (!name) {
        UI::appendLine("[model not found]");
        return false;
    }
    strncpy(g_session.model, name, SESSION_MODEL_LEN - 1);
    g_session.model[SESSION_MODEL_LEN - 1] = '\0';
    char msg[SESSION_MODEL_LEN + 16];
    snprintf(msg, sizeof(msg), "[model: %s]", name);
    UI::appendLine(msg);
    updateCtxStatus();
    return true;
}

static void cmdCompact() {
    if (g_session.count == 0) {
        UI::appendLine("[nothing to compact]");
        return;
    }
    UI::appendLine("[compacting...]");

    if (!g_session.addMsg("user", COMPACT_PROMPT)) {
        UI::appendLine("[compact: context full, cannot add prompt]");
        return;
    }
    g_session.resetAssistant();

    if (startRequest() != 0) {
        UI::appendLine("[compact: request failed]");
        return;
    }

    char lineBuf[HTTP_LINE_BUF];
    bool done = false;
    while (!done) {
        int16_t rc = g_http.readLine(lineBuf, sizeof(lineBuf));
        if (rc > 0) {
            if (processChunk(lineBuf)) {
                logCloseRsp();
                g_http.close();
                done = true;
            }
        } else if (rc == HttpClient::READ_EOF) {
            logCloseRsp();
            g_http.close();
            done = true;
        } else if (rc < 0) {
            logCloseRsp();
            g_http.close();
            UI::appendLine("[compact: stream error]");
            return;
        }
    }

    const char *summary = g_session.asmBuf;
    if (summary[0] == '\0') {
        UI::appendLine("[compact: empty summary]");
        g_session.resetAssistant();
        return;
    }

    char summaryBuf[SESSION_ASM_BUF];
    strncpy(summaryBuf, summary, SESSION_ASM_BUF - 1);
    summaryBuf[SESSION_ASM_BUF - 1] = '\0';

    g_session.clearHistory();
    g_session.addMsg("assistant", summaryBuf);
    g_session.resetAssistant();

    UI::appendLine("[compacted]");
    updateCtxStatus();
}

static bool handleSlashCmd(const char *input) {
    if (input[0] != '/') return false;

    if (strcmp(input, "/quit") == 0 || strcmp(input, "/exit") == 0) {
        Console::ctrlBreak = true;
        return true;
    }
    if (strcmp(input, "/models") == 0) {
        cmdModels();
        return true;
    }
    if (strncmp(input, "/model", 6) == 0) {
        const char *arg = input + 6;
        while (*arg == ' ') arg++;
        cmdSelectModel(arg);
        return true;
    }
    if (strcmp(input, "/debug") == 0) {
        g_debug = !g_debug;
        UI::appendLine(g_debug ? "[debug on]" : "[debug off]");
        return true;
    }
    if (strcmp(input, "/reasoning") == 0) {
        g_reasoning = !g_reasoning;
        UI::appendLine(g_reasoning ? "[reasoning on]" : "[reasoning off]");
        return true;
    }
    if (strcmp(input, "/log") == 0) {
        g_logging = !g_logging;
        UI::appendLine(g_logging ? "[log on]" : "[log off]");
        return true;
    }
    if (strcmp(input, "/compact") == 0) {
        cmdCompact();
        return true;
    }
    if (strcmp(input, "/help") == 0 || strcmp(input, "/?") == 0) {
        UI::appendLine("/help, /?       show this help");
        UI::appendLine("/models         list available models");
        UI::appendLine("/model <name>   switch to a model");
        UI::appendLine("/compact        compact conversation history");
        UI::appendLine("/reasoning      toggle reasoning mode");
        UI::appendLine("/debug          toggle debug output");
        UI::appendLine("/log            toggle conversation logging");
        UI::appendLine("/quit, /exit    quit cody");
        return true;
    }
    UI::appendLine("[unknown command]");
    return true;
}

/* -------------------------------------------------------------------------
   main
   ------------------------------------------------------------------------- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -h HOST   LLM server host (default: %s)\n"
        "  -p PORT   LLM server port (default: %d)\n"
        "  -m MODEL  Model name\n"
        "  -s SYSP   System prompt\n",
        prog, CODY_DEFAULT_HOST, CODY_DEFAULT_PORT);
}

int main(int argc, char *argv[]) {
    const char *host      = CODY_DEFAULT_HOST;
    int         port      = CODY_DEFAULT_PORT;
    const char *model     = NULL;
    const char *sysprompt = NULL;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-h") == 0 && i+1 < argc) { host      = argv[++i]; }
        else if (strcmp(argv[i], "-p") == 0 && i+1 < argc) { port      = atoi(argv[++i]); }
        else if (strcmp(argv[i], "-m") == 0 && i+1 < argc) { model     = argv[++i]; }
        else if (strcmp(argv[i], "-s") == 0 && i+1 < argc) { sysprompt = argv[++i]; }
        else { usage(argv[0]); return 1; }
    }

    Console::init();
    toolsInit();

    if (networkInit() != 0) {
        fprintf(stderr, "Failed to initialize network stack\n");
        return 1;
    }

    if (g_http.init(host, (uint16_t)port) != 0) {
        fprintf(stderr, "Failed to init HTTP client\n");
        networkShutdown();
        return 1;
    }

    g_modelCount = 0;
    if (fetchModels() > 0 && !model) {
        model = g_models[0];
    }
    if (!model) model = "default";

    if (!g_session.init(model)) {
        fprintf(stderr, "cody: out of memory\n");
        return 1;
    }
    if (sysprompt) g_session.addMsg("system", sysprompt);

    buildToolsJson(g_toolsJsonBuf, sizeof(g_toolsJsonBuf));

    /* Init TUI after all stderr startup messages are done */
    UI::init();
    logSetTuiReady(1);

    /* Show startup info in the output area */
    {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "cody - %s:%d  model: %s", host, port, model);
        UI::appendLine(tmp);
        if (g_modelCount > 0) {
            snprintf(tmp, sizeof(tmp),
                     "%u models available - /models to list, /model to switch",
                     (unsigned)g_modelCount);
            UI::appendLine(tmp);
        }
        UI::appendLine("Type your message and press Enter.  /quit to exit.");
    }
    updateCtxStatus();
    UI::syncInput();

    g_state              = STATE_IDLE;
    g_hasPendingTool     = false;
    g_pendingToolName[0] = '\0';
    g_pendingToolArgs[0] = '\0';

    /* ---- Main loop ---- */
    while (1) {
        if (Console::ctrlBreak && g_state == STATE_IDLE) break;

        driveNetwork();

        /* Refresh input line every iteration so it mirrors live editing */
        UI::syncInput();

        switch (g_state) {

        case STATE_IDLE:
            if (Console::ctrlBreak) break;
            if (Console::poll()) {
                static char inputCopy[CONSOLE_INPUT_LEN];
                strncpy(inputCopy, Console::inputBuf, CONSOLE_INPUT_LEN - 1);
                inputCopy[CONSOLE_INPUT_LEN - 1] = '\0';
                Console::inputBuf[0] = '\0';
                Console::inputDirty  = true;
                UI::syncInput();
                const char *input = inputCopy;

                if (input[0] == '\0') break;

                if (handleSlashCmd(input)) break;

                {
                    static char userLine[CONSOLE_INPUT_LEN + 2];
                    userLine[0] = '>';
                    userLine[1] = ' ';
                    strncpy(userLine + 2, input, sizeof(userLine) - 3);
                    userLine[sizeof(userLine) - 1] = '\0';
                    UI::appendUserLine(userLine);
                }

                if (!g_session.addMsg("user", input)) {
                    UI::appendLine("[ctx full — /compact first]");
                    updateCtxStatus();
                    break;
                }
                g_session.resetAssistant();
                g_hasPendingTool     = false;
                g_spinnerActive      = false;
                g_streamStart        = time(NULL);
                g_gotData            = false;
                g_pendingToolName[0] = '\0';
                g_pendingToolArgs[0] = '\0';
                if (startRequest() == 0) {
                    g_state = STATE_STREAMING;
                }
            }
            break;

        case STATE_STREAMING: {
            if (Console::ctrlBreak) {
                logCloseRsp();
                g_http.close();
                UI::clearCurrentLine();
                UI::appendLine("[aborted]");
                g_state = STATE_IDLE;
                Console::ctrlBreak = false;
                break;
            }
            int16_t rc = g_http.readLine(g_lineBuf, sizeof(g_lineBuf));
            if (rc > 0) {
                g_gotData   = true;
                bool done = processChunk(g_lineBuf);
                if (done) {
                    logCloseRsp();
                    g_http.close();
                    if (g_spinnerActive) {
                        UI::clearCurrentLine();
                        g_spinnerActive = false;
                    }
                    g_session.commitAssistant();
                    if (g_hasPendingTool && g_pendingToolName[0]) {
                        g_state = STATE_TOOL_PENDING;
                    } else {
                        updateCtxStatus();
                        uint32_t pct = ((uint32_t)g_session.contentUsed() * 100u)
                                       / g_session.contentCapacity();
                        if (pct >= CTX_COMPACT_THRESHOLD) {
                            UI::appendLine("[auto-compact triggered]");
                            cmdCompact();
                        }
                        g_state = STATE_IDLE;
                    }
                }
            } else if (rc == HttpClient::READ_EOF) {
                logCloseRsp();
                g_http.close();
                if (g_spinnerActive) {
                    UI::clearCurrentLine();
                    g_spinnerActive = false;
                }
                g_session.commitAssistant();
                if (g_hasPendingTool && g_pendingToolName[0]) {
                    g_state = STATE_TOOL_PENDING;
                } else {
                    updateCtxStatus();
                    uint32_t pct = ((uint32_t)g_session.contentUsed() * 100u)
                                   / g_session.contentCapacity();
                    if (pct >= CTX_COMPACT_THRESHOLD) {
                        UI::appendLine("[auto-compact triggered]");
                        cmdCompact();
                    }
                    g_state = STATE_IDLE;
                }
            } else if (rc < 0) {
                logCloseRsp();
                g_http.close();
                UI::clearCurrentLine();
                UI::appendLine("[error: stream interrupted]");
                g_state = STATE_IDLE;
            } else {
                if (!g_gotData) {
                    if (time(NULL) - g_streamStart > STREAM_IDLE_TIMEOUT_SECS) {
                        logCloseRsp();
                        g_http.close();
                        UI::clearCurrentLine();
                        UI::appendLine("[error: no response from server]");
                        g_state = STATE_IDLE;
                    }
                }
            }
            break;
        }

        case STATE_TOOL_PENDING: {
            char toolMsg[TOOL_NAME_LEN + 16];
            snprintf(toolMsg, sizeof(toolMsg), "[tool: %s]", g_pendingToolName);
            UI::appendLine(toolMsg);

            int trc = toolDispatch(g_pendingToolName, g_pendingToolArgs,
                                   g_toolResult, sizeof(g_toolResult),
                                   g_toolCtx);
            (void)trc;
            g_session.addMsg("tool", g_toolResult);

            g_hasPendingTool     = false;
            g_pendingToolName[0] = '\0';
            g_pendingToolArgs[0] = '\0';
            g_session.resetAssistant();
            g_streamStart        = time(NULL);
            g_gotData            = false;

            if (startRequest() == 0) {
                g_state = STATE_STREAMING;
            } else {
                g_state = STATE_IDLE;
            }
            break;
        }

        } /* switch */

    } /* while */

    g_http.close();
    networkShutdown();
    UI::shutdown();
    Console::shutdown();
    return 0;
}
