#include "http.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

HttpClient::HttpClient()
    : port(0), workStart(0), workLen(0),
      headersConsumed(false), chunkedEncoding(false),
      inChunkData(false), chunkRemain(0)
{
    hostname[0] = '\0';
}

int8_t HttpClient::init(const char *host, uint16_t p) {
    strncpy(hostname, host, sizeof(hostname) - 1);
    hostname[sizeof(hostname) - 1] = '\0';
    port = p;
    return 0;
}

/* -------------------------------------------------------------------------
   sendAll — loop until all bytes sent or error
   ------------------------------------------------------------------------- */

int8_t HttpClient::sendAll(const uint8_t *sbuf, uint16_t slen) {
    uint16_t sent = 0;
    uint16_t stalls = 0;
    while (sent < slen) {
        /* Drive before every send attempt so ACKs are processed and the
           outgoing window stays open */
        driveNetwork();
        int16_t rc = sock.send(sbuf + sent, slen - sent);
        if (rc < 0) {
            codylog("sendAll: send error at sent=%u/%u",
                    (unsigned)sent, (unsigned)slen);
            return -1;
        }
        if (rc == 0) {
            stalls++;
            if (stalls % 100 == 0)
                codylog("sendAll: stalled %u times, sent=%u/%u",
                        (unsigned)stalls, (unsigned)sent, (unsigned)slen);
            if (stalls > 5000) {
                codylog("sendAll: stall timeout, giving up");
                return -1;
            }
        } else {
            stalls = 0;
        }
        sent += (uint16_t)rc;
    }
    /* Pump hard after all bytes are queued to get them on the wire */
    driveNetwork(32);
    return 0;
}

/* -------------------------------------------------------------------------
   postJson — (re)connect if needed, send headers + body
   ------------------------------------------------------------------------- */

int8_t HttpClient::postBegin(const char *path, uint32_t contentLength) {
    workStart       = 0;
    workLen         = 0;
    headersConsumed = false;
    chunkedEncoding = false;
    inChunkData     = false;
    chunkRemain     = 0;

    sock.close();
    if (sock.connectTo(hostname, port) != 0) return -1;

    int n = snprintf(hdrBuf, sizeof(hdrBuf),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, hostname, (unsigned)port, (unsigned long)contentLength);

    if (n <= 0 || n >= (int)sizeof(hdrBuf)) return -1;
    return sendAll((uint8_t *)hdrBuf, (uint16_t)n);
}

int8_t HttpClient::postSend(const char *buf, uint16_t len) {
    return sendAll((uint8_t *)buf, len);
}

int8_t HttpClient::postEnd() {
    driveNetwork(64);
    return 0;
}

int8_t HttpClient::postJson(const char *path,
                             const char *body, uint32_t bodyLen) {
    if (postBegin(path, bodyLen) != 0) return -1;
    if (postSend(body, (uint16_t)bodyLen) != 0) return -1;
    return postEnd();
}

/* -------------------------------------------------------------------------
   fillWork — pull bytes from socket into work buffer
   ------------------------------------------------------------------------- */

int8_t HttpClient::fillWork() {
    if (workStart > 0 && workLen > 0) {
        memmove(workBuf, workBuf + workStart, workLen);
        workStart = 0;
    } else if (workLen == 0) {
        workStart = 0;
    }

    uint16_t space = (uint16_t)(sizeof(workBuf) - workLen);
    if (space == 0) {
        codylog("fillWork: workBuf full! workLen=%u", (unsigned)workLen);
        return 0;
    }

    driveNetwork();
    int16_t rc = sock.recv(workBuf + workLen, space);
    if (rc < 0) {
        return -1;
    }
    workLen += (uint16_t)rc;
    return 0;
}

/* -------------------------------------------------------------------------
   readHeaders — consume HTTP response headers
   ------------------------------------------------------------------------- */

int8_t HttpClient::readHeaders() {
    while (1) {
        /* Find \r\n in current buffer */
        bool foundLine = false;
        for (uint16_t i = workStart; i + 1 < workStart + workLen; i++) {
            if (workBuf[i] == '\r' && workBuf[i+1] == '\n') {
                uint16_t lineLen = i - workStart;
                if (lineLen == 0) {
                    /* Blank line = end of headers */
                    workStart += 2;
                    workLen   -= 2;
                    headersConsumed = true;
                    return 0;
                }
                char *line = (char *)(workBuf + workStart);
                if (lineLen >= 26) {
                    char tmp[27];
                    uint16_t cmp = lineLen < 26 ? lineLen : 26;
                    memcpy(tmp, line, cmp);
                    tmp[cmp] = '\0';
                    for (uint16_t k = 0; k < cmp; k++)
                        if (tmp[k] >= 'A' && tmp[k] <= 'Z') tmp[k] += 32;
                    if (strncmp(tmp, "transfer-encoding: chunked", 26) == 0)
                        chunkedEncoding = true;
                }
                workStart += lineLen + 2;
                workLen   -= lineLen + 2;
                foundLine  = true;
                break;
            }
        }
        if (!foundLine) {
            /* Only give up if remote is closed AND we have no buffered data */
            if (sock.isRemoteClosed() && workLen == 0) return -1;
            /* Block-spin: drive network and retry until at least 1 byte arrives */
            while (workLen == 0) {
                driveNetwork();
                if (fillWork() != 0) return -1;
                if (sock.isRemoteClosed() && workLen == 0) return -1;
            }
        }
    }
}

/* -------------------------------------------------------------------------
   stripChunkHeader — remove "hex-size\r\n" from workBuf front
   Returns: 0 ok, 1 need more data, -1 error
   ------------------------------------------------------------------------- */

int8_t HttpClient::stripChunkHeader() {
    for (uint16_t i = workStart; i + 1 < workStart + workLen; i++) {
        if (workBuf[i] == '\r' && workBuf[i+1] == '\n') {
            uint16_t hexLen = i - workStart;
            if (hexLen == 0 || hexLen > 8) return -1;
            char tmp[9];
            memcpy(tmp, workBuf + workStart, hexLen);
            tmp[hexLen] = '\0';
            unsigned long sz = 0;
            for (uint16_t k = 0; k < hexLen; k++) {
                char c = tmp[k];
                if      (c >= '0' && c <= '9') sz = sz * 16 + (unsigned long)(c - '0');
                else if (c >= 'a' && c <= 'f') sz = sz * 16 + (unsigned long)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') sz = sz * 16 + (unsigned long)(c - 'A' + 10);
                else if (c == ';') break;
                else return -1;
            }
            workStart   += hexLen + 2;
            workLen     -= hexLen + 2;
            chunkRemain  = (uint32_t)sz;
            inChunkData  = true;
            return 0;
        }
    }
    return 1;
}

/* -------------------------------------------------------------------------
   getLineFromWork — extract one '\n'-terminated line.
   If limit > 0, scan at most `limit` bytes (for chunked mode).
   ------------------------------------------------------------------------- */

uint16_t HttpClient::getLineFromWork(char *buf, uint16_t bufLen, uint16_t limit /*=0*/,
                                     uint16_t *consumedOut /*=NULL*/) {
    uint16_t scanLen = workLen;
    if (limit > 0 && limit < scanLen) scanLen = limit;
    for (uint16_t i = workStart; i < workStart + scanLen; i++) {
        if (workBuf[i] == '\n') {
            uint16_t lineLen = i - workStart;
            uint16_t consumed = lineLen + 1; /* content + \n */
            if (lineLen > 0 && workBuf[workStart + lineLen - 1] == '\r') {
                lineLen--;  /* strip \r from returned length */
                /* consumed already includes the \r (it's part of the raw bytes) */
            }
            uint16_t copy = lineLen < (uint16_t)(bufLen - 1) ? lineLen
                                                              : (uint16_t)(bufLen - 1);
            memcpy(buf, workBuf + workStart, copy);
            buf[copy] = '\0';
            workStart += consumed;
            workLen   -= consumed;
            if (consumedOut) *consumedOut = consumed;
            return lineLen;
        }
    }
    if (consumedOut) *consumedOut = 0;
    return 0;
}

/* -------------------------------------------------------------------------
   readLine — public non-blocking SSE line reader
   ------------------------------------------------------------------------- */

int16_t HttpClient::readLine(char *buf, uint16_t bufLen) {
    if (!headersConsumed) {
        if (readHeaders() != 0) {
            codylog("readLine: readHeaders failed");
            return -1;
        }
    }

    driveNetwork();
    if (fillWork() < 0) {
        codylog("readLine: fillWork failed (top)");
        return -1;
    }

    if (chunkedEncoding) {
        while (!inChunkData || chunkRemain == 0) {
            if (inChunkData && chunkRemain == 0) {
                /* Consume trailing \r\n after chunk body */
                while (workLen < 2) {
                    if (fillWork() < 0) {
                        codylog("readLine: fillWork failed (crlf)");
                        return -1;
                    }
                    if (workLen < 2 && sock.isRemoteClosed() && workLen == 0) {
                        codylog("readLine: remote closed waiting for crlf");
                        return -1;
                    }
                }
                if (workBuf[workStart] == '\r' && workBuf[workStart+1] == '\n') {
                    workStart += 2;
                    workLen   -= 2;
                }
                inChunkData = false;
            }
            int8_t rc = stripChunkHeader();
            if (rc == 1) {
                /* Need more data for the chunk header — spin until it arrives */
                if (sock.isRemoteClosed() && workLen == 0) {
                    codylog("readLine: remote closed waiting for chunk hdr");
                    return -1;
                }
                driveNetwork();
                if (fillWork() < 0) {
                    codylog("readLine: fillWork failed (chunk hdr)");
                    return -1;
                }
                return 0;
            }
            if (rc < 0) {
                codylog("readLine: stripChunkHeader failed");
                return -1;
            }
            if (chunkRemain == 0) {
                return HttpClient::READ_EOF;
            }
        }
    }

    uint16_t limit = 0;
    if (chunkedEncoding && chunkRemain > 0) {
        limit = (chunkRemain > 0xFFFFu) ? 0xFFFFu : (uint16_t)chunkRemain;
    }
    uint16_t consumed = 0;
    uint16_t lineLen = getLineFromWork(buf, bufLen, limit, &consumed);
    if (consumed > 0) {
        if (chunkedEncoding) {
            chunkRemain = (consumed < chunkRemain) ? chunkRemain - consumed : 0;
        }
        return (int16_t)lineLen;
    }

    if (sock.isRemoteClosed() && workLen == 0)
        return HttpClient::READ_EOF;
    if (workLen >= (uint16_t)(sizeof(workBuf) - 1))
        codylog("readLine: no newline in full workBuf (%u bytes)", (unsigned)workLen);
    return 0;
}

/* -------------------------------------------------------------------------
   getAll — blocking GET; reads entire response body into buf
   Returns bytes written (excluding NUL), or -1 on error.
   ------------------------------------------------------------------------- */

int16_t HttpClient::getAll(const char *path, char *buf, uint16_t bufLen) {
    /* Reset response state */
    workStart       = 0;
    workLen         = 0;
    headersConsumed = false;
    chunkedEncoding = false;
    inChunkData     = false;
    chunkRemain     = 0;

    /* Connect if needed */
    if (!sock.isConnected() || sock.isRemoteClosed()) {
        sock.close();
        if (sock.connectTo(hostname, port) != 0) return -1;
    }

    int n = snprintf(hdrBuf, sizeof(hdrBuf),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, hostname, (unsigned)port);
    if (n <= 0 || n >= (int)sizeof(hdrBuf)) return -1;

    if (sendAll((uint8_t *)hdrBuf, (uint16_t)n) != 0) {
        sock.close();
        if (sock.connectTo(hostname, port) != 0) return -1;
        if (sendAll((uint8_t *)hdrBuf, (uint16_t)n) != 0) return -1;
    }

    /* Consume headers */
    while (!headersConsumed) {
        driveNetwork();
        if (fillWork() < 0) return -1;
        /* readHeaders() is already spin-safe; call it once more data arrives */
        if (workLen > 0 && readHeaders() != 0) return -1;
    }

    /* Accumulate body */
    uint16_t total = 0;
    while (1) {
        driveNetwork();
        if (fillWork() < 0) return -1;

        if (chunkedEncoding) {
            /* Strip chunk header if not inside a chunk */
            while (!inChunkData || chunkRemain == 0) {
                if (inChunkData && chunkRemain == 0) {
                    /* Consume trailing \r\n after chunk body */
                    while (workLen < 2) {
                        driveNetwork();
                        if (fillWork() < 0) return -1;
                        if (workLen < 2 && sock.isRemoteClosed()) return -1;
                    }
                    if (workBuf[workStart] == '\r' && workBuf[workStart+1] == '\n') {
                        workStart += 2; workLen -= 2;
                    }
                    inChunkData = false;
                }
                int8_t rc = stripChunkHeader();
                if (rc == 1) {
                    /* need more data */
                    driveNetwork();
                    if (fillWork() < 0) return -1;
                    if (sock.isRemoteClosed() && workLen == 0) return -1;
                    continue;
                }
                if (rc < 0) return -1;
                if (chunkRemain == 0) goto done; /* terminal chunk */
            }

            /* Copy up to chunkRemain bytes into buf */
            uint32_t canCopy = chunkRemain < (uint32_t)(bufLen - 1 - total)
                               ? chunkRemain : (uint32_t)(bufLen - 1 - total);
            if (canCopy > workLen) canCopy = workLen;
            if (canCopy > 0) {
                memcpy(buf + total, workBuf + workStart, (uint16_t)canCopy);
                total       += (uint16_t)canCopy;
                workStart   += (uint16_t)canCopy;
                workLen     -= (uint16_t)canCopy;
                chunkRemain -= canCopy;
            }
        } else {
            /* Identity / Content-Length — copy whatever is in workBuf */
            if (workLen > 0) {
                uint16_t canCopy = workLen < (uint16_t)(bufLen - 1 - total)
                                   ? workLen : (uint16_t)(bufLen - 1 - total);
                memcpy(buf + total, workBuf + workStart, canCopy);
                total     += canCopy;
                workStart += canCopy;
                workLen   -= canCopy;
            }
            if (sock.isRemoteClosed() && workLen == 0) goto done;
        }

        /* Stop if buf is full */
        if (total >= bufLen - 1) break;
    }

done:
    buf[total] = '\0';
    sock.close();
    endResponse();
    return (int16_t)total;
}

/* -------------------------------------------------------------------------
   endResponse / close / isDone
   ------------------------------------------------------------------------- */

void HttpClient::endResponse() {
    /* Reset response-level state; keep socket open for reuse */
    workStart       = 0;
    workLen         = 0;
    headersConsumed = false;
    chunkedEncoding = false;
    inChunkData     = false;
    chunkRemain     = 0;
}

void HttpClient::close() {
    sock.close();
    endResponse();
}

bool HttpClient::isDone() const {
    return sock.isRemoteClosed() && workLen == 0;
}
