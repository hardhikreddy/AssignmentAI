#pragma once

// Readiness-notification abstraction for the Exchange Server event loop.
//
// Three interchangeable back ends, selected at compile time:
//
//   (default)      poll(2)          - portable, O(n) scan in the kernel
//   -DUSE_EPOLL    epoll (Linux)    - O(ready), for local benchmarking
//   -DUSE_KQUEUE   kqueue (FreeBSD) - O(ready), submission-target fast path
//
// All back ends are used in level-triggered mode so the server may stop
// draining a socket early (fairness caps) without losing a wakeup.
//
// Interface:
//   poller.add(fd, want_read, want_write)
//   poller.mod(fd, want_read, want_write)   // idempotent
//   poller.del(fd)
//   int n = poller.wait(events);            // blocks; fills `events`
//
// Each Event reports fd and the readable/writable/error/hangup flags.

#include <cstdint>
#include <vector>

#if defined(USE_EPOLL)
#include <sys/epoll.h>
#include <unistd.h>
#elif defined(USE_KQUEUE)
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctime>
#include <unordered_map>
#else
#include <poll.h>
#include <unordered_map>
#endif

#include <cerrno>

struct PollerEvent {
    int fd = -1;
    bool readable = false;
    bool writable = false;
    bool error = false;
    bool hangup = false;
};

// ---------------------------------------------------------------------------
#if defined(USE_EPOLL)
// ---------------------------------------------------------------------------

class Poller {
public:
    Poller() : epfd_(::epoll_create1(0)) {}
    ~Poller() { if (epfd_ != -1) ::close(epfd_); }

    static const char* backend_name() { return "epoll"; }
    bool valid() const { return epfd_ != -1; }

    void add(int fd, bool want_read, bool want_write) {
        epoll_event ev{};
        ev.events = mask(want_read, want_write);
        ev.data.fd = fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    }

    void mod(int fd, bool want_read, bool want_write) {
        epoll_event ev{};
        ev.events = mask(want_read, want_write);
        ev.data.fd = fd;
        ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }

    void del(int fd) {
        ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    int wait(std::vector<PollerEvent>& out) {
        if (raw_.size() < 64) raw_.resize(1024);
        int n = ::epoll_wait(epfd_, raw_.data(),
                             static_cast<int>(raw_.size()), -1);
        if (n < 0) return n;
        out.clear();
        for (int i = 0; i < n; ++i) {
            const auto& e = raw_[i];
            PollerEvent pe;
            pe.fd = e.data.fd;
            pe.readable = (e.events & (EPOLLIN | EPOLLRDHUP)) != 0;
            pe.writable = (e.events & EPOLLOUT) != 0;
            pe.error    = (e.events & EPOLLERR) != 0;
            pe.hangup   = (e.events & (EPOLLHUP | EPOLLRDHUP)) != 0;
            out.push_back(pe);
        }
        // Grow the scratch buffer if we filled it completely.
        if (n == static_cast<int>(raw_.size())) raw_.resize(raw_.size() * 2);
        return n;
    }

private:
    static uint32_t mask(bool r, bool w) {
        uint32_t m = EPOLLRDHUP;
        if (r) m |= EPOLLIN;
        if (w) m |= EPOLLOUT;
        return m;
    }

    int epfd_;
    std::vector<epoll_event> raw_;
};

// ---------------------------------------------------------------------------
#elif defined(USE_KQUEUE)
// ---------------------------------------------------------------------------

class Poller {
public:
    Poller() : kq_(::kqueue()) {}
    ~Poller() { if (kq_ != -1) ::close(kq_); }

    static const char* backend_name() { return "kqueue"; }
    bool valid() const { return kq_ != -1; }

    void add(int fd, bool want_read, bool want_write) {
        state_[fd] = 0;
        apply(fd, want_read, want_write);
    }

    void mod(int fd, bool want_read, bool want_write) {
        apply(fd, want_read, want_write);
    }

    void del(int fd) {
        // A closed descriptor is removed from the kqueue automatically, so we
        // just forget our bookkeeping. If the caller deletes before closing,
        // emit explicit EV_DELETEs for whatever is currently active.
        auto it = state_.find(fd);
        if (it == state_.end()) return;
        if (it->second & R) set_filter(fd, EVFILT_READ, EV_DELETE);
        if (it->second & W) set_filter(fd, EVFILT_WRITE, EV_DELETE);
        state_.erase(it);
    }

    int wait(std::vector<PollerEvent>& out) {
        flush_changes();
        if (raw_.size() < 64) raw_.resize(1024);

        int n = ::kevent(kq_, nullptr, 0, raw_.data(),
                         static_cast<int>(raw_.size()), nullptr);
        if (n < 0) return n;

        out.clear();
        for (int i = 0; i < n; ++i) {
            const struct kevent& e = raw_[i];
            const int fd = static_cast<int>(e.ident);

            PollerEvent* slot = nullptr;
            for (auto& existing : out) {
                if (existing.fd == fd) { slot = &existing; break; }
            }
            if (slot == nullptr) {
                out.push_back(PollerEvent{});
                slot = &out.back();
                slot->fd = fd;
            }

            if (e.filter == EVFILT_READ)  slot->readable = true;
            if (e.filter == EVFILT_WRITE) slot->writable = true;
            if (e.flags & EV_EOF)   slot->hangup = true;
            if (e.flags & EV_ERROR) slot->error = true;
        }

        if (n == static_cast<int>(raw_.size())) raw_.resize(raw_.size() * 2);
        return n;
    }

private:
    enum : uint8_t { R = 1, W = 2 };

    // Only emit a change when a filter actually turns on or off, tracked per
    // fd in state_. This avoids redundant kevent traffic and, more importantly,
    // avoids EV_DELETE on a filter that was never registered (which would fail
    // the whole change batch).
    void apply(int fd, bool want_read, bool want_write) {
        uint8_t& cur = state_[fd];
        const bool have_r = cur & R;
        const bool have_w = cur & W;
        if (want_read != have_r)
            set_filter(fd, EVFILT_READ, want_read ? EV_ADD : EV_DELETE);
        if (want_write != have_w)
            set_filter(fd, EVFILT_WRITE, want_write ? EV_ADD : EV_DELETE);
        cur = (want_read ? R : 0) | (want_write ? W : 0);
    }

    void set_filter(int fd, int16_t filter, uint16_t flags) {
        struct kevent kev;
        EV_SET(&kev, fd, filter, flags | EV_RECEIPT, 0, 0, nullptr);
        changes_.push_back(kev);
    }

    void flush_changes() {
        if (changes_.empty()) return;
        // EV_RECEIPT forces one status result per change into the receipt
        // buffer, so a single bad change cannot abort the rest of the batch.
        receipts_.resize(changes_.size());
        ::kevent(kq_, changes_.data(), static_cast<int>(changes_.size()),
                 receipts_.data(), static_cast<int>(receipts_.size()), nullptr);
        changes_.clear();
    }

    int kq_;
    std::unordered_map<int, uint8_t> state_;
    std::vector<struct kevent> changes_;
    std::vector<struct kevent> receipts_;
    std::vector<struct kevent> raw_;
};

// ---------------------------------------------------------------------------
#else   // default: poll(2)
// ---------------------------------------------------------------------------

class Poller {
public:
    static const char* backend_name() { return "poll"; }
    bool valid() const { return true; }

    void add(int fd, bool want_read, bool want_write) {
        if (index_.count(fd)) { mod(fd, want_read, want_write); return; }
        pollfd p{};
        p.fd = fd;
        p.events = mask(want_read, want_write);
        index_[fd] = pfds_.size();
        pfds_.push_back(p);
    }

    void mod(int fd, bool want_read, bool want_write) {
        auto it = index_.find(fd);
        if (it == index_.end()) return;
        pfds_[it->second].events = mask(want_read, want_write);
    }

    void del(int fd) {
        auto it = index_.find(fd);
        if (it == index_.end()) return;
        const size_t slot = it->second;
        const size_t last = pfds_.size() - 1;
        if (slot != last) {
            pfds_[slot] = pfds_[last];
            index_[pfds_[slot].fd] = slot;
        }
        pfds_.pop_back();
        index_.erase(it);
    }

    int wait(std::vector<PollerEvent>& out) {
        const int n = ::poll(pfds_.data(),
                             static_cast<nfds_t>(pfds_.size()), -1);
        if (n < 0) return n;
        out.clear();
        for (const auto& p : pfds_) {
            if (p.revents == 0) continue;
            PollerEvent pe;
            pe.fd = p.fd;
            pe.readable = (p.revents & (POLLIN | POLLHUP)) != 0;
            pe.writable = (p.revents & POLLOUT) != 0;
            pe.error    = (p.revents & (POLLERR | POLLNVAL)) != 0;
            pe.hangup   = (p.revents & POLLHUP) != 0;
            out.push_back(pe);
        }
        return n;
    }

private:
    static short mask(bool r, bool w) {
        short m = 0;
        if (r) m |= POLLIN;
        if (w) m |= POLLOUT;
        return m;
    }

    std::vector<pollfd> pfds_;
    std::unordered_map<int, size_t> index_;
};

#endif
