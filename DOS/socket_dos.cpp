#include "../src/socket.h"
#include "../src/console.h"
#include "../src/log.h"
#include <conio.h>
#include <time.h>

/* mTCP headers */
#include "tcp.h"
#include "tcpsockm.h"
#include "dns.h"
#include "arp.h"
#include "packet.h"
#include "timer.h"
#include "utils.h"

#define CONNECT_TIMEOUT_MS  (10000ul)
#define CLOSE_TIMEOUT_MS    (500ul)

#define RECV_BUF_SIZE       (16384u)

/* -----------------------------------------------------------------------
   Ctrl-Break / Ctrl-C handler — sets Console::ctrlBreak
   ----------------------------------------------------------------------- */

static void __interrupt __far ctrlBreakHandler() {
    Console::ctrlBreak = true;
}

/* -----------------------------------------------------------------------
   networkInit / networkShutdown — mTCP stack lifecycle
   ----------------------------------------------------------------------- */

int8_t networkInit() {
    int8_t rc;
    rc = Utils::parseEnv();
    if (rc != 0) { codylog("networkInit: parseEnv rc=%d", (int)rc); return -1; }
    rc = Utils::initStack(1, TCP_SOCKET_RING_SIZE, ctrlBreakHandler, ctrlBreakHandler);
    if (rc != 0) { codylog("networkInit: initStack rc=%d", (int)rc); return -1; }
    return 0;
}

void networkShutdown() {
    Utils::endStack();
}

/* -----------------------------------------------------------------------
   driveNetwork — called by HttpClient on every I/O loop iteration
   ----------------------------------------------------------------------- */

void driveNetwork(int n) {
    for (int i = 0; i < n; i++) {
        PACKET_PROCESS_MULT(5);
        Tcp::drivePackets();
        Arp::driveArp();
    }
}

/* -----------------------------------------------------------------------
   Socket implementation
   ----------------------------------------------------------------------- */

Socket::Socket() : tcpSock(NULL) {}

int8_t Socket::connectTo(const char *host, uint16_t port) {
    /* Free any leftover socket from a previous connection */
    if (tcpSock != NULL) {
        TcpSocket *s = (TcpSocket *)tcpSock;
        tcpSock = NULL;
        s->close();
        TcpSocketMgr::freeSocket(s);
    }

    /* Resolve hostname */
    IpAddr_t addr;
    int8_t rc = Dns::resolve((char *)host, addr, 1);
    if (rc < 0) { codylog("connectTo: DNS resolve failed"); return -1; }

    if (rc == 1) {
        /* Wait for DNS response */
        clockTicks_t start = TIMER_GET_CURRENT();
        while (Dns::isQueryPending()) {
            driveNetwork();
            Dns::drivePendingQuery();
            if (Timer_diff(start, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
                codylog("connectTo: DNS timeout");
                return -1;
            }
        }
        rc = Dns::resolve((char *)host, addr, 0);
        if (rc != 0) { codylog("connectTo: DNS re-resolve failed"); return -1; }
    }

    TcpSocket *s = TcpSocketMgr::getSocket();
    if (s == NULL) { codylog("connectTo: getSocket NULL"); return -1; }

    rc = s->setRecvBuffer(RECV_BUF_SIZE);
    if (rc != 0) { codylog("connectTo: setRecvBuffer rc=%d", (int)rc); return -1; }

    static uint16_t s_localPort = 2048u;
    s_localPort = (s_localPort < 0xFFFEu) ? s_localPort + 1 : 2048u;

    rc = s->connectNonBlocking(s_localPort, addr, port);
    if (rc != 0) { codylog("connectTo: connectNonBlocking rc=%d", (int)rc); return -1; }

    clockTicks_t start = TIMER_GET_CURRENT();
    while (1) {
        driveNetwork();
        if (s->isConnectComplete()) break;
        if (s->isClosed()) {
            codylog("connectTo: closed during connect");
            s->close();
            TcpSocketMgr::freeSocket(s);
            return -1;
        }
        if (Timer_diff(start, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(CONNECT_TIMEOUT_MS)) {
            codylog("connectTo: connect timeout");
            s->close();
            TcpSocketMgr::freeSocket(s);
            return -1;
        }
    }

    tcpSock = s;
    return 0;
}

int16_t Socket::send(const uint8_t *buf, uint16_t len) {
    if (tcpSock == NULL) return -1;
    TcpSocket *s = (TcpSocket *)tcpSock;
    int16_t rc = s->send((uint8_t *)buf, len);
    if (rc < 0) {
        if (rc == TCP_RC_NO_XMIT_BUFFERS) return 0;
        codylog("send: rc=%d state=%d", (int)rc, (int)s->state);
        return -1;
    }
    while (s->outgoing.entries > 0) {
        PACKET_PROCESS_MULT(5);
        Tcp::drivePackets();
        Arp::driveArp();
    }
    return rc;
}

int16_t Socket::recv(uint8_t *buf, uint16_t len) {
    if (tcpSock == NULL) return -1;
    TcpSocket *s = (TcpSocket *)tcpSock;
    int16_t rc = s->recv(buf, len);
    if (rc < 0) {
        codylog("recv: rc=%d state=%d", (int)rc, (int)s->state);
        return -1;
    }
    return rc;
}

int16_t Socket::recvWait(uint8_t *buf, uint16_t len, uint32_t timeoutMs) {
    if (tcpSock == NULL) return -1;
    TcpSocket *s = (TcpSocket *)tcpSock;
    clockTicks_t start = TIMER_GET_CURRENT();
    while (1) {
        driveNetwork();
        int16_t rc = s->recv(buf, len);
        if (rc > 0) return rc;
        if (rc < 0) { codylog("recvWait: rc=%d", (int)rc); return -1; }
        if (s->isRemoteClosed()) return -1;
        if (Timer_diff(start, TIMER_GET_CURRENT()) > TIMER_MS_TO_TICKS(timeoutMs)) return 0;
    }
}

void Socket::close() {
    if (tcpSock == NULL) return;
    TcpSocket *s = (TcpSocket *)tcpSock;
    tcpSock = NULL;
    s->close();
    TcpSocketMgr::freeSocket(s);
}

bool Socket::isConnected() const {
    if (tcpSock == NULL) return false;
    TcpSocket *s = (TcpSocket *)tcpSock;
    return s->isConnectComplete() != 0;
}

bool Socket::isRemoteClosed() const {
    if (tcpSock == NULL) return true;
    TcpSocket *s = (TcpSocket *)tcpSock;
    return s->isRemoteClosed() != 0;
}
