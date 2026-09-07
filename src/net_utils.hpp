#pragma once

#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

inline bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
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