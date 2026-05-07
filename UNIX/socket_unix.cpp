#include "socket.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

Socket::Socket() : fd(-1), connected(false), remoteClosed(false) {}

int8_t Socket::connectTo(const char *host, uint16_t port) {
    close();

    struct hostent *he = gethostbyname(host);
    if (!he) return -1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (::connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        fd = -1;
        return -1;
    }

    /* Set non-blocking for recv so it returns 0 when nothing is ready */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    connected    = true;
    remoteClosed = false;
    return 0;
}

int16_t Socket::send(const uint8_t *buf, uint16_t len) {
    if (fd < 0) return -1;
    ssize_t n = ::send(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return (int16_t)n;
}

int16_t Socket::recv(uint8_t *buf, uint16_t len) {
    if (fd < 0) return -1;
    ssize_t n = ::recv(fd, buf, len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        remoteClosed = true;
        return -1;
    }
    if (n == 0) {
        remoteClosed = true;
        return 0;
    }
    return (int16_t)n;
}

void Socket::close() {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    connected    = false;
    remoteClosed = false;
}

bool Socket::isConnected() const {
    return connected && fd >= 0;
}

bool Socket::isRemoteClosed() const {
    return remoteClosed;
}

/* No-ops on Unix — mTCP stack management not needed */
void driveNetwork(int /*n*/) {}
int8_t networkInit() { return 0; }
void networkShutdown() {}
