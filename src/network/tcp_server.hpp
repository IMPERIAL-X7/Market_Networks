#ifndef TCP_SERVER_HPP
#define TCP_SERVER_HPP

#include <string>

// The Exchange Server's listening socket.
//
// This owns exactly one descriptor: the passive socket produced by
// socket() + bind() + listen(). Every accepted connection gets its own
// separate descriptor, which is what distinguishes the listening socket from
// the connected sockets in Experiment 1.
class TCPServer {
public:
    TCPServer(std::string host, int port);
    ~TCPServer();

    TCPServer(const TCPServer&) = delete;
    TCPServer& operator=(const TCPServer&) = delete;

    // Creates, binds and listens. Returns false with a reason in error().
    bool start(int backlog);

    // Accepts one pending connection and returns a non-blocking descriptor.
    // Returns -1 when the backlog is momentarily empty (EAGAIN) or the
    // connection died before it could be accepted; `again` distinguishes
    // "nothing left to accept" from a real error.
    //
    // `exhausted` is set when the failure was a resource limit (EMFILE,
    // ENFILE, ENOBUFS, ENOMEM). Retrying immediately in that case is futile
    // and harmful: the listening socket stays readable, so the event loop
    // reports it again at once and the server spins at full CPU.
    // `peer` receives the client's "ip:port" as reported by accept() itself,
    // which stays available even if the connection is reset immediately after.
    int accept_connection(bool* again, std::string* peer, bool* exhausted);

    int fd() const { return fd_; }
    int port() const { return port_; }
    const std::string& host() const { return host_; }
    const std::string& error() const { return error_; }

private:
    std::string host_;
    int port_;
    int fd_ = -1;
    std::string error_;
};

#endif
