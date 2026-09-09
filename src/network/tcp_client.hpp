#ifndef TCP_CLIENT_HPP
#define TCP_CLIENT_HPP

#include <string>
#include <vector>

// Client-side end of a persistent TCP connection to the Exchange Server.
//
// Reassembling messages is the client's job too: recv() returns whatever
// bytes have arrived, which may be half a message or several at once.
class TCPClient {
public:
    TCPClient(std::string host, int port);
    ~TCPClient();

    TCPClient(const TCPClient&) = delete;
    TCPClient& operator=(const TCPClient&) = delete;

    bool connect_to_server();

    int fd() const { return fd_; }
    const std::string& error() const { return error_; }

    // Sends one complete application message, retrying until every byte has
    // been handed to the kernel. Returns false if the connection failed.
    bool send_message(const std::string& line);

    // Reads whatever has arrived and appends every complete message to `out`.
    // Returns false when the server closed the connection or the socket
    // failed; `out` may still have received messages in that case.
    bool receive_messages(std::vector<std::string>& out);

    // Sends FIN while leaving the receive direction open, so the server's
    // final messages can still be read.
    void shutdown_writes();

    void disconnect();

private:
    std::string host_;
    int port_;
    int fd_ = -1;
    std::string inbuf_;
    std::string error_;
};

#endif
