#ifndef SGH_NET_HOST_H
#define SGH_NET_HOST_H

/* Socket conventions shared by the WAP and Java guest boundaries. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET NetSocket;
typedef int NetAddrLen;
#define NET_INVALID INVALID_SOCKET

static inline int net_start(void)
{
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

static inline void net_stop(void)
{
    WSACleanup();
}

static inline void net_socket_close(NetSocket socket)
{
    closesocket(socket);
}

static inline int net_error(void)
{
    return WSAGetLastError();
}

static inline int net_would_block(void)
{
    return net_error() == WSAEWOULDBLOCK;
}

static inline int net_connect_pending(void)
{
    return net_would_block();
}

static inline int net_nonblocking(NetSocket socket)
{
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

static inline int net_readable_bytes(NetSocket socket, unsigned *bytes)
{
    u_long pending = 0;
    if (ioctlsocket(socket, FIONREAD, &pending))
        return 0;
    *bytes = pending;
    return 1;
}

static inline int net_ready(NetSocket socket, int write)
{
    fd_set ready, errors;
    struct timeval now = {0};
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    FD_ZERO(&errors);
    FD_SET(socket, &errors);
    return select(0, write ? NULL : &ready, write ? &ready : NULL, write ? &errors : NULL, &now) > 0;
}

static inline int net_send(NetSocket socket, const char *bytes, int size)
{
    return send(socket, bytes, size, 0);
}

static inline void net_sleep(unsigned milliseconds)
{
    Sleep(milliseconds);
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int NetSocket;
typedef socklen_t NetAddrLen;
#define NET_INVALID (-1)

static inline int net_start(void)
{
    return 1;
}

static inline void net_stop(void)
{
}

static inline void net_socket_close(NetSocket socket)
{
    close(socket);
}

static inline int net_error(void)
{
    return errno;
}

static inline int net_would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static inline int net_connect_pending(void)
{
    return errno == EINPROGRESS || net_would_block();
}

static inline int net_nonblocking(NetSocket socket)
{
    const int flags = fcntl(socket, F_GETFL);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

static inline int net_readable_bytes(NetSocket socket, unsigned *bytes)
{
    int pending = 0;
    if (ioctl(socket, FIONREAD, &pending))
        return 0;
    *bytes = pending > 0 ? (unsigned)pending : 0;
    return 1;
}

static inline int net_ready(NetSocket socket, int write)
{
    struct pollfd fd = {socket, write ? POLLOUT : POLLIN, 0};
    return poll(&fd, 1, 0) > 0;
}

static inline int net_send(NetSocket socket, const char *bytes, int size)
{
    /* A guest TCP peer can disconnect at any point without killing SGHemu. */
    return (int)send(socket, bytes, (size_t)size, MSG_NOSIGNAL);
}

static inline void net_sleep(unsigned milliseconds)
{
    struct timespec duration = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000};
    while (nanosleep(&duration, &duration) && errno == EINTR) {
    }
}
#endif

static inline NetSocket net_socket(int type, int protocol)
{
#ifndef _WIN32
    type |= SOCK_CLOEXEC;
#endif
    NetSocket s = socket(AF_INET, type, protocol);
    if (s != NET_INVALID && !net_nonblocking(s)) {
        net_socket_close(s);
        return NET_INVALID;
    }
    return s;
}

#endif
