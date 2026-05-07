#ifndef CODY_TOOLS_H
#define CODY_TOOLS_H

#include "codytypes.h"

#define TOOL_MAX         (8)
#define TOOL_NAME_LEN   (32)
#define TOOL_DESC_LEN  (128)
#define TOOL_RESULT_LEN (512)  /* max bytes a tool can write to result */
#define TOOLS_JSON_BUF (2048)  /* buffer for the tools JSON array sent to LLM */

/* ctx is caller-owned application context passed through at dispatch time.
   Tools use it to invoke callbacks or access app state. */
typedef int (*ToolFn)(const char *argsJson, char *result,
                      uint16_t resultLen, void *ctx);

struct Tool {
    char   name[TOOL_NAME_LEN];
    char   desc[TOOL_DESC_LEN];
    ToolFn fn;
};

/* Platform-specific tool impls populate this table via toolsInit() */
extern Tool    g_tools[];
extern uint8_t g_toolCount;

/* Called once at startup by platform-specific code to register tools */
void    toolsInit();

/* Find tool by name and call it, passing ctx through to the tool function.
   Returns 0 on success, -1 if tool not found, tool's own rc otherwise. */
int     toolDispatch(const char *name, const char *argsJson,
                     char *result, uint16_t resultLen, void *ctx);

/* Build the Ollama tools JSON array (array of function-schema objects).
   Returns length written into buf, or -1 on overflow. */
int16_t buildToolsJson(char *buf, uint16_t bufLen);

#endif
