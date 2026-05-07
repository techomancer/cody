#include "session.h"
#include "log.h"
#include "json.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

bool Session::init(const char *modelName) {
    strncpy(model, modelName, SESSION_MODEL_LEN - 1);
    model[SESSION_MODEL_LEN - 1] = '\0';
    count   = 0;
    ctxUsed = 0;
    ctxBuf = (char *)malloc(SESSION_CTX_BUF);
    msgBuf = (char *)malloc(SESSION_MSG_BUF);
    asmBuf = (char *)malloc(SESSION_ASM_BUF);
    if (!ctxBuf || !msgBuf || !asmBuf) {
        codylog("session: malloc failed ctx=%p msg=%p asm=%p",
                (void*)ctxBuf, (void*)msgBuf, (void*)asmBuf);
        return false;
    }
    asmBuf[0] = '\0';
    msgBuf[0] = '\0';
    return true;
}

void Session::clearHistory() {
    count   = 0;
    ctxUsed = 0;
}

bool Session::addMsg(const char *role, const char *content) {
    if (count >= SESSION_MAX_MSGS) return false;

    uint16_t len  = (uint16_t)strlen(content);
    uint32_t need = (uint32_t)len + 1; /* include NUL */
    if (ctxUsed + need > SESSION_CTX_BUF) return false;

    MsgDesc &d = descs[count];
    d.offset = ctxUsed;
    d.len    = len;
    strncpy(d.role, role, SESSION_ROLE_LEN - 1);
    d.role[SESSION_ROLE_LEN - 1] = '\0';

    memcpy(ctxBuf + ctxUsed, content, len + 1);
    ctxUsed += need;
    count++;
    return true;
}

void Session::appendAssistant(const char *token) {
    uint32_t cur = (uint32_t)strlen(asmBuf);
    if (cur + 1 >= SESSION_ASM_BUF) return; /* full — drop token */
    uint32_t rem = SESSION_ASM_BUF - cur - 1;
    strncat(asmBuf, token, (size_t)rem);
}

void Session::commitAssistant() {
    if (asmBuf[0] != '\0') {
        if (!addMsg("assistant", asmBuf)) {
            codylog("session: commitAssistant: context full, reply truncated");
        }
    }
    asmBuf[0] = '\0';
}

void Session::resetAssistant() {
    asmBuf[0] = '\0';
}

/* -------------------------------------------------------------------------
   Streaming serialization — build request in small pieces
   ------------------------------------------------------------------------- */

uint16_t Session::serializePrefix() {
    JsonBuilder b;
    b.reset(msgBuf, SESSION_MSG_BUF);
    b.beginObj();
    b.str("model", model);
    b.beginArr("messages");
    return b.finish();
}

uint16_t Session::serializeMsgHeader(uint16_t i) {
    /* Writes: [,]{"role":"...","content":" — note trailing open quote, no close */
    JsonBuilder b;
    b.reset(msgBuf, SESSION_MSG_BUF);
    if (i > 0) b.setNeedComma(true);
    b.beginObj();
    b.str("role", descs[i].role);
    /* Open the content key + opening quote manually; content streamed separately */
    b.appendKey("content");
    b.appendChar('"');
    return b.finish();
}

/* Escape one printable/escapable char into buf[pos..].
   Returns bytes written, or 0 if not enough room (caller must flush). */
static uint8_t escapeChar(char *buf, uint16_t limit, uint16_t pos, unsigned char c) {
    if (c == '"' || c == '\\') {
        if (pos + 2 > limit) return 0;
        buf[pos] = '\\'; buf[pos+1] = (char)c; return 2;
    }
    if (c == '\n') { if (pos + 2 > limit) return 0; buf[pos]='\\'; buf[pos+1]='n'; return 2; }
    if (c == '\r') { if (pos + 2 > limit) return 0; buf[pos]='\\'; buf[pos+1]='r'; return 2; }
    if (c == '\t') { if (pos + 2 > limit) return 0; buf[pos]='\\'; buf[pos+1]='t'; return 2; }
    if (pos + 1 > limit) return 0;
    buf[pos] = (char)c; return 1;
}

uint16_t Session::serializeMsgContent(uint16_t i, uint32_t *srcPos) {
    const char *src    = ctxBuf + descs[i].offset;
    uint16_t    srcLen = descs[i].len;
    uint16_t    out    = 0;
    /* Leave 3 bytes headroom for closing `"}` + NUL */
    uint16_t    limit  = SESSION_MSG_BUF - 3;

    while (*srcPos < srcLen && out < limit) {
        unsigned char c = (unsigned char)src[*srcPos];
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            /* drop control char */
            (*srcPos)++;
            continue;
        }
        uint8_t written = escapeChar(msgBuf, limit, out, c);
        if (written == 0) break; /* not enough room — flush this chunk first */
        (*srcPos)++;
        out += written;
    }

    bool done = (*srcPos >= srcLen);
    if (done) {
        /* Close the content string and the object.
           Headroom of 3 bytes was reserved above so this always fits. */
        msgBuf[out++] = '"';
        msgBuf[out++] = '}';
        /* Sanity check — should never fire */
        if (out < 2 || msgBuf[out-2] != '"' || msgBuf[out-1] != '}') {
            codylog("session: BUG: final chunk missing closing !}");
        }
    }
    msgBuf[out] = '\0';
    return out;
}

uint32_t Session::escapedContentLen(uint16_t i) const {
    const char *src    = ctxBuf + descs[i].offset;
    uint16_t    srcLen = descs[i].len;
    uint32_t    len    = 0;
    for (uint16_t k = 0; k < srcLen; k++) {
        unsigned char c = (unsigned char)src[k];
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') len += 2;
        else if (c >= 0x20) len += 1;
        /* control chars dropped */
    }
    return len;
}

uint16_t Session::serializeSuffix(const char *toolsJson) {
    JsonBuilder b;
    b.reset(msgBuf, SESSION_MSG_BUF);
    b.endArr();
    b.boolean("stream", true);
    if (toolsJson && toolsJson[0] != '\0') {
        b.strRaw("tools", toolsJson);
    }
    b.endObj();
    return b.finish();
}

uint32_t Session::measureRequest(const char *toolsJson) const {
    Session *self = const_cast<Session *>(this);
    uint32_t total = 0;
    total += self->serializePrefix();
    for (uint16_t i = 0; i < count; i++) {
        /* header: [,]{"role":"...","content":" */
        total += self->serializeMsgHeader(i);
        /* escaped content */
        total += escapedContentLen(i);
        /* closing: "} */
        total += 2;
    }
    total += self->serializeSuffix(toolsJson);
    return total;
}
