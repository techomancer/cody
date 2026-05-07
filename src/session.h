#ifndef CODY_SESSION_H
#define CODY_SESSION_H

#include "codytypes.h"

#define SESSION_MAX_MSGS  (256)
#define SESSION_ROLE_LEN   (16)   /* "user","assistant","tool","system" */
#define SESSION_MODEL_LEN  (64)
#define SESSION_MSG_BUF   (4096)  /* scratch for one serialized message */
#define SESSION_ASM_BUF  (32768)  /* assembled assistant response — malloc'd */
#define SESSION_CTX_BUF  (65472)  /* flat content store (64K-64 for malloc overhead) */

struct MsgDesc {
    uint32_t offset;            /* byte offset into ctxBuf */
    uint16_t len;               /* content length (excluding NUL) */
    char     role[SESSION_ROLE_LEN];
};

struct Session {
    char    model[SESSION_MODEL_LEN];

    MsgDesc  descs[SESSION_MAX_MSGS];
    uint16_t count;             /* number of valid messages */

    uint32_t ctxUsed;           /* bytes used in ctxBuf (including NULs) */
    char    *ctxBuf;            /* SESSION_CTX_BUF bytes, malloc'd in init() */
    char    *msgBuf;            /* SESSION_MSG_BUF bytes, malloc'd in init() */
    char    *asmBuf;            /* SESSION_ASM_BUF bytes, malloc'd in init() */

    /* Returns false if malloc failed */
    bool  init(const char *modelName);

    /* Add a message. Returns false if context or descriptor table is full. */
    bool  addMsg(const char *role, const char *content);

    /* Wipe all messages but keep model. Used after compact. */
    void  clearHistory();

    /* Append to the current assistant message (used during streaming) */
    void  appendAssistant(const char *token);

    /* Commit the accumulated asmBuf as an "assistant" message */
    void  commitAssistant();

    /* Reset asmBuf to empty (call before a new streaming turn) */
    void  resetAssistant();

    /* Context usage stats */
    uint32_t contentUsed() const     { return ctxUsed; }
    uint32_t contentCapacity() const { return SESSION_CTX_BUF; }

    /* Compute total body length for Content-Length header.
       toolsJson may be NULL. */
    uint32_t measureRequest(const char *toolsJson) const;

    /* Serialize the static prefix into msgBuf: {"model":"...","messages":[
       Returns length written. */
    uint16_t serializePrefix();

    /* Serialize message i header into msgBuf (role/open, no content yet).
       Prepends comma if not first (i>0).  Returns length written. */
    uint16_t serializeMsgHeader(uint16_t i);

    /* Escape and write message i content into msgBuf in chunks of up to
       SESSION_MSG_BUF bytes.  Call repeatedly: pass *pos=0 on first call;
       returns bytes written this chunk, 0 when done.
       Writes the closing `"}` on the final chunk. */
    uint16_t serializeMsgContent(uint16_t i, uint32_t *pos);

    /* Compute escaped length of message i content (for measureRequest). */
    uint32_t escapedContentLen(uint16_t i) const;

    /* Serialize the closing part into msgBuf: ],"stream":true[,"tools":...]}
       toolsJson may be NULL.  Returns length written. */
    uint16_t serializeSuffix(const char *toolsJson);
};

#endif
