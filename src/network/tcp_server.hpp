#ifndef TCP_SERVER_HPP
#define TCP_SERVER_HPP

#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

class TCPServer {
private:
    int server_fd;
    int port;
    struct sockaddr_in address;
public:
    TCPServer(int port);
    ~TCPServer();

    // Initializes the socket, binds, and starts listening
    void start(); 
    
    // Accepts a new incoming client connection
    int accept_connection(); 

    int get_server_fd() const { return server_fd; }
};

#endif