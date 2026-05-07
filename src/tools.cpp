#include "tools.h"
#include "json.h"
#include <string.h>
#include <stdio.h>

Tool    g_tools[TOOL_MAX];
uint8_t g_toolCount = 0;

int toolDispatch(const char *name, const char *argsJson,
                 char *result, uint16_t resultLen, void *ctx) {
    for (uint8_t i = 0; i < g_toolCount; i++) {
        if (strcmp(g_tools[i].name, name) == 0) {
            return g_tools[i].fn(argsJson, result, resultLen, ctx);
        }
    }
    snprintf(result, resultLen, "error: unknown tool '%s'", name);
    return -1;
}

/* Build the Ollama tools array:
   [{"type":"function","function":{"name":"...","description":"...",
     "parameters":{"type":"object","properties":{}}}},...] */
int16_t buildToolsJson(char *buf, uint16_t bufLen) {
    if (bufLen < 4) return -1;

    if (g_toolCount == 0) {
        buf[0] = '['; buf[1] = ']'; buf[2] = '\0';
        return 2;
    }

    /* Build each tool entry into a temp buffer, then assemble the array */
    uint16_t pos = 0;

#define APPEND(s) do { \
    const char *_s = (s); \
    while (*_s && pos + 1 < bufLen) buf[pos++] = *_s++; \
} while(0)

    buf[pos++] = '[';

    for (uint8_t i = 0; i < g_toolCount; i++) {
        if (i > 0) buf[pos++] = ',';

        /* Build the "function" sub-object in a temp buffer */
        char fnBuf[512];
        JsonBuilder fb;
        fb.reset(fnBuf, sizeof(fnBuf));
        fb.beginObj();
        fb.str("name",        g_tools[i].name);
        fb.str("description", g_tools[i].desc);
        fb.strRaw("parameters",
            "{\"type\":\"object\",\"properties\":{}}");
        fb.endObj();
        fb.finish();

        /* Build the outer tool object in another temp buffer */
        char entryBuf[600];
        JsonBuilder eb;
        eb.reset(entryBuf, sizeof(entryBuf));
        eb.beginObj();
        eb.str("type", "function");
        eb.strRaw("function", fnBuf);
        eb.endObj();
        eb.finish();

        APPEND(entryBuf);
    }

    if (pos + 2 < bufLen) { buf[pos++] = ']'; buf[pos] = '\0'; }
    else return -1;

#undef APPEND

    return (int16_t)pos;
}
