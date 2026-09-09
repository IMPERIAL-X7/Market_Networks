// The Socket Exchange - Exchange Server.
//
// Concurrency / I/O model: a single-threaded, non-blocking event loop over
// kqueue(2) (FreeBSD) or poll(2). Every socket, including the listening
// socket, is O_NONBLOCK, and every client has its own input and output buffer.
// No socket operation the server performs can block, so a client that never
// sends (Experiment 4) or never reads (Experiment 7) cannot stall any other
// client.

#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "network/client_session.hpp"
#include "network/event_loop.hpp"
#include "network/socket_utils.hpp"
#include "network/tcp_server.hpp"
#include "protocol/message_parser.hpp"
#include "trading/order_book.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace {

constexpr int kDefaultPort = 5000;
constexpr const char* kDefaultHost = "127.0.0.1";

// Largest application message accepted from a client. A peer that sends more
// than this without a newline is not framing correctly, and buffering it
// without bound would be a denial-of-service vector.
constexpr std::size_t kMaxLineBytes = 64 * 1024;

// How many unwritten bytes may pile up for one slow client before the server
// gives up on it. Reaching this means the client has stopped reading for long
// enough to exhaust both its receive window and this backlog.
constexpr std::size_t kMaxOutputBytes = 16 * 1024 * 1024;

constexpr std::size_t kReadChunk = 65536;

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

bool env_flag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

}  // namespace

class ExchangeServer {
public:
    ExchangeServer(const std::string& host, int port,
                   net::EventLoop::Backend backend, bool verbose,
                   int send_buffer_bytes)
        : listener_(host, port),
          loop_(backend),
          verbose_(verbose),
          send_buffer_bytes_(send_buffer_bytes) {}

    bool start() {
        if (!listener_.start(SOMAXCONN)) {
            std::fprintf(stderr, "exchange_server: %s\n",
                         listener_.error().c_str());
            return false;
        }
        if (!loop_.add(listener_.fd(), true, false)) {
            std::fprintf(stderr,
                         "exchange_server: cannot register listening socket\n");
            return false;
        }

        std::printf(
            "Exchange Server listening on %s:%d (listening socket fd %d, "
            "I/O backend %s)\n",
            listener_.host().c_str(), listener_.port(), listener_.fd(),
            net::EventLoop::backend_name(loop_.backend()));
        std::fflush(stdout);
        return true;
    }

    void run() {
        std::vector<net::Event> events;

        while (!g_stop) {
            int ready = loop_.wait(events, 1000);
            if (ready < 0) {
                if (errno == EINTR) continue;
                std::fprintf(stderr, "exchange_server: event wait failed: %s\n",
                             std::strerror(errno));
                break;
            }

            for (const net::Event& event : events) {
                if (g_stop) break;

                if (event.fd == listener_.fd()) {
                    accept_pending();
                    continue;
                }

                // A descriptor may have been dropped earlier in this same
                // batch; its remaining events are stale and must be ignored.
                if (sessions_.find(event.fd) == sessions_.end()) continue;

                if (event.writable) on_writable(event.fd);
                if (sessions_.find(event.fd) == sessions_.end()) continue;

                if (event.readable) on_readable(event.fd);
                if (sessions_.find(event.fd) == sessions_.end()) continue;

                if (event.hangup) {
                    drop(event.fd, "socket error reported by the event loop");
                }
            }

            // Descriptors are closed only once the whole batch is processed,
            // so that a number freed here cannot be handed straight back by
            // accept() and confused with a stale event above.
            reap_closed();
        }

        shutdown_all();
    }

private:
    TCPServer listener_;
    net::EventLoop loop_;
    bool verbose_;
    int send_buffer_bytes_;

    std::unordered_map<int, ClientSession> sessions_;
    std::unordered_map<std::string, std::unordered_set<int>> subscribers_;
    std::unordered_set<std::string> usernames_;
    std::vector<int> closed_fds_;
    OrderBook book_;

    // --- connection lifecycle ---------------------------------------------

    void accept_pending() {
        // Drain the accept queue: with level-triggered notification one
        // wakeup can correspond to several completed handshakes.
        for (;;) {
            bool again = false;
            std::string peer;
            int fd = listener_.accept_connection(&again, &peer);
            if (fd < 0) {
                if (again) return;
                if (!listener_.error().empty()) {
                    std::fprintf(stderr, "exchange_server: %s\n",
                                 listener_.error().c_str());
                }
                return;
            }

            if (send_buffer_bytes_ > 0) {
                net::set_send_buffer(fd, send_buffer_bytes_);
            }

            ClientSession session(fd);
            session.peer = peer;
            sessions_.emplace(fd, std::move(session));

            if (!loop_.add(fd, true, false)) {
                sessions_.erase(fd);
                close(fd);
                continue;
            }

            std::printf("[+] connection accepted: fd %d from %s (%zu clients)\n",
                        fd, sessions_[fd].peer.c_str(), sessions_.size());
            std::fflush(stdout);
        }
    }

    // Detaches a session from all server state. The descriptor itself is
    // closed later by reap_closed().
    void drop(int fd, const std::string& reason) {
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;

        ClientSession& session = it->second;
        for (const std::string& instrument : session.subscriptions) {
            subscribers_[instrument].erase(fd);
        }
        if (session.logged_in) usernames_.erase(session.username);
        book_.remove_orders_of(fd);

        std::printf("[-] connection closed: %s - %s (%zu clients remain)\n",
                    session.label().c_str(), reason.c_str(),
                    sessions_.size() - 1);
        std::fflush(stdout);

        sessions_.erase(it);
        loop_.remove(fd);
        closed_fds_.push_back(fd);
    }

    void reap_closed() {
        for (int fd : closed_fds_) close(fd);
        closed_fds_.clear();
    }

    void shutdown_all() {
        std::printf("Exchange Server shutting down; closing %zu connection(s).\n",
                    sessions_.size());
        std::fflush(stdout);
        for (auto& entry : sessions_) {
            // Send FIN so peers observe an orderly termination rather than a
            // reset when the process exits.
            ::shutdown(entry.first, SHUT_RDWR);
            close(entry.first);
        }
        sessions_.clear();
        reap_closed();
    }

    // --- output path -------------------------------------------------------

    void send_to(int fd, const std::string& message) {
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;
        ClientSession& session = it->second;
        session.outbuf.append(message);
        flush(session);
    }

    // Writes as much of the pending output as the socket will take right now.
    void flush(ClientSession& session) {
        while (session.has_pending_output()) {
            ssize_t written =
                ::send(session.fd, session.outbuf.data() + session.out_start,
                       session.pending_bytes(), MSG_NOSIGNAL);

            if (written > 0) {
                session.out_start += static_cast<std::size_t>(written);
                continue;
            }

            if (written < 0 && errno == EINTR) continue;

            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // The peer's receive window and this socket's send buffer are
                // both full: keep the rest queued and wait for writability.
                break;
            }

            // EPIPE / ECONNRESET: the peer is gone. This is the path taken in
            // Experiment 8, where the client process was killed outright.
            drop(session.fd, std::string("send() failed: ") +
                                 std::strerror(errno));
            return;
        }

        // Reclaim the consumed prefix without copying on every single write.
        if (!session.has_pending_output()) {
            session.outbuf.clear();
            session.out_start = 0;
        } else if (session.out_start > kReadChunk) {
            session.outbuf.erase(0, session.out_start);
            session.out_start = 0;
        }

        const std::size_t pending = session.pending_bytes();

        if (pending > kMaxOutputBytes) {
            drop(session.fd, "output backlog exceeded " +
                                 std::to_string(kMaxOutputBytes) +
                                 " bytes (client is not reading)");
            return;
        }

        if (pending > 0 && !session.backlogged) {
            session.backlogged = true;
            std::printf(
                "[!] %s is not draining its socket; buffering output "
                "(backlog now %zu bytes)\n",
                session.label().c_str(), pending);
            std::fflush(stdout);
        } else if (pending == 0 && session.backlogged) {
            session.backlogged = false;
            std::printf("[ ] %s drained its backlog\n",
                        session.label().c_str());
            std::fflush(stdout);
        }

        if (pending == 0 && session.closing) {
            drop(session.fd, "orderly close completed after flushing output");
            return;
        }

        // Only ask for write readiness while there is something to write;
        // otherwise the loop would spin on a permanently writable socket.
        // Re-registering costs a syscall, so it happens only on a real change.
        const bool want_read = !session.closing;
        const bool want_write = pending > 0;
        if (want_read != session.read_interest ||
            want_write != session.write_interest) {
            session.read_interest = want_read;
            session.write_interest = want_write;
            loop_.update(session.fd, want_read, want_write);
        }
    }

    void on_writable(int fd) {
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;
        flush(it->second);
    }

    // --- input path --------------------------------------------------------

    void on_readable(int fd) {
        char buffer[kReadChunk];

        for (;;) {
            // Re-resolved every pass: handling a message can drop the session,
            // which would leave any cached reference dangling.
            auto it = sessions_.find(fd);
            if (it == sessions_.end()) return;
            ClientSession& session = it->second;

            // A half-closed client has nothing more to say; keep flushing only.
            if (session.closing) return;

            ssize_t received = ::recv(fd, buffer, sizeof(buffer), 0);

            if (received > 0) {
                if (verbose_) {
                    std::printf("[recv] %s: %zd byte(s) from the TCP stream\n",
                                session.label().c_str(), received);
                    std::fflush(stdout);
                }
                session.inbuf.append(buffer, static_cast<std::size_t>(received));
                if (!consume_messages(fd)) return;  // session was dropped

                // Fewer bytes than the buffer holds means the socket is now
                // drained; going round again would only return EAGAIN.
                if (static_cast<std::size_t>(received) < sizeof(buffer)) return;
                continue;
            }

            if (received == 0) {
                // Orderly shutdown by the peer (FIN). Anything still queued is
                // written out before the server closes its own half.
                begin_orderly_close(fd, "peer closed its sending direction (FIN)");
                return;
            }

            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;

            drop(fd, std::string("recv() failed: ") + std::strerror(errno));
            return;
        }
    }

    void begin_orderly_close(int fd, const std::string& reason) {
        auto it = sessions_.find(fd);
        if (it == sessions_.end()) return;
        ClientSession& session = it->second;

        session.closing = true;
        session.inbuf.clear();

        if (!session.has_pending_output()) {
            drop(fd, reason);
            return;
        }
        // Stop reading, keep writing until the backlog is gone.
        ::shutdown(fd, SHUT_RD);
        session.read_interest = false;
        session.write_interest = true;
        loop_.update(fd, false, true);
    }

    // Extracts every complete newline-terminated message from the input
    // buffer. Returns false if the session was dropped along the way.
    bool consume_messages(int fd) {
        for (;;) {
            auto it = sessions_.find(fd);
            if (it == sessions_.end()) return false;
            ClientSession& session = it->second;

            std::size_t newline = session.inbuf.find('\n');
            if (newline == std::string::npos) {
                if (session.inbuf.size() > kMaxLineBytes) {
                    send_to(fd, protocol::error("Message exceeds " +
                                                std::to_string(kMaxLineBytes) +
                                                " bytes without a newline"));
                    begin_orderly_close(fd, "oversized application message");
                    return false;
                }
                return true;  // partial message: wait for the rest
            }

            std::string line = session.inbuf.substr(0, newline);
            session.inbuf.erase(0, newline + 1);

            // Tolerate CRLF from line-oriented tools such as telnet or nc.
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (verbose_) {
                std::printf("[msg ] %s: complete message %s\n",
                            session.label().c_str(),
                            ("\"" + line + "\"").c_str());
                std::fflush(stdout);
            }

            handle_message(fd, line);

            if (sessions_.find(fd) == sessions_.end()) return false;
            if (sessions_[fd].closing) return true;
        }
    }

    // --- protocol ----------------------------------------------------------

    void handle_message(int fd, const std::string& line) {
        const protocol::ParsedCommand cmd = protocol::parse(line);
        if (!cmd.valid) {
            send_to(fd, protocol::error(cmd.error));
            return;
        }

        switch (cmd.type) {
            case protocol::CommandType::QUIT:
                begin_orderly_close(fd, "client sent QUIT");
                return;
            case protocol::CommandType::LOGIN:
                handle_login(fd, cmd);
                return;
            case protocol::CommandType::SUBSCRIBE:
            case protocol::CommandType::UNSUBSCRIBE:
                handle_subscription(fd, cmd);
                return;
            case protocol::CommandType::BUY:
            case protocol::CommandType::SELL:
                handle_order(fd, cmd);
                return;
            case protocol::CommandType::CANCEL:
                handle_cancel(fd, cmd);
                return;
            case protocol::CommandType::UNKNOWN:
                send_to(fd, protocol::error("Unknown command"));
                return;
        }
    }

    // Rejects a command the client's role does not permit. Returns true when
    // the command was rejected and the caller should stop.
    bool reject_wrong_role(int fd, ClientRole required,
                           protocol::CommandType type) {
        ClientSession& session = sessions_[fd];
        if (session.role == ClientRole::UNKNOWN || session.role == required) {
            return false;
        }
        const char* actual = session.role == ClientRole::TRADER
                                 ? "trader"
                                 : "market-data";
        send_to(fd, protocol::error(std::string(protocol::verb(type)) +
                                    " is not permitted for a " + actual +
                                    " client"));
        return true;
    }

    void handle_login(int fd, const protocol::ParsedCommand& cmd) {
        if (reject_wrong_role(fd, ClientRole::TRADER, cmd.type)) return;
        ClientSession& session = sessions_[fd];

        if (session.logged_in) {
            send_to(fd, protocol::error("Already logged in as " +
                                        session.username));
            return;
        }
        const std::string& username = cmd.args[0];
        if (usernames_.count(username)) {
            send_to(fd, protocol::error("Username " + username +
                                        " is already in use"));
            return;
        }

        session.role = ClientRole::TRADER;
        session.username = username;
        session.logged_in = true;
        usernames_.insert(username);

        std::printf("[=] fd %d is trader '%s'\n", fd, username.c_str());
        std::fflush(stdout);

        send_to(fd, protocol::ok());
    }

    void handle_subscription(int fd, const protocol::ParsedCommand& cmd) {
        if (reject_wrong_role(fd, ClientRole::MARKET_DATA, cmd.type)) return;
        ClientSession& session = sessions_[fd];

        const bool subscribing = cmd.type == protocol::CommandType::SUBSCRIBE;

        if (!subscribing && !session.subscriptions.count(cmd.instrument)) {
            send_to(fd, protocol::error("Not subscribed to " + cmd.instrument));
            return;
        }

        if (session.role == ClientRole::UNKNOWN) {
            session.role = ClientRole::MARKET_DATA;
            std::printf("[=] fd %d is a market-data client\n", fd);
            std::fflush(stdout);
        }

        if (subscribing) {
            session.subscriptions.insert(cmd.instrument);
            subscribers_[cmd.instrument].insert(fd);
        } else {
            session.subscriptions.erase(cmd.instrument);
            subscribers_[cmd.instrument].erase(fd);
        }

        send_to(fd, protocol::ok());
    }

    // Trading commands additionally require a completed LOGIN.
    bool require_logged_in_trader(int fd, protocol::CommandType type) {
        if (reject_wrong_role(fd, ClientRole::TRADER, type)) return false;
        if (!sessions_[fd].logged_in) {
            send_to(fd, protocol::error("Not logged in: send LOGIN <username> "
                                        "before trading"));
            return false;
        }
        return true;
    }

    void handle_order(int fd, const protocol::ParsedCommand& cmd) {
        if (!require_logged_in_trader(fd, cmd.type)) return;

        const Side side =
            cmd.type == protocol::CommandType::BUY ? Side::BUY : Side::SELL;

        std::vector<TradeExecution> executions;
        const long order_id = book_.add_order(fd, cmd.instrument, side,
                                              cmd.quantity, cmd.price,
                                              executions);

        // The acknowledgement is queued before any execution notification, so
        // the trader always sees ORDER_ACCEPTED ahead of its own BOUGHT/SOLD.
        send_to(fd, protocol::order_accepted(order_id));

        for (const TradeExecution& execution : executions) {
            publish(execution);
            // Notifying a counterparty can drop it (Experiment 8), which is
            // why every send is looked up by descriptor rather than cached.
            if (sessions_.find(fd) == sessions_.end()) return;
        }
    }

    void publish(const TradeExecution& execution) {
        send_to(execution.buyer_fd,
                protocol::bought(execution.instrument, execution.quantity,
                                 execution.price));
        send_to(execution.seller_fd,
                protocol::sold(execution.instrument, execution.quantity,
                               execution.price));

        const std::string update = protocol::trade(
            execution.instrument, execution.quantity, execution.price);

        auto it = subscribers_.find(execution.instrument);
        if (it == subscribers_.end()) return;

        // Copy the subscriber list: delivering to one subscriber may drop it
        // and mutate the set being iterated.
        const std::vector<int> targets(it->second.begin(), it->second.end());
        for (int subscriber_fd : targets) {
            send_to(subscriber_fd, update);
        }
    }

    void handle_cancel(int fd, const protocol::ParsedCommand& cmd) {
        if (!require_logged_in_trader(fd, cmd.type)) return;

        std::string error;
        if (book_.cancel_order(cmd.order_id, fd, error)) {
            send_to(fd, protocol::order_cancelled(cmd.order_id));
        } else {
            send_to(fd, protocol::error(error));
        }
    }
};

namespace {

void usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [host] [port] [--backend=poll|kqueue] [--verbose]\n"
                 "\n"
                 "  host, port          address to listen on "
                 "(default %s %d)\n"
                 "  --backend=NAME      I/O readiness mechanism\n"
                 "  --verbose, -v       log every recv() and every framed "
                 "message\n"
                 "\n"
                 "Environment: EXCHANGE_BACKEND, EXCHANGE_VERBOSE, "
                 "EXCHANGE_SNDBUF\n",
                 program, kDefaultHost, kDefaultPort);
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = kDefaultHost;
    int port = kDefaultPort;
    bool verbose = env_flag("EXCHANGE_VERBOSE");

    // Optional: shrink the per-connection send buffer so that a client which
    // stops reading causes send() to return EAGAIN at a modest data volume.
    int send_buffer_bytes = 0;
    if (const char* value = std::getenv("EXCHANGE_SNDBUF")) {
        send_buffer_bytes = std::atoi(value);
        if (send_buffer_bytes < 0) send_buffer_bytes = 0;
    }

    net::EventLoop::Backend backend = net::EventLoop::default_backend();
    if (const char* name = std::getenv("EXCHANGE_BACKEND")) {
        bool ok = false;
        backend = net::EventLoop::parse_backend(name, &ok);
        if (!ok) {
            std::fprintf(stderr,
                         "exchange_server: unsupported backend '%s'; using %s\n",
                         name, net::EventLoop::backend_name(backend));
        }
    }

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg.rfind("--backend=", 0) == 0) {
            bool ok = false;
            backend = net::EventLoop::parse_backend(arg.substr(10), &ok);
            if (!ok) {
                std::fprintf(stderr,
                             "exchange_server: unsupported backend '%s'; "
                             "using %s\n",
                             arg.substr(10).c_str(),
                             net::EventLoop::backend_name(backend));
            }
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            usage(argv[0]);
            return 2;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() > 2) {
        usage(argv[0]);
        return 2;
    }
    if (positional.size() >= 1) host = positional[0];
    if (positional.size() >= 2) {
        port = std::atoi(positional[1].c_str());
        if (port <= 0 || port > 65535) {
            std::fprintf(stderr, "exchange_server: invalid port '%s'\n",
                         positional[1].c_str());
            return 2;
        }
    }

    // Writing to a socket whose peer has vanished must return EPIPE, not kill
    // the process. This is what keeps the server alive in Experiment 8.
    signal(SIGPIPE, SIG_IGN);

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    net::raise_fd_limit();

    ExchangeServer server(host, port, backend, verbose, send_buffer_bytes);
    if (!server.start()) return 1;
    server.run();
    return 0;
}
