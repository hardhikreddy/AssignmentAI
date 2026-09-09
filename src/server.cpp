#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "net_utils.hpp"

enum class Role { UNKNOWN, TRADER, MARKET_DATA };
enum class Side { BUY, SELL };

enum class Instrument { JNST = 0, IMCT = 1 };

static constexpr int MAX_VALUE = 2147483647;
static constexpr size_t MAX_INPUT_BUFFER = 64 * 1024;
static constexpr size_t MAX_OUTPUT_BUFFER = 8 * 1024 * 1024;
static constexpr int LISTEN_BACKLOG = 512;

// Raise our open-file limit to the hard max so we can hold many connections
// (needed for the 70k-connection scalability test). Harmless otherwise.
static void raise_fd_limit() {
#ifndef _WIN32
    rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur < lim.rlim_max) {
        lim.rlim_cur = lim.rlim_max;
        setrlimit(RLIMIT_NOFILE, &lim);
    }
#endif
}

struct Client {
    int fd = -1;
    uint64_t id = 0;
    Role role = Role::UNKNOWN;
    std::string username;
    std::unordered_set<int> subscriptions;
    std::string input;
    size_t input_offset = 0;   // cursor: avoids O(n) erase on every newline
    // Single contiguous output buffer + write cursor eliminates one heap
    // allocation per queued message (vs deque<string>).
    std::string output_buf;
    size_t output_offset = 0;
    size_t output_bytes = 0;   // bytes currently buffered (for limit check)
    bool input_closed = false;
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
        raise_fd_limit();

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
    bool fds_dirty_ = true;          // true whenever client set changes

    std::unordered_map<int, Client> clients_;
    std::unordered_map<uint64_t, int> client_id_to_fd_;
    std::unordered_map<std::string, int> trader_user_to_fd_;
    std::unordered_map<int32_t, OrderRef> order_index_;

    // Subscriber index: per-instrument set of fds for market-data clients.
    // Avoids iterating all clients on every broadcast_trade call.
    std::unordered_set<int> md_subscribers_[2];

    // Persistent vectors reused across poll loops (avoids per-loop heap allocs).
    std::vector<pollfd> fds_;
    std::vector<int> doomed_;

    // [instrument][side][price] -> FIFO list of orders at that price.
    std::map<int32_t, OrderList> books_[2][2];

    static int instrument_index(Instrument instrument) {
        return static_cast<int>(instrument);
    }

    static int side_index(Side side) {
        return static_cast<int>(side);
    }

    static bool parse_int32(std::string_view text, int32_t& value, bool positive_only) {
        if (text.empty()) return false;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
        }

        long long parsed = 0;
        auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
        if (positive_only) {
            if (parsed < 1 || parsed > MAX_VALUE) return false;
        } else {
            if (parsed < 0 || parsed > MAX_VALUE) return false;
        }
        value = static_cast<int32_t>(parsed);
        return true;
    }

    static bool parse_instrument(std::string_view text, Instrument& instrument) {
        if (text == "JNST") { instrument = Instrument::JNST; return true; }
        if (text == "IMCT") { instrument = Instrument::IMCT; return true; }
        return false;
    }

    static const char* instrument_name(Instrument instrument) {
        return instrument == Instrument::JNST ? "JNST" : "IMCT";
    }

    static const char* side_name(Side side) {
        return side == Side::BUY ? "BUY" : "SELL";
    }

    // Zero-allocation tokeniser: splits 'line' on spaces into string_views.
    static std::vector<std::string_view> tokenize(std::string_view line) {
        std::vector<std::string_view> tokens;
        tokens.reserve(4);
        size_t start = 0;
        while (start < line.size()) {
            // skip spaces
            while (start < line.size() && line[start] == ' ') ++start;
            if (start >= line.size()) break;
            size_t end = start;
            while (end < line.size() && line[end] != ' ') ++end;
            tokens.push_back(line.substr(start, end - start));
            start = end;
        }
        return tokens;
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

    // Accept by value so callers can pass temporaries without an extra copy.
    bool queue_message(Client& client, std::string message) {
        if (client.closing) return false;
        if (message.empty() || message.back() != '\n') message.push_back('\n');

        if (client.output_bytes + message.size() > MAX_OUTPUT_BUFFER) {
            std::cerr << "Closing fd " << client.fd
                      << " because its output buffer exceeded "
                      << MAX_OUTPUT_BUFFER << " bytes\n";
            client.closing = true;
            return false;
        }

        client.output_bytes += message.size();
        // Append directly into the flat buffer — no heap alloc per message.
        client.output_buf += message;
        return true;
    }

    bool queue_to_client_id(uint64_t client_id, std::string message) {
        Client* client = find_client_by_id(client_id);
        if (client == nullptr || client->closing) return false;
        return queue_message(*client, std::move(message));
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

        // Remove from subscriber index if this was a market-data client.
        if (client.role == Role::MARKET_DATA) {
            for (int inst_idx : {0, 1}) {
                md_subscribers_[inst_idx].erase(fd);
            }
        }

        client_id_to_fd_.erase(client.id);
        close(fd);
        clients_.erase(it);
        fds_dirty_ = true;
    }

    void cleanup_marked_clients() {
        doomed_.clear();
        for (const auto& [fd, client] : clients_) {
            if (client.closing || (client.input_closed && client.output_buf.size() == client.output_offset)) {
                doomed_.push_back(fd);
            }
        }
        for (int fd : doomed_) close_client(fd);
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

            // Disable Nagle's algorithm for lower latency on individual messages.
            set_tcp_nodelay(client_fd);

            Client client;
            client.fd = client_fd;
            client.id = next_client_id_++;
            clients_.emplace(client_fd, std::move(client));
            client_id_to_fd_[clients_.at(client_fd).id] = client_fd;
            fds_dirty_ = true;
        }
    }

    void compact_input_buffer(Client& client) {
        // Only compact when we've consumed at least half the buffer
        // to amortise the O(n) copy.
        if (client.input_offset == 0) return;
        if (client.input_offset < client.input.size() / 2 &&
            client.input.size() < MAX_INPUT_BUFFER / 2) return;
        client.input.erase(0, client.input_offset);
        client.input_offset = 0;
    }

    void receive_from_client(Client& client) {
        char buffer[8192];

        while (!client.closing && !client.input_closed) {
            const ssize_t n = recv(client.fd, buffer, sizeof(buffer), 0);

            if (n > 0) {
                client.input.append(buffer, static_cast<size_t>(n));
                if (client.input.size() > MAX_INPUT_BUFFER) {
                    client.input.clear();
                    client.input_offset = 0;
                    queue_message(client, "ERROR message too long");
                    client.input_closed = true;
                    break;
                }

                while (!client.closing) {
                    // Search from the current cursor position only.
                    const size_t newline = client.input.find('\n', client.input_offset);
                    if (newline == std::string::npos) break;

                    // Build a string_view of the line without any allocation.
                    std::string_view line(client.input.data() + client.input_offset,
                                         newline - client.input_offset);
                    client.input_offset = newline + 1;

                    // Strip trailing CR.
                    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                    process_message(client, line);
                }

                // Periodically compact to reclaim memory.
                compact_input_buffer(client);
                continue;
            }

            if (n == 0) {
                // TCP FIN: stop reading, but keep the socket alive long enough
                // to flush any protocol response already queued.
                compact_input_buffer(client);
                client.input_closed = true;
                break;
            }

            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            client.closing = true;
            break;
        }
    }

    void send_pending(Client& client) {
        // Drain from the flat output_buf starting at output_offset.
        while (client.output_offset < client.output_buf.size() && !client.closing) {
            const char* data = client.output_buf.data() + client.output_offset;
            const size_t remaining = client.output_buf.size() - client.output_offset;

            const ssize_t n = send(client.fd, data, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                client.output_offset += static_cast<size_t>(n);
                client.output_bytes -= static_cast<size_t>(n);
                // Compact the buffer once fully drained to reclaim memory.
                if (client.output_offset == client.output_buf.size()) {
                    client.output_buf.clear();
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

    void handle_login(Client& client, const std::vector<std::string_view>& tokens) {
        if (client.role != Role::UNKNOWN) {
            queue_message(client, "ERROR client role already selected");
            return;
        }
        if (tokens.size() != 2 || tokens[1].empty()) {
            queue_message(client, "ERROR invalid LOGIN syntax");
            return;
        }
        const std::string_view username = tokens[1];
        for (char c : username) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                queue_message(client, "ERROR invalid username");
                return;
            }
        }
        std::string uname(username);
        if (trader_user_to_fd_.count(uname) != 0) {
            queue_message(client, "ERROR username already in use");
            return;
        }

        client.role = Role::TRADER;
        client.username = std::move(uname);
        trader_user_to_fd_[client.username] = client.fd;
        queue_message(client, "OK");
    }

    void handle_subscribe(Client& client, const std::vector<std::string_view>& tokens) {
        if (client.role == Role::TRADER) {
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

        if (client.role == Role::UNKNOWN) {
            client.role = Role::MARKET_DATA;
        }

        const int idx = instrument_index(instrument);
        client.subscriptions.insert(idx);
        md_subscribers_[idx].insert(client.fd);
        queue_message(client, "OK");
    }

    void handle_unsubscribe(Client& client, const std::vector<std::string_view>& tokens) {
        if (client.role == Role::TRADER) {
            queue_message(client, "ERROR command not permitted for Trader Client");
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

        if (client.role == Role::UNKNOWN) {
            client.role = Role::MARKET_DATA;
        }

        const int idx = instrument_index(instrument);
        client.subscriptions.erase(idx);
        md_subscribers_[idx].erase(client.fd);
        queue_message(client, "OK");
    }

    void handle_order(Client& client, const std::vector<std::string_view>& tokens, Side side) {
        if (client.role == Role::UNKNOWN) {
            queue_message(client, "ERROR must login first");
            return;
        }
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

    // Build "BOUGHT <inst> <qty> <price>" or "SOLD <inst> <qty> <price>" efficiently.
    static std::string make_fill_msg(const char* verb, const char* inst,
                                     int32_t qty, int32_t price) {
        return std::string(verb) + " " + inst + " " + std::to_string(qty) + " " + std::to_string(price);
    }

    void match_incoming_order(Order& incoming) {
        const int inst = instrument_index(incoming.instrument);
        const Side opposite_side = incoming.side == Side::BUY ? Side::SELL : Side::BUY;
        auto& opposite_levels = books_[inst][side_index(opposite_side)];

        // Same-price-only matching per assignment spec:
        // A BUY and a SELL match only if they are at exactly the same price.
        auto level_it = opposite_levels.find(incoming.price);
        if (level_it == opposite_levels.end()) return;

        const char* inst_name = instrument_name(incoming.instrument);
        OrderList& level = level_it->second;

        while (incoming.remaining > 0 && !level.empty()) {
            auto resting_it = level.begin();
            Order& resting = *resting_it;
            const int32_t traded = std::min(incoming.remaining, resting.remaining);

            incoming.remaining -= traded;
            resting.remaining -= traded;

            const Order& buy_order  = incoming.side == Side::BUY  ? incoming : resting;
            const Order& sell_order = incoming.side == Side::SELL ? incoming : resting;

            // Use pre-built strings with reserve; avoid repeated + concatenations.
            queue_to_client_id(buy_order.owner_client_id,
                               make_fill_msg("BOUGHT", inst_name, traded, incoming.price));
            queue_to_client_id(sell_order.owner_client_id,
                               make_fill_msg("SOLD", inst_name, traded, incoming.price));

            broadcast_trade(inst, inst_name, traded, incoming.price);

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

    void handle_cancel(Client& client, const std::vector<std::string_view>& tokens) {
        if (client.role == Role::UNKNOWN) {
            queue_message(client, "ERROR must login first");
            return;
        }
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

    // broadcast_trade only visits the subscriber index for this instrument —
    // O(subscribers) instead of O(all_clients).
    void broadcast_trade(int instrument_id, const char* inst_name,
                         int32_t quantity, int32_t price) {
        if (md_subscribers_[instrument_id].empty()) return;

        std::string message = "TRADE " + std::string(inst_name) + " " + std::to_string(quantity) + " " + std::to_string(price);

        for (int fd : md_subscribers_[instrument_id]) {
            Client* client = find_client(fd);
            if (client != nullptr && !client->closing) {
                queue_message(*client, message);  // copies the pre-built string
            }
        }
    }

    void process_message(Client& client, std::string_view line) {
        // Zero-allocation tokeniser using string_view.
        const auto tokens = tokenize(line);

        if (tokens.empty()) {
            queue_message(client, "ERROR empty message");
            return;
        }

        const std::string_view command = tokens[0];

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
        // Rebuild fds_ every call but reuse the vector's capacity to avoid
        // heap allocation. unordered_map iteration order is not stable so we
        // cannot safely patch individual entries in-place.
        fds_.clear();
        fds_.reserve(clients_.size() + 1);
        fds_.push_back({listen_fd_, POLLIN, 0});
        for (const auto& [fd, client] : clients_) {
            short events = 0;
            if (!client.input_closed) events |= POLLIN;
            if (client.output_offset < client.output_buf.size()) events |= POLLOUT;
            fds_.push_back({fd, events, 0});
        }
        (void)fds_dirty_; // kept as a field in case a future ordered structure is used

        const int ready = poll(fds_.data(), static_cast<nfds_t>(fds_.size()), -1);
        if (ready == -1) {
            if (errno == EINTR) return true;
            std::cerr << "poll() failed: " << std::strerror(errno) << '\n';
            return false;
        }

        if (fds_[0].revents & POLLIN) {
            accept_ready_clients();
        }

        for (size_t i = 1; i < fds_.size(); ++i) {
            const int fd = fds_[i].fd;
            Client* client = find_client(fd);
            if (client == nullptr) continue;

            const short events = fds_[i].revents;

            if (events & POLLNVAL) {
                client->closing = true;
                continue;
            }

            if ((events & POLLERR) && !(events & POLLIN)) {
                client->closing = true;
                continue;
            }

            if (events & (POLLIN | POLLHUP)) {
                receive_from_client(*client);
            }

            // This also flushes replies created while handling POLLIN above.
            if (!client->closing && client->output_offset < client->output_buf.size()) {
                send_pending(*client);
            }

            if (client->input_closed && client->output_offset == client->output_buf.size()) {
                client->closing = true;
            }
        }

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
