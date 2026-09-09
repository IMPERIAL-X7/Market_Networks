#ifndef EVENT_LOOP_HPP
#define EVENT_LOOP_HPP

#include <poll.h>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
#define EXCHANGE_HAVE_KQUEUE 1
#else
#define EXCHANGE_HAVE_KQUEUE 0
#endif

namespace net {

// One readiness notification for a descriptor.
struct Event {
    int fd = -1;
    bool readable = false;
    bool writable = false;
    bool hangup = false;  // peer closed or the descriptor errored
};

// I/O readiness notification over a set of descriptors.
//
// Two interchangeable backends are provided so that the same server binary can
// be measured under both mechanisms (see the connection-scalability bonus):
//
//   Backend::Poll   - poll(2), portable, O(n) per call in the number of
//                     registered descriptors.
//   Backend::Kqueue - kqueue(2)/kevent(2), FreeBSD-native, O(1) per registered
//                     descriptor and O(ready) per call.
class EventLoop {
public:
    enum class Backend { Poll, Kqueue };

    // Returns the backend actually used; falls back to Poll when kqueue is
    // unavailable on this platform.
    static Backend default_backend();
    static Backend parse_backend(const std::string& name, bool* ok);
    static const char* backend_name(Backend backend);

    explicit EventLoop(Backend backend);
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    Backend backend() const { return backend_; }

    // Registers fd. At least one of want_read / want_write should be true.
    bool add(int fd, bool want_read, bool want_write);

    // Updates the interest set for an already registered fd.
    bool update(int fd, bool want_read, bool want_write);

    // Unregisters fd. Must be called before close() so that the poll backend
    // can drop its bookkeeping; kqueue would drop it automatically.
    void remove(int fd);

    // Blocks until at least one descriptor is ready or timeout_ms elapses
    // (-1 means wait forever). Fills out and returns its size, or -1 on error
    // other than EINTR.
    int wait(std::vector<Event>& out, int timeout_ms);

    std::size_t size() const;

private:
    Backend backend_;

    // --- poll backend state ---
    std::vector<struct pollfd> pollfds_;
    std::unordered_map<int, std::size_t> poll_index_;

    // --- kqueue backend state ---
    int kq_ = -1;
    std::unordered_map<int, std::pair<bool, bool>> kq_interest_;
    std::vector<char> kq_events_;  // raw storage for struct kevent array

    bool poll_add(int fd, bool r, bool w);
    bool poll_update(int fd, bool r, bool w);
    void poll_remove(int fd);
    int poll_wait(std::vector<Event>& out, int timeout_ms);

    bool kq_apply(int fd, bool r, bool w);
    void kq_remove(int fd);
    int kq_wait(std::vector<Event>& out, int timeout_ms);
};

}  // namespace net

#endif
