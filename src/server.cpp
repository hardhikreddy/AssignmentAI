#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <deque>
#include <iostream>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net_utils.hpp"

enum class Role { UNKNOWN, TRADER, MARKET_DATA };
enum class Side { BUY, SELL };

enum class Instrument { JNST = 0, IMCT = 1 };

static constexpr int MAX_VALUE = 2147483647;
static constexpr size_t MAX_INPUT_BUFFER = 64 * 1024;
static constexpr size_t MAX_OUTPUT_BUFFER = 8 * 1024 * 1024;
static constexpr int LISTEN_BACKLOG = 128;

struct Client {
    int fd = -1;
    uint64_t id = 0;
    Role role = Role::UNKNOWN;
    std::string username;
    std::unordered_set<int> subscriptions;
    std::string input;
    std::deque<std::string> output;
    size_t output_bytes = 0;
    size_t output_offset = 0;
    bool closing = false;
};

struct Order {
    int32_t id = 0;
    uint64_t owner_client_id = 0;
    Instrument instrument = Instrument::JNST;
    Side side = Side::BUY;
    int32_t price = 0;
    int32_t remaining = 0;
};

using OrderList = std::list<Order>;

struct OrderRef {
    Instrument instrument;
    Side side;
    int32_t price;
    OrderList::iterator iterator;
};

class ExchangeServer {
public:
    ExchangeServer(std::string host, std::string port)
        : host_(std::move(host)), port_(std::move(port)) {}

    int run() {
        std::signal(SIGPIPE, SIG_IGN);

        listen_fd_ = create_listening_socket(host_, port_, LISTEN_BACKLOG);
        if (listen_fd_ == -1) {
            std::cerr << "Failed to create/listen on " << host_ << ':' << port_ << "\n";
            return 1;
        }

        if (!set_nonblocking(listen_fd_)) {
            std::cerr << "Failed to make listening socket non-blocking\n";
            close(listen_fd_);
            return 1;
        }

        std::cout << "Exchange Server listening on " << host_ << ':' << port_ << "\n";

        while (true) {
            if (!poll_once()) break;
        }

        for (auto& [fd, client] : clients_) {
            close(fd);
        }
        clients_.clear();
        client_id_to_fd_.clear();
        close(listen_fd_);
        return 0;
    }

private:
    std::string host_;
    std::string port_;
    int listen_fd_ = -1;
    uint64_t next_client_id_ = 1;
    uint64_t next_order_id_ = 0;

    std::unordered_map<int, Client> clients_;
    std::unordered_map<uint64_t, int> client_id_to_fd_;
    std::unordered_map<std::string, int> trader_user_to_fd_;
    std::unordered_map<int32_t, OrderRef> order_index_;

    // [instrument][side][price] -> FIFO list of orders at that price.
    std::map<int32_t, OrderList> books_[2][2];

    static int instrument_index(Instrument instrument) {
        return static_cast<int>(instrument);
    }

    static int side_index(Side side) {
        return static_cast<int>(side);
    }

    static bool parse_int32(const std::string& text, int32_t& value, bool positive_only) {
        if (text.empty()) return false;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
        }

        long long parsed = 0;
        const char* first = text.data();
        const char* last = first + text.size();
        auto result = std::from_chars(first, last, parsed);
        if (result.ec != std::errc() || result.ptr != last) return false;
        if (positive_only) {
            if (parsed < 1 || parsed > MAX_VALUE) return false;
        } else {
            if (parsed < 0 || parsed > MAX_VALUE) return false;
        }
        value = static_cast<int32_t>(parsed);
        return true;
    }

    static bool parse_instrument(const std::string& text, Instrument& instrument) {
        if (text == "JNST") {
            instrument = Instrument::JNST;
            return true;
        }
        if (text == "IMCT") {
            instrument = Instrument::IMCT;
            return true;
        }
        return false;
    }

    static std::string instrument_name(Instrument instrument) {
        return instrument == Instrument::JNST ? "JNST" : "IMCT";
    }

    static std::string side_name(Side side) {
        return side == Side::BUY ? "BUY" : "SELL";
    }

    Client* find_client(int fd) {
        auto it = clients_.find(fd);
        return it == clients_.end() ? nullptr : &it->second;
    }

    Client* find_client_by_id(uint64_t id) {
        auto fd_it = client_id_to_fd_.find(id);
        if (fd_it == client_id_to_fd_.end()) return nullptr;
        return find_client(fd_it->second);
    }

    bool queue_message(Client& client, const std::string& message) {
        if (client.closing) return false;
        std::string data = message;
        if (data.empty() || data.back() != '\n') data.push_back('\n');

        if (client.output_bytes + data.size() > MAX_OUTPUT_BUFFER) {
            std::cerr << "Closing fd " << client.fd
                      << " because its output buffer exceeded "
                      << MAX_OUTPUT_BUFFER << " bytes\n";
            client.closing = true;
            return false;
        }

        client.output_bytes += data.size();
        client.output.push_back(std::move(data));
        return true;
    }

    bool queue_to_client_id(uint64_t client_id, const std::string& message) {
        Client* client = find_client_by_id(client_id);
        if (client == nullptr || client->closing) return false;
        return queue_message(*client, message);
    }

    void close_client(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;

        Client& client = it->second;
        if (client.role == Role::TRADER && !client.username.empty()) {
            auto user_it = trader_user_to_fd_.find(client.username);
            if (user_it != trader_user_to_fd_.end() && user_it->second == fd) {
                trader_user_to_fd_.erase(user_it);
            }
        }

        client_id_to_fd_.erase(client.id);
        close(fd);
        clients_.erase(it);
    }

    void cleanup_marked_clients() {
        std::vector<int> doomed;
        doomed.reserve(clients_.size());
        for (const auto& [fd, client] : clients_) {
            if (client.closing) doomed.push_back(fd);
        }
        for (int fd : doomed) close_client(fd);
    }

    void accept_ready_clients() {
        while (true) {
            int client_fd = accept(listen_fd_, nullptr, nullptr);
            if (client_fd == -1) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
                break;
            }

            if (!set_nonblocking(client_fd)) {
                close(client_fd);
                continue;
            }

            Client client;
            client.fd = client_fd;
            client.id = next_client_id_++;
            clients_.emplace(client_fd, std::move(client));
            client_id_to_fd_[clients_.at(client_fd).id] = client_fd;
        }
    }

    void receive_from_client(Client& client) {
        char buffer[8192];

        while (!client.closing) {
            ssize_t n = recv(client.fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                client.input.append(buffer, static_cast<size_t>(n));
                if (client.input.size() > MAX_INPUT_BUFFER) {
                    queue_message(client, "ERROR message too long");
                    client.closing = true;
                    break;
                }

                while (!client.closing) {
                    size_t newline = client.input.find('\n');
                    if (newline == std::string::npos) break;

                    std::string line = client.input.substr(0, newline);
                    client.input.erase(0, newline + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    process_message(client, line);
                }
                continue;
            }

            if (n == 0) {
                client.closing = true;
                break;
            }

            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            client.closing = true;
            break;
        }
    }

    void send_pending(Client& client) {
        while (!client.output.empty() && !client.closing) {
            std::string& message = client.output.front();
            const char* data = message.data() + client.output_offset;
            size_t remaining = message.size() - client.output_offset;

            ssize_t n = send(client.fd, data, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                client.output_offset += static_cast<size_t>(n);
                client.output_bytes -= static_cast<size_t>(n);
                if (client.output_offset == message.size()) {
                    client.output.pop_front();
                    client.output_offset = 0;
                }
                continue;
            }

            if (n == -1 && errno == EINTR) continue;
            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            client.closing = true;
            break;
        }
    }

    void handle_login(Client& client, const std::vector<std::string>& tokens) {
        if (client.role != Role::UNKNOWN) {
            queue_message(client, "ERROR client role already selected");
            return;
        }
        if (tokens.size() != 2 || tokens[1].empty()) {
            queue_message(client, "ERROR invalid LOGIN syntax");
            return;
        }
        const std::string& username = tokens[1];
        if (username.find_first_of(" \t\r\n") != std::string::npos) {
            queue_message(client, "ERROR invalid username");
            return;
        }
        if (trader_user_to_fd_.count(username) != 0) {
            queue_message(client, "ERROR username already in use");
            return;
        }

        client.role = Role::TRADER;
        client.username = username;
        trader_user_to_fd_[username] = client.fd;
        queue_message(client, "OK");
    }

    void handle_subscribe(Client& client, const std::vector<std::string>& tokens) {
        if (client.role == Role::UNKNOWN) {
            client.role = Role::MARKET_DATA;
        }
        if (client.role != Role::MARKET_DATA) {
            queue_message(client, "ERROR command not permitted for Trader Client");
            return;
        }
        if (tokens.size() != 2) {
            queue_message(client, "ERROR invalid SUBSCRIBE syntax");
            return;
        }
        Instrument instrument;
        if (!parse_instrument(tokens[1], instrument)) {
            queue_message(client, "ERROR invalid instrument");
            return;
        }
        client.subscriptions.insert(instrument_index(instrument));
        queue_message(client, "OK");
    }

    void handle_unsubscribe(Client& client, const std::vector<std::string>& tokens) {
        if (client.role != Role::MARKET_DATA) {
            queue_message(client, "ERROR command not permitted");
            return;
        }
        if (tokens.size() != 2) {
            queue_message(client, "ERROR invalid UNSUBSCRIBE syntax");
            return;
        }
        Instrument instrument;
        if (!parse_instrument(tokens[1], instrument)) {
            queue_message(client, "ERROR invalid instrument");
            return;
        }
        client.subscriptions.erase(instrument_index(instrument));
        queue_message(client, "OK");
    }

    void handle_order(Client& client, const std::vector<std::string>& tokens, Side side) {
        if (client.role != Role::TRADER) {
            queue_message(client, "ERROR command not permitted for Market-Data Client");
            return;
        }
        if (tokens.size() != 4) {
            queue_message(client, "ERROR invalid order syntax");
            return;
        }

        Instrument instrument;
        int32_t quantity = 0;
        int32_t price = 0;
        if (!parse_instrument(tokens[1], instrument)) {
            queue_message(client, "ERROR invalid instrument");
            return;
        }
        if (!parse_int32(tokens[2], quantity, true)) {
            queue_message(client, "ERROR invalid quantity");
            return;
        }
        if (!parse_int32(tokens[3], price, true)) {
            queue_message(client, "ERROR invalid price");
            return;
        }
        if (next_order_id_ > static_cast<uint64_t>(MAX_VALUE)) {
            queue_message(client, "ERROR order ID limit reached");
            return;
        }

        const int32_t order_id = static_cast<int32_t>(next_order_id_++);
        queue_message(client, "ORDER_ACCEPTED " + std::to_string(order_id));

        Order incoming;
        incoming.id = order_id;
        incoming.owner_client_id = client.id;
        incoming.instrument = instrument;
        incoming.side = side;
        incoming.price = price;
        incoming.remaining = quantity;

        match_incoming_order(incoming);

        if (incoming.remaining > 0) {
            add_resting_order(std::move(incoming));
        }
    }

    void match_incoming_order(Order& incoming) {
        const int inst = instrument_index(incoming.instrument);
        const Side opposite_side = incoming.side == Side::BUY ? Side::SELL : Side::BUY;
        auto& opposite_levels = books_[inst][side_index(opposite_side)];
        auto level_it = opposite_levels.find(incoming.price);

        if (level_it == opposite_levels.end()) return;

        OrderList& level = level_it->second;
        while (incoming.remaining > 0 && !level.empty()) {
            auto resting_it = level.begin();
            Order& resting = *resting_it;
            int32_t traded = std::min(incoming.remaining, resting.remaining);

            incoming.remaining -= traded;
            resting.remaining -= traded;

            const Order& buy_order = incoming.side == Side::BUY ? incoming : resting;
            const Order& sell_order = incoming.side == Side::SELL ? incoming : resting;

            queue_to_client_id(buy_order.owner_client_id,
                               "BOUGHT " + instrument_name(incoming.instrument) +
                               " " + std::to_string(traded) +
                               " " + std::to_string(incoming.price));
            queue_to_client_id(sell_order.owner_client_id,
                               "SOLD " + instrument_name(incoming.instrument) +
                               " " + std::to_string(traded) +
                               " " + std::to_string(incoming.price));

            broadcast_trade(incoming.instrument, traded, incoming.price);

            if (resting.remaining == 0) {
                order_index_.erase(resting.id);
                level.erase(resting_it);
            }
        }

        if (level.empty()) {
            opposite_levels.erase(level_it);
        }
    }

    void add_resting_order(Order order) {
        const int inst = instrument_index(order.instrument);
        const int side = side_index(order.side);
        auto& level = books_[inst][side][order.price];
        auto it = level.emplace(level.end(), std::move(order));

        order_index_[it->id] = OrderRef{
            it->instrument,
            it->side,
            it->price,
            it,
        };
    }

    void handle_cancel(Client& client, const std::vector<std::string>& tokens) {
        if (client.role != Role::TRADER) {
            queue_message(client, "ERROR command not permitted for Market-Data Client");
            return;
        }
        if (tokens.size() != 2) {
            queue_message(client, "ERROR invalid CANCEL syntax");
            return;
        }

        int32_t order_id = 0;
        if (!parse_int32(tokens[1], order_id, false)) {
            queue_message(client, "ERROR invalid order ID");
            return;
        }

        auto it = order_index_.find(order_id);
        if (it == order_index_.end()) {
            queue_message(client, "ERROR order not found or already completed");
            return;
        }

        OrderRef ref = it->second;
        Order& order = *ref.iterator;
        if (order.owner_client_id != client.id) {
            queue_message(client, "ERROR order belongs to another trader");
            return;
        }

        auto& level = books_[instrument_index(ref.instrument)][side_index(ref.side)][ref.price];
        level.erase(ref.iterator);
        if (level.empty()) {
            books_[instrument_index(ref.instrument)][side_index(ref.side)].erase(ref.price);
        }
        order_index_.erase(it);
        queue_message(client, "ORDER_CANCELLED " + std::to_string(order_id));
    }

    void broadcast_trade(Instrument instrument, int32_t quantity, int32_t price) {
        const int instrument_id = instrument_index(instrument);
        const std::string message = "TRADE " + instrument_name(instrument) +
                                    " " + std::to_string(quantity) +
                                    " " + std::to_string(price);
        for (auto& [fd, client] : clients_) {
            if (client.role == Role::MARKET_DATA &&
                client.subscriptions.count(instrument_id) != 0) {
                queue_message(client, message);
            }
        }
    }

    void process_message(Client& client, const std::string& line) {
        std::istringstream input(line);
        std::vector<std::string> tokens;
        std::string token;
        while (input >> token) tokens.push_back(token);

        if (tokens.empty()) {
            queue_message(client, "ERROR empty message");
            return;
        }

        const std::string& command = tokens[0];

        if (command == "QUIT") {
            if (tokens.size() != 1) {
                queue_message(client, "ERROR invalid QUIT syntax");
            } else {
                client.closing = true;
            }
            return;
        }

        if (command == "LOGIN") {
            handle_login(client, tokens);
            return;
        }

        if (command == "SUBSCRIBE") {
            handle_subscribe(client, tokens);
            return;
        }

        if (command == "UNSUBSCRIBE") {
            handle_unsubscribe(client, tokens);
            return;
        }

        if (command == "BUY") {
            handle_order(client, tokens, Side::BUY);
            return;
        }

        if (command == "SELL") {
            handle_order(client, tokens, Side::SELL);
            return;
        }

        if (command == "CANCEL") {
            handle_cancel(client, tokens);
            return;
        }

        queue_message(client, "ERROR unknown command");
    }

    bool poll_once() {
        std::vector<pollfd> fds;
        fds.reserve(clients_.size() + 1);
        fds.push_back({listen_fd_, POLLIN, 0});

        for (const auto& [fd, client] : clients_) {
            short events = POLLIN;
            if (!client.output.empty()) events |= POLLOUT;
            fds.push_back({fd, events, 0});
        }

        int ready = poll(fds.data(), fds.size(), -1);
        if (ready == -1) {
            if (errno == EINTR) return true;
            std::cerr << "poll() failed: " << std::strerror(errno) << "\n";
            return false;
        }

        if (fds[0].revents & POLLIN) {
            accept_ready_clients();
        }

        for (size_t i = 1; i < fds.size(); ++i) {
            const int fd = fds[i].fd;
            auto client_it = clients_.find(fd);
            if (client_it == clients_.end()) continue;
            Client& client = client_it->second;

            short events = fds[i].revents;
            if (events & POLLNVAL) {
                client.closing = true;
                continue;
            }
            if (events & (POLLERR | POLLHUP)) {
                // If POLLIN is also set, process the readable bytes first.
                if (!(events & POLLIN)) {
                    client.closing = true;
                    continue;
                }
            }

            if (events & POLLIN) {
                receive_from_client(client);
            }

            if (!client.closing && (events & POLLOUT)) {
                send_pending(client);
            }
        }

        // Try to flush newly queued data on sockets that became ready only
        // because application processing generated output. POLLOUT will be
        // requested on the next poll iteration if anything remains.
        cleanup_marked_clients();
        return true;
    }
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <host> <port>\n";
        return 1;
    }

    ExchangeServer server(argv[1], argv[2]);
    return server.run();
}
