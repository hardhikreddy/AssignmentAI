#include <algorithm>
#include <array>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <list>
#include <map>
#include <memory>
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
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "net_utils.hpp"
#include "event_poller.hpp"

enum class Role { UNKNOWN, TRADER, MARKET_DATA };
enum class Side { BUY, SELL };

enum class Instrument { JNST = 0, IMCT = 1 };

static constexpr int MAX_VALUE = 2147483647;
static constexpr size_t MAX_INPUT_BUFFER = 64 * 1024;
static constexpr size_t MAX_OUTPUT_BUFFER = 8 * 1024 * 1024;
static constexpr int LISTEN_BACKLOG = 1024;

// Fairness caps: the amount of work a single ready socket may do before the
// event loop moves on to other clients. The socket stays level-triggered, so
// any leftover is handled on the next iteration.
static constexpr size_t MAX_RECV_PER_ITERATION = 256 * 1024;
static constexpr size_t MAX_SEND_PER_ITERATION = 1024 * 1024;

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
    // Cached readiness interest currently registered with the Poller, so we
    // only issue an epoll_ctl / kevent syscall when it actually changes.
    bool want_read_registered = false;
    bool want_write_registered = false;
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

// Fixed-capacity token list: the protocol never has more than 4 meaningful
// tokens, so splitting a line needs no heap allocation. Tokens beyond the
// capacity are still counted (so arity checks reject them) but not stored.
struct TokenList {
    static constexpr size_t CAP = 8;
    std::array<std::string_view, CAP> data{};
    size_t count = 0;

    size_t size() const { return count; }
    bool empty() const { return count == 0; }
    std::string_view operator[](size_t i) const { return data[i]; }
    const std::string_view* begin() const { return data.data(); }
    const std::string_view* end() const {
        return data.data() + std::min(count, CAP);
    }
};

class ExchangeServer {
public:
    ExchangeServer(std::string host, std::string port)
        : host_(std::move(host)), port_(std::move(port)) {}

    int run() {
        std::signal(SIGPIPE, SIG_IGN);

        if (!poller_.valid()) {
            std::cerr << "Failed to create event poller\n";
            return 1;
        }

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

        poller_.add(listen_fd_, true, false);

        std::cout << "Exchange Server listening on " << host_ << ':' << port_ << "\n";

        while (true) {
            if (!poll_once()) break;
        }

        for (auto& [fd, client] : clients_) {
            (void)client;
            close(fd);
        }
        clients_.clear();
        client_id_to_client_.clear();
        close(listen_fd_);
        return 0;
    }

private:
    std::string host_;
    std::string port_;
    int listen_fd_ = -1;
    uint64_t next_client_id_ = 1;
    uint64_t next_order_id_ = 0;

    Poller poller_;

    // unique_ptr keeps every Client at a stable address across rehashes, so we
    // can safely hold Client* in the ready-event scan and in the subscriber
    // index without worrying about map growth invalidating them.
    std::unordered_map<int, std::unique_ptr<Client>> clients_;
    std::unordered_map<uint64_t, Client*> client_id_to_client_;
    std::unordered_map<std::string, int> trader_user_to_fd_;
    std::unordered_map<int32_t, OrderRef> order_index_;

    // Subscriber index: per-instrument set of market-data clients. Holding
    // Client* (not fd) removes a hash lookup per subscriber on every trade.
    std::unordered_set<Client*> md_subscribers_[2];

    // Persistent buffers reused across poll loops (no per-loop heap alloc).
    std::vector<PollerEvent> events_;
    std::vector<int> doomed_;
    std::string scratch_;   // reused for formatting outbound messages

    // [instrument][side][price] -> FIFO list of orders at that price.
    std::map<int32_t, OrderList> books_[2][2];

    static int instrument_index(Instrument instrument) {
        return static_cast<int>(instrument);
    }

    static int side_index(Side side) {
        return static_cast<int>(side);
    }

    static void append_int(std::string& out, long long value) {
        char buf[24];
        auto result = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, result.ptr);
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

    // Zero-allocation tokeniser: splits 'line' on spaces into string_views.
    static TokenList tokenize(std::string_view line) {
        TokenList tokens;
        size_t start = 0;
        while (start < line.size()) {
            while (start < line.size() && line[start] == ' ') ++start;
            if (start >= line.size()) break;
            size_t end = start;
            while (end < line.size() && line[end] != ' ') ++end;
            if (tokens.count < TokenList::CAP) {
                tokens.data[tokens.count] = line.substr(start, end - start);
            }
            ++tokens.count;
            start = end;
        }
        return tokens;
    }

    Client* find_client(int fd) {
        auto it = clients_.find(fd);
        return it == clients_.end() ? nullptr : it->second.get();
    }

    Client* find_client_by_id(uint64_t id) {
        auto it = client_id_to_client_.find(id);
        return it == client_id_to_client_.end() ? nullptr : it->second;
    }

    // Register/refresh this client's readiness interest with the Poller, but
    // only call into the kernel when the desired mask actually changed.
    void update_interest(Client& client) {
        const bool want_read = !client.input_closed && !client.closing;
        const bool want_write =
            client.output_offset < client.output_buf.size() && !client.closing;
        if (want_read == client.want_read_registered &&
            want_write == client.want_write_registered) {
            return;
        }
        client.want_read_registered = want_read;
        client.want_write_registered = want_write;
        poller_.mod(client.fd, want_read, want_write);
    }

    bool queue_message(Client& client, std::string_view message) {
        if (client.closing) return false;

        const size_t total = message.size() + (message.empty() || message.back() != '\n' ? 1 : 0);
        if (client.output_bytes + total > MAX_OUTPUT_BUFFER) {
            std::cerr << "Closing fd " << client.fd
                      << " because its output buffer exceeded "
                      << MAX_OUTPUT_BUFFER << " bytes\n";
            client.closing = true;
            return false;
        }

        const bool was_idle = client.output_offset == client.output_buf.size();
        client.output_buf.append(message);
        if (message.empty() || message.back() != '\n') client.output_buf.push_back('\n');
        client.output_bytes += total;

        if (was_idle) update_interest(client);
        return true;
    }

    bool queue_to_client_id(uint64_t client_id, std::string_view message) {
        Client* client = find_client_by_id(client_id);
        if (client == nullptr || client->closing) return false;
        return queue_message(*client, message);
    }

    void close_client(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;

        Client& client = *it->second;
        if (client.role == Role::TRADER && !client.username.empty()) {
            auto user_it = trader_user_to_fd_.find(client.username);
            if (user_it != trader_user_to_fd_.end() && user_it->second == fd) {
                trader_user_to_fd_.erase(user_it);
            }
        }

        if (client.role == Role::MARKET_DATA) {
            md_subscribers_[0].erase(&client);
            md_subscribers_[1].erase(&client);
        }

        poller_.del(fd);
        client_id_to_client_.erase(client.id);
        close(fd);
        clients_.erase(it);
    }

    void cleanup_marked_clients() {
        doomed_.clear();
        for (const auto& [fd, client] : clients_) {
            if (client->closing ||
                (client->input_closed &&
                 client->output_buf.size() == client->output_offset)) {
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

            set_tcp_nodelay(client_fd);
            set_socket_buffers(client_fd);

            auto client = std::make_unique<Client>();
            client->fd = client_fd;
            client->id = next_client_id_++;
            client->want_read_registered = true;
            Client* raw = client.get();
            clients_.emplace(client_fd, std::move(client));
            client_id_to_client_[raw->id] = raw;
            poller_.add(client_fd, true, false);
        }
    }

    void compact_input_buffer(Client& client) {
        if (client.input_offset == 0) return;
        if (client.input_offset < client.input.size() / 2 &&
            client.input.size() < MAX_INPUT_BUFFER / 2) return;
        client.input.erase(0, client.input_offset);
        client.input_offset = 0;
    }

    void receive_from_client(Client& client) {
        char buffer[16384];
        size_t consumed_this_iteration = 0;

        while (!client.closing && !client.input_closed) {
            if (consumed_this_iteration >= MAX_RECV_PER_ITERATION) break;

            const ssize_t n = recv(client.fd, buffer, sizeof(buffer), 0);

            if (n > 0) {
                consumed_this_iteration += static_cast<size_t>(n);
                client.input.append(buffer, static_cast<size_t>(n));
                if (client.input.size() > MAX_INPUT_BUFFER) {
                    client.input.clear();
                    client.input_offset = 0;
                    queue_message(client, "ERROR message too long");
                    client.input_closed = true;
                    break;
                }

                while (!client.closing) {
                    const size_t newline = client.input.find('\n', client.input_offset);
                    if (newline == std::string::npos) break;

                    std::string_view line(client.input.data() + client.input_offset,
                                         newline - client.input_offset);
                    client.input_offset = newline + 1;

                    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                    process_message(client, line);
                }

                compact_input_buffer(client);
                continue;
            }

            if (n == 0) {
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
        size_t sent_this_iteration = 0;

        while (client.output_offset < client.output_buf.size() && !client.closing) {
            if (sent_this_iteration >= MAX_SEND_PER_ITERATION) break;

            const char* data = client.output_buf.data() + client.output_offset;
            const size_t remaining = client.output_buf.size() - client.output_offset;

            const ssize_t n = send(client.fd, data, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                sent_this_iteration += static_cast<size_t>(n);
                client.output_offset += static_cast<size_t>(n);
                client.output_bytes -= static_cast<size_t>(n);
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

    void handle_login(Client& client, const TokenList& tokens) {
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

    void handle_subscribe(Client& client, const TokenList& tokens) {
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
        md_subscribers_[idx].insert(&client);
        queue_message(client, "OK");
    }

    void handle_unsubscribe(Client& client, const TokenList& tokens) {
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
        md_subscribers_[idx].erase(&client);
        queue_message(client, "OK");
    }

    void handle_order(Client& client, const TokenList& tokens, Side side) {
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

        scratch_.clear();
        scratch_ += "ORDER_ACCEPTED ";
        append_int(scratch_, order_id);
        queue_message(client, scratch_);

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

    void emit_fill(uint64_t client_id, const char* verb, const char* inst,
                   int32_t qty, int32_t price) {
        scratch_.clear();
        scratch_ += verb;
        scratch_ += ' ';
        scratch_ += inst;
        scratch_ += ' ';
        append_int(scratch_, qty);
        scratch_ += ' ';
        append_int(scratch_, price);
        queue_to_client_id(client_id, scratch_);
    }

    void match_incoming_order(Order& incoming) {
        const int inst = instrument_index(incoming.instrument);
        const Side opposite_side = incoming.side == Side::BUY ? Side::SELL : Side::BUY;
        auto& opposite_levels = books_[inst][side_index(opposite_side)];

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

            emit_fill(buy_order.owner_client_id, "BOUGHT", inst_name, traded, incoming.price);
            emit_fill(sell_order.owner_client_id, "SOLD", inst_name, traded, incoming.price);

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

    void handle_cancel(Client& client, const TokenList& tokens) {
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

        scratch_.clear();
        scratch_ += "ORDER_CANCELLED ";
        append_int(scratch_, order_id);
        queue_message(client, scratch_);
    }

    void broadcast_trade(int instrument_id, const char* inst_name,
                         int32_t quantity, int32_t price) {
        auto& subscribers = md_subscribers_[instrument_id];
        if (subscribers.empty()) return;

        scratch_.clear();
        scratch_ += "TRADE ";
        scratch_ += inst_name;
        scratch_ += ' ';
        append_int(scratch_, quantity);
        scratch_ += ' ';
        append_int(scratch_, price);

        for (Client* client : subscribers) {
            if (!client->closing) queue_message(*client, scratch_);
        }
    }

    void process_message(Client& client, std::string_view line) {
        const TokenList tokens = tokenize(line);

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

        if (command == "LOGIN")       { handle_login(client, tokens); return; }
        if (command == "SUBSCRIBE")   { handle_subscribe(client, tokens); return; }
        if (command == "UNSUBSCRIBE") { handle_unsubscribe(client, tokens); return; }
        if (command == "BUY")         { handle_order(client, tokens, Side::BUY); return; }
        if (command == "SELL")        { handle_order(client, tokens, Side::SELL); return; }
        if (command == "CANCEL")      { handle_cancel(client, tokens); return; }

        queue_message(client, "ERROR unknown command");
    }

    bool poll_once() {
        const int ready = poller_.wait(events_);
        if (ready == -1) {
            if (errno == EINTR) return true;
            std::cerr << "poll wait failed: " << std::strerror(errno) << '\n';
            return false;
        }

        bool listen_ready = false;
        for (const PollerEvent& ev : events_) {
            if (ev.fd == listen_fd_) { listen_ready = true; break; }
        }
        if (listen_ready) accept_ready_clients();

        for (const PollerEvent& ev : events_) {
            if (ev.fd == listen_fd_) continue;

            Client* client = find_client(ev.fd);
            if (client == nullptr) continue;

            if (ev.error && !ev.readable) {
                client->closing = true;
                continue;
            }

            if (ev.readable || ev.hangup) {
                receive_from_client(*client);
            }

            if (!client->closing && client->output_offset < client->output_buf.size()) {
                send_pending(*client);
            }

            if (client->input_closed && client->output_offset == client->output_buf.size()) {
                client->closing = true;
            }

            if (!client->closing) update_interest(*client);
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
