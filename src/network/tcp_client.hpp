#ifndef TCP_CLIENT_HPP
#define TCP_CLIENT_HPP

#include <string>

class TCPClient {
private:
    int sock_fd;
    std::string server_ip;
    int server_port;

public:
    TCPClient(const std::string& ip, int port);
    ~TCPClient();

    bool connect_to_server();
    void send_message(const std::string& message);
    std::string receive_message();
    void disconnect();
};

#endif