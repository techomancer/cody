#ifndef CODY_HTTP_H
#define CODY_HTTP_H

#include "codytypes.h"
#include "socket.h"

#define HTTP_RECV_BUF  (8192)  /* socket recv staging buffer */
#define HTTP_WORK_BUF  (4096)  /* accumulated data awaiting line parse */
#define HTTP_HDR_BUF   (2048)  /* outgoing request headers */
#define HTTP_LINE_BUF   (512)  /* one response line extracted from work buf */

/* HTTP POST client with keep-alive and streaming response support.
   The connection is kept open across turns and reused.  If the server
   closes it postJson() will reconnect automatically. */
class HttpClient {
public:
    HttpClient();

    /* Store host/port for (re)connection.  Does not connect yet. */
    int8_t init(const char *host, uint16_t port);

    /* Single-buffer POST (convenience wrapper around the three below). */
    int8_t postJson(const char *path,
                    const char *body, uint32_t bodyLen);

    /* Split-send POST: connect + send HTTP header with known content-length. */
    int8_t postBegin(const char *path, uint32_t contentLength);

    /* Send a chunk of the body (call one or more times after postBegin). */
    int8_t postSend(const char *buf, uint16_t len);

    /* Finish the POST (just flushes — no body terminator needed for HTTP/1.1). */
    int8_t postEnd();

    /* GET path.  Blocking: reads entire response body into buf (null-terminated).
       Returns number of bytes written, or -1 on error. */
    int16_t getAll(const char *path, char *buf, uint16_t bufLen);

    /* Non-blocking line read from the streaming response.
       Returns:
         N > 0  a line of N bytes was written to buf
         0      no full line ready yet
        -1      error (socket error or unexpected close)
        -2      end of response body (normal completion) */
    int16_t readLine(char *buf, uint16_t bufLen);
    static const int16_t READ_EOF = -2;

    /* Signal end of response consumption so the connection can be reused.
       Does NOT close the socket unless the server closed it. */
    void endResponse();

    /* Force-close the socket (use on error or shutdown). */
    void close();

    bool isDone() const;

private:
    /* Pull more data from socket into workBuf. */
    int8_t fillWork();

    /* Try to extract one '\n'-terminated line from workBuf.
       limit>0 restricts scanning to that many bytes (for chunked mode).
       Returns line length (>0) if found, 0 if not enough data yet. */
    uint16_t getLineFromWork(char *buf, uint16_t bufLen, uint16_t limit = 0,
                             uint16_t *consumedOut = NULL);

    /* Send all bytes in sbuf[0..slen-1].  Loops until done or error.
       Returns 0 on success, -1 on error. */
    int8_t sendAll(const uint8_t *sbuf, uint16_t slen);

    /* Read and discard HTTP response headers.
       Sets chunkedEncoding.  Returns 0 on success, -1 on error. */
    int8_t readHeaders();

    /* Strip chunk-size line from front of workBuf if chunkedEncoding.
       Returns 0 ok, -1 parse error, 1 need more data. */
    int8_t stripChunkHeader();

    Socket   sock;
    char     hostname[80];
    uint16_t port;

    uint8_t  workBuf[HTTP_WORK_BUF];
    uint16_t workStart;   /* index of first unprocessed byte */
    uint16_t workLen;     /* bytes of valid data from workStart */

    char     hdrBuf[HTTP_HDR_BUF]; /* scratch for building request headers */

    bool     headersConsumed;
    bool     chunkedEncoding;
    bool     inChunkData;      /* inside a chunk body (past its size line) */
    uint32_t chunkRemain;      /* bytes left in current chunk */
};

#endif
