#ifndef CODY_SOCKET_H
#define CODY_SOCKET_H

#include "codytypes.h"

/* Platform-neutral TCP socket interface.
   Implemented in DOS/socket_dos.cpp (mTCP) and UNIX/socket_unix.cpp (POSIX).

   All methods return 0 on success, -1 on error unless noted otherwise.
   recv() returns bytes received (>0), 0 if nothing available yet, -1 on error.
   send() returns bytes sent (>=0) or -1 on error. */

class Socket {
public:
    Socket();

    /* Resolve host and open a TCP connection.  Blocking on Unix; on DOS
       the caller must drive the mTCP event loop around this call. */
    int8_t  connectTo(const char *host, uint16_t port);

    /* Non-blocking send.  May return fewer bytes than len. */
    int16_t send(const uint8_t *buf, uint16_t len);

    /* Non-blocking receive.  Returns 0 if nothing ready yet. */
    int16_t recv(uint8_t *buf, uint16_t len);

    /* Blocking receive with timeout in milliseconds.
       Returns bytes received (>0), 0 on timeout, -1 on error/close. */
    int16_t recvWait(uint8_t *buf, uint16_t len, uint32_t timeoutMs);

    void    close();

    bool    isConnected()     const;
    bool    isRemoteClosed()  const;

private:
#ifdef __WATCOMC__
    /* DOS: pointer to mTCP TcpSocket, managed by TcpSocketMgr */
    void *tcpSock; /* TcpSocket* — void* avoids pulling in mTCP headers here */
#else
    int  fd;
    bool connected;
    bool remoteClosed;
#endif
};

/* On DOS the main loop must call this every iteration to service mTCP.
   On Unix this is a no-op.
   n controls how many times to pump the mTCP event loop. */
void driveNetwork(int n = 1);

/* Initialize the network stack.  Must be called before any Socket use.
   On DOS: calls Utils::parseEnv() + Utils::initStack().
   On Unix: no-op.
   Returns 0 on success, -1 on failure. */
int8_t networkInit();

/* Shut down the network stack.  Call before exit.
   On DOS: calls Utils::endStack().
   On Unix: no-op. */
void networkShutdown();

#endif
