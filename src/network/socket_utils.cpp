#include "socket_utils.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace net {

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

bool set_tcp_nodelay(int fd) {
    int opt = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
}

long raise_fd_limit() {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return -1;
    if (rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        getrlimit(RLIMIT_NOFILE, &rl);
    }
    return static_cast<long>(rl.rlim_cur);
}

// Resolves host into an IPv4 address. An empty host means "any interface".
static bool resolve_ipv4(const std::string& host, struct in_addr* out,
                         std::string& err) {
    if (host.empty() || host == "*") {
        out->s_addr = htonl(INADDR_ANY);
        return true;
    }
    if (inet_pton(AF_INET, host.c_str(), out) == 1) return true;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        err = "cannot resolve host '" + host + "': " + gai_strerror(rc);
        return false;
    }
    *out = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

int create_listening_socket(const std::string& host, int port, int backlog,
                            std::string& err) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (!resolve_ipv4(host, &addr.sin_addr, err)) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("socket() failed: ") + std::strerror(errno);
        return -1;
    }

    // Without SO_REUSEADDR a restart within TIME_WAIT would fail to bind,
    // which matters because the experiment harness restarts the server often.
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("bind() failed: ") + std::strerror(errno);
        close(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        err = std::string("listen() failed: ") + std::strerror(errno);
        close(fd);
        return -1;
    }

    if (!set_nonblocking(fd)) {
        err = std::string("fcntl(O_NONBLOCK) failed: ") + std::strerror(errno);
        close(fd);
        return -1;
    }

    return fd;
}

int connect_to(const std::string& host, int port, std::string& err) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (!resolve_ipv4(host.empty() ? "127.0.0.1" : host, &addr.sin_addr, err)) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("socket() failed: ") + std::strerror(errno);
        return -1;
    }

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = std::string("connect() failed: ") + std::strerror(errno);
        close(fd);
        return -1;
    }

    return fd;
}

int set_send_buffer(int fd, int bytes) {
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    int applied = 0;
    socklen_t len = sizeof(applied);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &applied, &len) != 0) return -1;
    return applied;
}

std::string peer_name(int fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        return "<unknown>";
    }
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf)) == nullptr) {
        return "<unknown>";
    }
    return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

}  // namespace net
