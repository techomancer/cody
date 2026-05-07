#ifndef CODY_JSON_H
#define CODY_JSON_H

#include "codytypes.h"

#define JSON_TOKEN_MAX (512)

/* Minimal JSON builder.  Caller owns the output buffer. */
class JsonBuilder {
public:
    void     reset(char *buf, uint16_t bufLen);
    void     setNeedComma(bool v) { needComma = v; }
    void     beginObj();
    void     endObj();
    void     beginArr(const char *key);
    void     endArr();
    void     str(const char *key, const char *val);
    void     strRaw(const char *key, const char *valJson); /* pre-escaped value */
    void     boolean(const char *key, bool v);
    void     num(const char *key, int32_t v);
    uint16_t finish(); /* null-terminates, returns total length */

    /* Low-level — public for streaming serialization */
    void     appendChar(char c);
    void     appendKey(const char *key); /* comma + quoted key + colon */

private:
    void     comma();
    void     appendStr(const char *s);
    void     appendEscaped(const char *s); /* JSON-escape string content */

    char    *buf;
    uint16_t bufLen;
    uint16_t pos;
    bool     needComma;
};


/* Minimal JSON parser — linear scanner, no allocation, no parse tree.
   All functions return 0 on success, -1 if key not found or type mismatch. */
class JsonParser {
public:
    /* Extract a string value for a top-level key */
    int8_t getString(const char *json, const char *key,
                     char *out, uint16_t outLen);

    /* Extract a boolean value for a top-level key */
    int8_t getBool(const char *json, const char *key, bool &out);

    /* Extract string at json["key1"]["key2"] */
    int8_t getNested(const char *json,
                     const char *key1, const char *key2,
                     char *out, uint16_t outLen);

    /* OpenAI SSE streaming helpers.
       A streaming chunk looks like:
         {"choices":[{"delta":{"content":"token"},"finish_reason":null}]}
       or for tool calls:
         {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"...","arguments":"..."}}]},"finish_reason":null}]}
       or the final chunk:
         {"choices":[{"delta":{},"finish_reason":"stop"}]}  (or "tool_calls") */

    /* Extract choices[0].delta.content.  Returns 0 on success, -1 if absent. */
    int8_t getDeltaContent(const char *json, char *out, uint16_t outLen);

    /* Extract choices[0].delta.reasoning_content (qwen3 thinking tokens).
       Returns 0 on success, -1 if absent. */
    int8_t getDeltaReasoning(const char *json, char *out, uint16_t outLen);

    /* Extract choices[0].finish_reason.  Returns 0 on success, -1 if absent/null. */
    int8_t getFinishReason(const char *json, char *out, uint16_t outLen);

    /* Extract the idx-th tool call from choices[0].delta.tool_calls[idx].
       name receives function.name, argsJson receives function.arguments (a JSON string).
       Returns 0 if found, 1 if idx out of range, -1 on parse error. */
    int8_t getDeltaToolCall(const char *json, uint8_t idx,
                            char *name,    uint16_t nameLen,
                            char *argsJson, uint16_t argsLen);

    /* Low-level helpers — public so file-static helpers in json.cpp can use them */
    static const char *findKey(const char *src, const char *key);
    static const char *copyStringVal(const char *src, char *out, uint16_t outLen);
    static const char *skipWs(const char *p);
    static const char *skipValue(const char *p);
    static const char *arrayElem(const char *src, uint8_t n);
};

#endif
