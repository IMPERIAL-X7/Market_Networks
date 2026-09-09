#include "event_loop.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>

#if EXCHANGE_HAVE_KQUEUE
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#endif

namespace net {

EventLoop::Backend EventLoop::default_backend() {
#if EXCHANGE_HAVE_KQUEUE
    return Backend::Kqueue;
#else
    return Backend::Poll;
#endif
}

EventLoop::Backend EventLoop::parse_backend(const std::string& name, bool* ok) {
    if (ok) *ok = true;
    if (name == "poll") return Backend::Poll;
    if (name == "kqueue") {
#if EXCHANGE_HAVE_KQUEUE
        return Backend::Kqueue;
#else
        if (ok) *ok = false;
        return Backend::Poll;
#endif
    }
    if (ok) *ok = false;
    return default_backend();
}

const char* EventLoop::backend_name(Backend backend) {
    return backend == Backend::Kqueue ? "kqueue" : "poll";
}

EventLoop::EventLoop(Backend backend) : backend_(backend) {
#if EXCHANGE_HAVE_KQUEUE
    if (backend_ == Backend::Kqueue) {
        kq_ = kqueue();
        if (kq_ < 0) backend_ = Backend::Poll;  // fall back rather than abort
    }
#else
    backend_ = Backend::Poll;
#endif
}

EventLoop::~EventLoop() {
    if (kq_ >= 0) close(kq_);
}

std::size_t EventLoop::size() const {
    return backend_ == Backend::Poll ? pollfds_.size() : kq_interest_.size();
}

bool EventLoop::add(int fd, bool want_read, bool want_write) {
    return backend_ == Backend::Poll ? poll_add(fd, want_read, want_write)
                                     : kq_apply(fd, want_read, want_write);
}

bool EventLoop::update(int fd, bool want_read, bool want_write) {
    return backend_ == Backend::Poll ? poll_update(fd, want_read, want_write)
                                     : kq_apply(fd, want_read, want_write);
}

void EventLoop::remove(int fd) {
    if (backend_ == Backend::Poll) {
        poll_remove(fd);
    } else {
        kq_remove(fd);
    }
}

int EventLoop::wait(std::vector<Event>& out, int timeout_ms) {
    return backend_ == Backend::Poll ? poll_wait(out, timeout_ms)
                                     : kq_wait(out, timeout_ms);
}

// ---------------------------------------------------------------------------
// poll(2) backend
// ---------------------------------------------------------------------------

static short poll_mask(bool r, bool w) {
    short mask = 0;
    if (r) mask |= POLLIN;
    if (w) mask |= POLLOUT;
    return mask;
}

bool EventLoop::poll_add(int fd, bool r, bool w) {
    if (poll_index_.count(fd)) return poll_update(fd, r, w);
    struct pollfd entry;
    entry.fd = fd;
    entry.events = poll_mask(r, w);
    entry.revents = 0;
    poll_index_[fd] = pollfds_.size();
    pollfds_.push_back(entry);
    return true;
}

bool EventLoop::poll_update(int fd, bool r, bool w) {
    auto it = poll_index_.find(fd);
    if (it == poll_index_.end()) return poll_add(fd, r, w);
    pollfds_[it->second].events = poll_mask(r, w);
    return true;
}

void EventLoop::poll_remove(int fd) {
    auto it = poll_index_.find(fd);
    if (it == poll_index_.end()) return;

    // Swap-with-last removal keeps this O(1); the moved entry's index is fixed
    // up so that poll_index_ stays consistent.
    std::size_t idx = it->second;
    std::size_t last = pollfds_.size() - 1;
    if (idx != last) {
        pollfds_[idx] = pollfds_[last];
        poll_index_[pollfds_[idx].fd] = idx;
    }
    pollfds_.pop_back();
    poll_index_.erase(it);
}

int EventLoop::poll_wait(std::vector<Event>& out, int timeout_ms) {
    out.clear();
    if (pollfds_.empty()) return 0;

    int ready = ::poll(pollfds_.data(), static_cast<nfds_t>(pollfds_.size()),
                       timeout_ms);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready == 0) return 0;

    // Snapshot the ready descriptors before returning: the caller may close
    // descriptors while iterating, which would invalidate pollfds_ indices.
    for (const struct pollfd& entry : pollfds_) {
        if (entry.revents == 0) continue;
        Event ev;
        ev.fd = entry.fd;
        ev.readable = (entry.revents & (POLLIN | POLLHUP)) != 0;
        ev.writable = (entry.revents & POLLOUT) != 0;
        ev.hangup = (entry.revents & (POLLERR | POLLNVAL)) != 0;
        out.push_back(ev);
    }
    return static_cast<int>(out.size());
}

// ---------------------------------------------------------------------------
// kqueue(2) backend
// ---------------------------------------------------------------------------

#if EXCHANGE_HAVE_KQUEUE

bool EventLoop::kq_apply(int fd, bool r, bool w) {
    struct kevent changes[2];
    int n = 0;

    // EV_ADD on an existing filter is idempotent, so enabling and disabling
    // filters is expressed as add/delete of the two filters independently.
    auto& interest = kq_interest_[fd];
    if (r != interest.first) {
        EV_SET(&changes[n++], fd, EVFILT_READ, r ? EV_ADD : EV_DELETE, 0, 0,
               nullptr);
    }
    if (w != interest.second) {
        EV_SET(&changes[n++], fd, EVFILT_WRITE, w ? EV_ADD : EV_DELETE, 0, 0,
               nullptr);
    }

    if (n > 0 && kevent(kq_, changes, n, nullptr, 0, nullptr) < 0) {
        // EV_DELETE of a filter that was never added returns ENOENT; that is
        // benign and must not be treated as a registration failure.
        if (errno != ENOENT) return false;
    }
    interest = {r, w};
    return true;
}

void EventLoop::kq_remove(int fd) {
    auto it = kq_interest_.find(fd);
    if (it == kq_interest_.end()) return;

    struct kevent changes[2];
    int n = 0;
    if (it->second.first) {
        EV_SET(&changes[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (it->second.second) {
        EV_SET(&changes[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    if (n > 0) kevent(kq_, changes, n, nullptr, 0, nullptr);
    kq_interest_.erase(it);
}

int EventLoop::kq_wait(std::vector<Event>& out, int timeout_ms) {
    out.clear();
    if (kq_interest_.empty()) return 0;

    // Room for both filters of every registered descriptor.
    std::size_t capacity = kq_interest_.size() * 2 + 8;
    kq_events_.resize(capacity * sizeof(struct kevent));
    struct kevent* events = reinterpret_cast<struct kevent*>(kq_events_.data());

    struct timespec ts;
    struct timespec* tsp = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        tsp = &ts;
    }

    int ready = kevent(kq_, nullptr, 0, events, static_cast<int>(capacity), tsp);
    if (ready < 0) return errno == EINTR ? 0 : -1;

    // kqueue reports each filter separately; coalesce per descriptor so the
    // caller sees the same shape of event as with the poll backend.
    std::unordered_map<int, std::size_t> seen;
    for (int i = 0; i < ready; ++i) {
        int fd = static_cast<int>(events[i].ident);
        auto it = seen.find(fd);
        if (it == seen.end()) {
            seen[fd] = out.size();
            out.push_back(Event{fd, false, false, false});
            it = seen.find(fd);
        }
        Event& ev = out[it->second];
        if (events[i].filter == EVFILT_READ) ev.readable = true;
        if (events[i].filter == EVFILT_WRITE) ev.writable = true;
        if (events[i].flags & EV_EOF) ev.readable = true;
        if (events[i].flags & EV_ERROR) ev.hangup = true;
    }
    return static_cast<int>(out.size());
}

#else

bool EventLoop::kq_apply(int, bool, bool) { return false; }
void EventLoop::kq_remove(int) {}
int EventLoop::kq_wait(std::vector<Event>&, int) { return -1; }

#endif

}  // namespace net
