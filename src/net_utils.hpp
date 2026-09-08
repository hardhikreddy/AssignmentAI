#pragma once

#include <cerrno>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
typedef SSIZE_T ssize_t;
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

inline bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

// Disable Nagle's algorithm so small messages are sent immediately without
// waiting to coalesce with subsequent data (reduces latency).
inline void set_tcp_nodelay(int fd) {
    const int flag = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

// Increase kernel socket buffer sizes to 256 KB (from the typical 4–8 KB
// default). Larger buffers absorb bursts and reduce EAGAIN stalls when many
// messages arrive / depart in rapid succession.
inline void set_socket_buffers(int fd) {
    const int buf_size = 256 * 1024;
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
}

inline int connect_tcp(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0) {
        return -1;
    }

    int fd = -1;
    for (addrinfo* p = results; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) continue;

        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            set_tcp_nodelay(fd);
            break;
        }

        ::close(fd);
        fd = -1;
    }

    ::freeaddrinfo(results);
    return fd;
}

inline int create_listening_socket(const std::string& host,
                                   const std::string& port,
                                   int backlog) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0) {
        return -1;
    }

    int fd = -1;
    for (addrinfo* p = results; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) continue;

        const int reuse = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        if (::bind(fd, p->ai_addr, p->ai_addrlen) == 0 &&
            ::listen(fd, backlog) == 0) {
            break;
        }

        ::close(fd);
        fd = -1;
    }

    ::freeaddrinfo(results);
    return fd;
}

inline bool send_all_blocking(int fd, const std::string& data) {
    const char* ptr = data.data();
    size_t left = data.size();

    while (left > 0) {
        const ssize_t n = ::send(fd, ptr, left, MSG_NOSIGNAL);
        if (n > 0) {
            ptr += n;
            left -= static_cast<size_t>(n);
            continue;
        }
        if (n == -1 && errno == EINTR) continue;
        return false;
    }
    return true;
}