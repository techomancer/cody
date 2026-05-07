#include "json.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
   JsonBuilder
   ------------------------------------------------------------------------- */

void JsonBuilder::reset(char *b, uint16_t len) {
    buf       = b;
    bufLen    = len;
    pos       = 0;
    needComma = false;
}

void JsonBuilder::appendChar(char c) {
    if (pos + 1 < bufLen) buf[pos++] = c;
}

void JsonBuilder::appendStr(const char *s) {
    while (*s && pos + 1 < bufLen) buf[pos++] = *s++;
}

void JsonBuilder::appendEscaped(const char *s) {
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if      (c == '"')  { appendChar('\\'); appendChar('"');  }
        else if (c == '\\') { appendChar('\\'); appendChar('\\'); }
        else if (c == '\n') { appendChar('\\'); appendChar('n');  }
        else if (c == '\r') { appendChar('\\'); appendChar('r');  }
        else if (c == '\t') { appendChar('\\'); appendChar('t');  }
        else if (c < 0x20)  { /* drop other control chars */ }
        else                { appendChar((char)c); }
    }
}

void JsonBuilder::comma() {
    if (needComma) appendChar(',');
    needComma = true;
}

void JsonBuilder::appendKey(const char *key) {
    comma();
    appendChar('"');
    appendEscaped(key);
    appendStr("\":");
}

void JsonBuilder::beginObj() {
    comma();
    appendChar('{');
    needComma = false;
}

void JsonBuilder::endObj() {
    appendChar('}');
    needComma = true;
}

void JsonBuilder::beginArr(const char *key) {
    appendKey(key);
    appendChar('[');
    needComma = false;
}

void JsonBuilder::endArr() {
    appendChar(']');
    needComma = true;
}

void JsonBuilder::str(const char *key, const char *val) {
    appendKey(key);
    appendChar('"');
    appendEscaped(val);
    appendChar('"');
}

void JsonBuilder::strRaw(const char *key, const char *valJson) {
    appendKey(key);
    appendStr(valJson);
}

void JsonBuilder::boolean(const char *key, bool v) {
    appendKey(key);
    appendStr(v ? "true" : "false");
}

void JsonBuilder::num(const char *key, int32_t v) {
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%ld", (long)v);
    appendKey(key);
    appendStr(tmp);
}

uint16_t JsonBuilder::finish() {
    if (pos < bufLen) buf[pos] = '\0';
    else buf[bufLen - 1] = '\0';
    return pos;
}


/* -------------------------------------------------------------------------
   JsonParser helpers
   ------------------------------------------------------------------------- */

const char *JsonParser::skipWs(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

const char *JsonParser::skipValue(const char *p) {
    if (!p) return NULL;
    p = skipWs(p);
    if (!*p) return NULL;

    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '\\') { p++; if (*p) p++; continue; }
            if (*p == '"')  { p++; return p; }
            p++;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (*p == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                /* skip string to avoid misinterpreting braces inside strings */
                p++;
                while (*p) {
                    if (*p == '\\') { p++; if (*p) p++; continue; }
                    if (*p == '"')  { p++; break; }
                    p++;
                }
                continue;
            }
            if (*p == open)  depth++;
            if (*p == close) depth--;
            p++;
        }
        return (depth == 0) ? p : NULL;
    }
    /* number, bool, null: scan until delimiter */
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p;
}

/* Copy unescaped string value from src (pointing at opening '"') into out. */
const char *JsonParser::copyStringVal(const char *src,
                                       char *out, uint16_t outLen) {
    if (!src || *src != '"') return NULL;
    src++;
    uint16_t i = 0;
    while (*src && i + 1 < outLen) {
        if (*src == '\\') {
            src++;
            if (!*src) return NULL;
            switch (*src) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/';  break;
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *src; break;
            }
            src++;
        } else if (*src == '"') {
            out[i] = '\0';
            return src + 1;
        } else {
            out[i++] = *src++;
        }
    }
    /* ran out of buffer or hit end of input without closing quote */
    if (i < outLen) out[i] = '\0';
    return (*src == '"') ? src + 1 : NULL;
}

/* Find the value of `key` inside the JSON object fragment starting at src.
   src should point at the opening '{' or just inside it.
   Returns pointer to start of value (after ':' and whitespace), or NULL. */
const char *JsonParser::findKey(const char *src, const char *key) {
    if (!src) return NULL;
    /* advance past '{' if present */
    src = skipWs(src);
    if (*src == '{') src++;

    size_t keyLen = strlen(key);

    while (*src) {
        src = skipWs(src);
        if (*src == '}' || *src == '\0') break;
        if (*src == ',') { src++; continue; }
        if (*src != '"') break; /* malformed */

        /* read key name */
        const char *keyStart = src + 1;
        /* find end of key */
        const char *keyEnd = keyStart;
        while (*keyEnd && *keyEnd != '"') {
            if (*keyEnd == '\\') keyEnd++;
            if (*keyEnd) keyEnd++;
        }
        src = (*keyEnd == '"') ? keyEnd + 1 : keyEnd;
        src = skipWs(src);
        if (*src != ':') break;
        src++;
        src = skipWs(src);

        size_t foundLen = (size_t)(keyEnd - keyStart);
        if (foundLen == keyLen && memcmp(keyStart, key, keyLen) == 0) {
            return src; /* points at value */
        }
        /* skip value */
        src = skipValue(src);
        if (!src) break;
    }
    return NULL;
}

const char *JsonParser::arrayElem(const char *src, uint8_t n) {
    if (!src) return NULL;
    src = skipWs(src);
    if (*src != '[') return NULL;
    src++;
    uint8_t i = 0;
    while (*src) {
        src = skipWs(src);
        if (*src == ']' || !*src) return NULL;
        if (*src == ',') { src++; continue; }
        if (i == n) return src;
        src = skipValue(src);
        if (!src) return NULL;
        i++;
    }
    return NULL;
}


/* -------------------------------------------------------------------------
   JsonParser public methods
   ------------------------------------------------------------------------- */

int8_t JsonParser::getString(const char *json, const char *key,
                              char *out, uint16_t outLen) {
    const char *v = findKey(json, key);
    if (!v) return -1;
    v = skipWs(v);
    if (*v != '"') return -1;
    return copyStringVal(v, out, outLen) ? 0 : -1;
}

int8_t JsonParser::getBool(const char *json, const char *key, bool &out) {
    const char *v = findKey(json, key);
    if (!v) return -1;
    v = skipWs(v);
    if (strncmp(v, "true", 4) == 0)  { out = true;  return 0; }
    if (strncmp(v, "false", 5) == 0) { out = false; return 0; }
    return -1;
}

int8_t JsonParser::getNested(const char *json,
                              const char *key1, const char *key2,
                              char *out, uint16_t outLen) {
    const char *v1 = findKey(json, key1);
    if (!v1) return -1;
    v1 = skipWs(v1);
    if (*v1 != '{') return -1;
    const char *v2 = findKey(v1, key2);
    if (!v2) return -1;
    v2 = skipWs(v2);
    if (*v2 != '"') return -1;
    return copyStringVal(v2, out, outLen) ? 0 : -1;
}

/* -------------------------------------------------------------------------
   OpenAI SSE streaming helpers
   Chunk format:
     {"choices":[{"delta":{"content":"tok"},"finish_reason":null}],...}
   Tool call chunk:
     {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"name":"...","arguments":"{"}}]},"finish_reason":null}]}
   Final chunk:
     {"choices":[{"delta":{},"finish_reason":"stop"}]}
   ------------------------------------------------------------------------- */

/* Navigate to choices[0].delta */
static const char *getDelta(const char *json) {
    const char *choices = JsonParser::findKey(json, "choices");
    if (!choices) return NULL;
    choices = JsonParser::skipWs(choices);
    const char *c0 = JsonParser::arrayElem(choices, 0);
    if (!c0) return NULL;
    c0 = JsonParser::skipWs(c0);
    if (*c0 != '{') return NULL;
    const char *delta = JsonParser::findKey(c0, "delta");
    if (!delta) return NULL;
    delta = JsonParser::skipWs(delta);
    return (*delta == '{') ? delta : NULL;
}

int8_t JsonParser::getDeltaContent(const char *json, char *out, uint16_t outLen) {
    const char *delta = getDelta(json);
    if (!delta) return -1;
    const char *v = findKey(delta, "content");
    if (!v) return -1;
    v = skipWs(v);
    if (*v == 'n') return -1; /* null */
    if (*v != '"') return -1;
    return copyStringVal(v, out, outLen) ? 0 : -1;
}

int8_t JsonParser::getDeltaReasoning(const char *json, char *out, uint16_t outLen) {
    const char *delta = getDelta(json);
    if (!delta) return -1;
    const char *v = findKey(delta, "reasoning_content");
    if (!v) return -1;
    v = skipWs(v);
    if (*v == 'n') return -1; /* null */
    if (*v != '"') return -1;
    return copyStringVal(v, out, outLen) ? 0 : -1;
}

int8_t JsonParser::getFinishReason(const char *json, char *out, uint16_t outLen) {
    /* finish_reason is in choices[0], not in delta */
    const char *choices = findKey(json, "choices");
    if (!choices) return -1;
    choices = skipWs(choices);
    const char *c0 = arrayElem(choices, 0);
    if (!c0) return -1;
    c0 = skipWs(c0);
    if (*c0 != '{') return -1;
    const char *v = findKey(c0, "finish_reason");
    if (!v) return -1;
    v = skipWs(v);
    if (*v == 'n') return -1; /* null — not finished yet */
    if (*v != '"') return -1;
    return copyStringVal(v, out, outLen) ? 0 : -1;
}

int8_t JsonParser::getDeltaToolCall(const char *json, uint8_t idx,
                                     char *name,     uint16_t nameLen,
                                     char *argsJson, uint16_t argsLen) {
    const char *delta = getDelta(json);
    if (!delta) return 1;

    const char *tcArr = findKey(delta, "tool_calls");
    if (!tcArr) return 1;
    tcArr = skipWs(tcArr);
    if (*tcArr != '[') return 1;

    const char *elem = arrayElem(tcArr, idx);
    if (!elem) return 1;
    elem = skipWs(elem);
    if (*elem != '{') return -1;

    const char *fn = findKey(elem, "function");
    if (!fn) return -1;
    fn = skipWs(fn);
    if (*fn != '{') return -1;

    /* name — may be absent in continuation chunks (only first chunk has it) */
    if (name && nameLen > 0) {
        name[0] = '\0';
        const char *nv = findKey(fn, "name");
        if (nv) {
            nv = skipWs(nv);
            if (*nv == '"') copyStringVal(nv, name, nameLen);
        }
    }

    /* arguments — a JSON string that accumulates across chunks.
       Copy its unescaped content (the partial JSON fragment). */
    if (argsJson && argsLen > 0) {
        argsJson[0] = '\0';
        const char *av = findKey(fn, "arguments");
        if (av) {
            av = skipWs(av);
            if (*av == '"') copyStringVal(av, argsJson, argsLen);
        }
    }

    return 0;
}
