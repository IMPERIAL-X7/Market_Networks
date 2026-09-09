#include "tcp_server.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "socket_utils.hpp"

TCPServer::TCPServer(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

TCPServer::~TCPServer() {
    if (fd_ >= 0) close(fd_);
}

bool TCPServer::start(int backlog) {
    fd_ = net::create_listening_socket(host_, port_, backlog, error_);
    return fd_ >= 0;
}

int TCPServer::accept_connection(bool* again) {
    if (again) *again = false;

    int client_fd = accept(fd_, nullptr, nullptr);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (again) *again = true;
        } else if (errno == ECONNABORTED || errno == EINTR) {
            // The peer went away between the SYN and our accept(); the
            // listening socket itself is still healthy.
            if (again) *again = false;
        } else {
            error_ = std::string("accept() failed: ") + std::strerror(errno);
        }
        return -1;
    }

    if (!net::set_nonblocking(client_fd)) {
        error_ = std::string("fcntl(O_NONBLOCK) failed: ") +
                 std::strerror(errno);
        close(client_fd);
        return -1;
    }
    net::set_tcp_nodelay(client_fd);

    return client_fd;
}
