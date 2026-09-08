#include <cerrno>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#else
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "net_utils.hpp"

static bool valid_instrument(std::string_view s) {
    return s == "JNST" || s == "IMCT";
}

// Print all complete '\n'-terminated lines from buffer using a read cursor
// instead of erasing from the front on every newline (avoids O(n^2) shifts).
static void print_complete_lines(std::string& buffer, size_t& offset) {
    while (true) {
        const size_t pos = buffer.find('\n', offset);
        if (pos == std::string::npos) break;

        std::string_view line(buffer.data() + offset, pos - offset);
        offset = pos + 1;

        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        std::cout << line << '\n' << std::flush;
    }

    // Compact: only copy-shift when we've consumed a significant portion.
    if (offset >= buffer.size() / 2 || offset >= 4096) {
        buffer.erase(0, offset);
        offset = 0;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0]
                  << " <host> <port> [JNST|IMCT ...]\n";
        return 1;
    }

    int fd = connect_tcp(argv[1], argv[2]);
    if (fd == -1) {
        std::cerr << "Failed to connect to server\n";
        return 1;
    }

    for (int i = 3; i < argc; ++i) {
        if (!valid_instrument(argv[i])) {
            std::cerr << "Invalid instrument: " << argv[i] << '\n';
            close(fd);
            return 1;
        }
        if (!send_all_blocking(fd, std::string("SUBSCRIBE ") + argv[i] + "\n")) {
            std::cerr << "Failed to subscribe to " << argv[i] << '\n';
            close(fd);
            return 1;
        }
    }

    std::cout << "Connected as market-data client.\n";
    if (argc > 3) {
        std::cout << "Initial subscriptions sent.\n";
    }
    std::cout << "Enter SUBSCRIBE/UNSUBSCRIBE/QUIT commands, or wait for TRADE updates.\n"
              << std::flush;

    std::string socket_buffer;
    size_t socket_offset = 0;
    std::string stdin_buffer;
    size_t stdin_offset = 0;
    bool running = true;

    while (running) {
        pollfd fds[2] = {
            {fd, POLLIN, 0},
            {STDIN_FILENO, POLLIN, 0}
        };

        int ready = poll(fds, 2, -1);
        if (ready == -1) {
            if (errno == EINTR) continue;
            std::cerr << "poll() failed\n";
            break;
        }

        if (fds[0].revents & (POLLERR | POLLNVAL)) {
            std::cerr << "Server connection failed\n";
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            char buffer[8192];
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                socket_buffer.append(buffer, static_cast<size_t>(n));
                print_complete_lines(socket_buffer, socket_offset);
            } else if (n == 0) {
                std::cout << "Server closed the connection.\n";
                break;
            } else if (errno != EINTR) {
                std::cerr << "recv() failed\n";
                break;
            }
        }

        if (fds[1].revents & POLLIN) {
            char buffer[4096];
            ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n == 0) {
                shutdown(fd, SHUT_WR);
                break;
            }
            if (n < 0) {
                if (errno != EINTR) break;
            } else {
                stdin_buffer.append(buffer, static_cast<size_t>(n));
                while (true) {
                    const size_t pos = stdin_buffer.find('\n', stdin_offset);
                    if (pos == std::string::npos) break;

                    std::string_view sv(stdin_buffer.data() + stdin_offset,
                                       pos - stdin_offset);
                    stdin_offset = pos + 1;

                    if (!sv.empty() && sv.back() == '\r') sv.remove_suffix(1);

                    std::string line(sv);
                    line += '\n';
                    if (!send_all_blocking(fd, line)) {
                        running = false;
                        break;
                    }
                    if (sv == "QUIT") {
                        running = false;
                        break;
                    }
                }

                // Compact stdin buffer.
                if (stdin_offset >= stdin_buffer.size() / 2 || stdin_offset >= 4096) {
                    stdin_buffer.erase(0, stdin_offset);
                    stdin_offset = 0;
                }
            }
        }
    }

    close(fd);
    return 0;
}
