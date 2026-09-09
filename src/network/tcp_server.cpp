#include "tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
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

int TCPServer::accept_connection(bool* again, std::string* peer,
                                 bool* exhausted) {
    if (again) *again = false;
    if (exhausted) *exhausted = false;

    struct sockaddr_in address;
    socklen_t address_len = sizeof(address);
    int client_fd = accept(fd_, reinterpret_cast<struct sockaddr*>(&address),
                           &address_len);
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (again) *again = true;
        } else if (errno == ECONNABORTED || errno == EINTR) {
            // The peer went away between the SYN and our accept(); the
            // listening socket itself is still healthy.
            if (again) *again = false;
        } else if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
                   errno == ENOMEM) {
            if (exhausted) *exhausted = true;
            error_ = std::string("accept() failed: ") + std::strerror(errno);
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

    if (peer != nullptr) {
        char text[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text))) {
            *peer = std::string(text) + ":" +
                    std::to_string(ntohs(address.sin_port));
        } else {
            *peer = "<unknown>";
        }
    }

    return client_fd;
}
