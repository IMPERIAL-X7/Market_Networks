// The Socket Exchange - bonus connection generator.
//
// Opens and holds a large number of simultaneous, idle TCP connections to the
// Exchange Server so that per-connection resource consumption can be measured
// with FreeBSD tools (sockstat, netstat, procstat, top, sysctl).
//
// usage: conn_gen <host> <port> <count> [options]
//
// The connections carry no application data: the point is to isolate the cost
// of a TCP connection and its kernel socket buffers from anything the
// application protocol does.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "network/socket_utils.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

double now_seconds() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

void usage(const char* program) {
    std::fprintf(
        stderr,
        "usage: %s <host> <port> <count> [options]\n"
        "\n"
        "  --report-every N   progress line every N connections "
        "(default 5000)\n"
        "  --src-ips LIST     comma-separated local addresses to bind "
        "round-robin.\n"
        "                     Each source address has its own ephemeral port\n"
        "                     range, which is how you get past ~65k "
        "connections\n"
        "                     to a single server address and port. Create the\n"
        "                     extra addresses first, e.g.\n"
        "                       ifconfig lo0 alias 127.0.0.2/8\n"
        "  --hold             keep the connections open until interrupted "
        "(default)\n"
        "  --no-hold          exit as soon as the target count is reached\n",
        program);
}

// Binds the socket to a specific local address so that the ephemeral port
// range of that address is used, rather than sharing one range for everything.
bool bind_source(int fd, const std::string& address) {
    struct sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = 0;  // let the kernel choose the port
    if (inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) return false;
    return bind(fd, reinterpret_cast<struct sockaddr*>(&local),
                sizeof(local)) == 0;
}

std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t comma = text.find(',', start);
        if (comma == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, comma - start));
        start = comma + 1;
    }
    return parts;
}

void report(std::size_t established, double elapsed, long fd_limit) {
    std::printf(
        "established %zu connections in %.2fs (%.0f conn/s), "
        "RLIMIT_NOFILE soft limit %ld\n",
        established, elapsed, elapsed > 0 ? established / elapsed : 0.0,
        fd_limit);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }

    const std::string host = argv[1];
    const int port = std::atoi(argv[2]);
    const long target = std::atol(argv[3]);

    if (port <= 0 || port > 65535 || target <= 0) {
        usage(argv[0]);
        return 2;
    }

    long report_every = 5000;
    bool hold = true;
    std::vector<std::string> source_addresses;

    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--report-every" && i + 1 < argc) {
            report_every = std::atol(argv[++i]);
            if (report_every <= 0) report_every = 5000;
        } else if (arg == "--src-ips" && i + 1 < argc) {
            source_addresses = split_commas(argv[++i]);
        } else if (arg == "--hold") {
            hold = true;
        } else if (arg == "--no-hold") {
            hold = false;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    const long fd_limit = net::raise_fd_limit();
    std::printf("conn_gen: target %ld connections to %s:%d, "
                "RLIMIT_NOFILE soft limit %ld\n",
                target, host.c_str(), port, fd_limit);
    if (fd_limit >= 0 && fd_limit < target + 16) {
        std::printf("conn_gen: warning - the descriptor limit is below the "
                    "target; expect EMFILE.\n");
    }
    std::fflush(stdout);

    std::vector<int> connections;
    connections.reserve(static_cast<std::size_t>(target));

    const double started = now_seconds();
    std::string error;

    for (long i = 0; i < target && !g_stop; ++i) {
        int fd;

        if (source_addresses.empty()) {
            fd = net::connect_to(host, port, error);
        } else {
            // Manual path so the socket can be bound to a chosen source
            // address before connect() picks an ephemeral port for it.
            struct sockaddr_in remote;
            std::memset(&remote, 0, sizeof(remote));
            remote.sin_family = AF_INET;
            remote.sin_port = htons(static_cast<uint16_t>(port));
            if (inet_pton(AF_INET, host.c_str(), &remote.sin_addr) != 1) {
                std::fprintf(stderr, "conn_gen: --src-ips requires a numeric "
                                     "server address\n");
                return 2;
            }

            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                const std::string& source =
                    source_addresses[connections.size() %
                                     source_addresses.size()];
                if (!bind_source(fd, source) ||
                    connect(fd, reinterpret_cast<struct sockaddr*>(&remote),
                            sizeof(remote)) < 0) {
                    error = "bind()/connect() from " + source + " failed: " +
                            std::strerror(errno);
                    close(fd);
                    fd = -1;
                }
            } else {
                error = std::string("socket() failed: ") + std::strerror(errno);
            }
        }

        if (fd < 0) {
            std::printf(
                "\nconn_gen: stopped after %zu established connections.\n"
                "conn_gen: first failure at connection %ld: %s\n",
                connections.size(), i + 1, error.c_str());
            std::fflush(stdout);
            break;
        }

        connections.push_back(fd);

        if (static_cast<long>(connections.size()) % report_every == 0) {
            report(connections.size(), now_seconds() - started, fd_limit);
        }
    }

    report(connections.size(), now_seconds() - started, fd_limit);

    struct rusage usage_info;
    if (getrusage(RUSAGE_SELF, &usage_info) == 0) {
        std::printf("conn_gen: own max RSS %ld KiB, user %.2fs, system %.2fs\n",
                    usage_info.ru_maxrss,
                    usage_info.ru_utime.tv_sec +
                        usage_info.ru_utime.tv_usec / 1e6,
                    usage_info.ru_stime.tv_sec +
                        usage_info.ru_stime.tv_usec / 1e6);
    }
    std::printf("conn_gen: pid %d. Take the server-side measurements now.\n",
                static_cast<int>(getpid()));
    std::fflush(stdout);

    // Idle hold: the connections stay established, exchanging no application
    // data, until the measurements have been taken.
    while (hold && !g_stop) {
        struct timespec interval;
        interval.tv_sec = 1;
        interval.tv_nsec = 0;
        nanosleep(&interval, nullptr);
    }

    std::printf("conn_gen: closing %zu connections.\n", connections.size());
    std::fflush(stdout);
    for (int fd : connections) close(fd);
    return 0;
}
