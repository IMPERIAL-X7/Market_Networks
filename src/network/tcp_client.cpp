#include "tcp_client.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "socket_utils.hpp"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

TCPClient::TCPClient(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

TCPClient::~TCPClient() { disconnect(); }

bool TCPClient::connect_to_server() {
    fd_ = net::connect_to(host_, port_, error_);
    if (fd_ < 0) return false;
    net::set_tcp_nodelay(fd_);
    // Non-blocking receives let the client interleave server notifications
    // with keyboard input in a single poll() loop.
    net::set_nonblocking(fd_);
    return true;
}

bool TCPClient::send_message(const std::string& line) {
    if (fd_ < 0) return false;

    // One send() call is not guaranteed to accept the whole buffer, so the
    // remainder is retried until the message has been fully written.
    std::size_t sent = 0;
    while (sent < line.size()) {
        ssize_t written = ::send(fd_, line.data() + sent, line.size() - sent,
                                 MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The send buffer is momentarily full; wait for room rather than
            // busy-looping or dropping the tail of the message.
            struct pollfd wait_write;
            wait_write.fd = fd_;
            wait_write.events = POLLOUT;
            wait_write.revents = 0;
            if (::poll(&wait_write, 1, -1) < 0 && errno != EINTR) {
                error_ = std::string("poll() failed: ") + std::strerror(errno);
                return false;
            }
            continue;
        }
        error_ = std::string("send() failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool TCPClient::receive_messages(std::vector<std::string>& out) {
    char buffer[8192];

    for (;;) {
        ssize_t received = ::recv(fd_, buffer, sizeof(buffer), 0);

        if (received > 0) {
            inbuf_.append(buffer, static_cast<std::size_t>(received));

            std::size_t newline;
            while ((newline = inbuf_.find('\n')) != std::string::npos) {
                std::string line = inbuf_.substr(0, newline);
                inbuf_.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                out.push_back(line);
            }

            if (static_cast<std::size_t>(received) < sizeof(buffer)) return true;
            continue;
        }

        if (received == 0) {
            error_ = "server closed the connection";
            return false;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;

        error_ = std::string("recv() failed: ") + std::strerror(errno);
        return false;
    }
}

void TCPClient::shutdown_writes() {
    if (fd_ >= 0) ::shutdown(fd_, SHUT_WR);
}

void TCPClient::disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}
