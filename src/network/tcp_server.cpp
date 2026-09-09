#include "tcp_server.hpp"
#include <iostream>
#include <cstring>

TCPServer::TCPServer(int port) : server_fd(-1), port(port) {
    // Clear the address structure
    memset(&address, 0, sizeof(address));
}

void TCPServer::start() {
    // 1. Create the socket (IPv4, TCP)
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        std::cerr << "Failed to create socket\n";
        exit(EXIT_FAILURE);
    }

    // Optional but highly recommended: Allow immediate port reuse after a crash
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Bind the socket to the port
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Listen on any available network interface
    address.sin_port = htons(port);       // Convert port to network byte order

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed\n";
        exit(EXIT_FAILURE);
    }

    // 3. Listen for incoming connections
    // SOMAXCONN tells the OS to allow the maximum reasonable queue length
    if (listen(server_fd, SOMAXCONN) < 0) {
        std::cerr << "Listen failed\n";
        exit(EXIT_FAILURE);
    }
    std::cout << "Server listening on port " << port << "\n";
}

int TCPServer::accept_connection() {
    int addrlen = sizeof(address);
    // 4. Accept a new client connection
    // This blocks until a client actually connects
    int client_fd = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
    return client_fd;
}

TCPServer::~TCPServer() {
    if (server_fd != -1) {
        close(server_fd);
    }
}