#include "tcp_client.hpp"
#include <sys/socket.h>
#include <netinet/in.h>  // Add this line
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

TCPClient::TCPClient(const std::string& ip, int port) : sock_fd(-1), server_ip(ip), server_port(port) {}

TCPClient::~TCPClient() {
    disconnect();
}

bool TCPClient::connect_to_server() {
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) return false;

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr) <= 0) {
        return false;
    }

    if (connect(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        return false;
    }
    return true;
}

void TCPClient::send_message(const std::string& message) {
    if (sock_fd >= 0) {
        send(sock_fd, message.c_str(), message.length(), 0);
    }
}

std::string TCPClient::receive_message() {
    char buffer[1024] = {0};
    int bytes_read = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read > 0) {
        return std::string(buffer, bytes_read);
    }
    return "";
}

void TCPClient::disconnect() {
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
}