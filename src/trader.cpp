#include <cerrno>
#include <iostream>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_utils.hpp"

static void print_complete_lines(std::string& buffer) {
    while (true) {
        size_t pos = buffer.find('\n');
        if (pos == std::string::npos) return;
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::cout << line << '\n' << std::flush;
    }
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <host> <port> <username>\n";
        return 1;
    }

    int fd = connect_tcp(argv[1], argv[2]);
    if (fd == -1) {
        std::cerr << "Failed to connect to server\n";
        return 1;
    }

    if (!send_all_blocking(fd, std::string("LOGIN ") + argv[3] + "\n")) {
        std::cerr << "Failed to send LOGIN\n";
        close(fd);
        return 1;
    }

    std::cout << "Connected as trader '" << argv[3] << "'.\n";
    std::cout << "Enter BUY/SELL/CANCEL/QUIT commands.\n" << std::flush;

    std::string socket_buffer;
    std::string stdin_buffer;
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
                print_complete_lines(socket_buffer);
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
                    size_t pos = stdin_buffer.find('\n');
                    if (pos == std::string::npos) break;
                    std::string line = stdin_buffer.substr(0, pos);
                    stdin_buffer.erase(0, pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!send_all_blocking(fd, line + "\n")) {
                        running = false;
                        break;
                    }
                    if (line == "QUIT") {
                        running = false;
                        break;
                    }
                }
            }
        }
    }

    close(fd);
    return 0;
}
