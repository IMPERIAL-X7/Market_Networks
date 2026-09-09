#ifndef SOCKET_UTILS_HPP
#define SOCKET_UTILS_HPP

#include <string>

namespace net {

// Puts a descriptor into non-blocking mode (O_NONBLOCK).
// Returns false and leaves errno set on failure.
bool set_nonblocking(int fd);

// Disables Nagle's algorithm so that small protocol messages are not delayed.
bool set_tcp_nodelay(int fd);

// Raises RLIMIT_NOFILE to the hard limit and returns the resulting soft limit.
// Needed for the connection-scalability bonus, harmless otherwise.
long raise_fd_limit();

// Creates a listening TCP socket bound to host:port.
//   host may be an IPv4 dotted-quad, a hostname, or "" / "0.0.0.0" for INADDR_ANY.
// The returned descriptor is non-blocking and has SO_REUSEADDR set.
// Returns -1 on failure, with a human-readable reason in err.
int create_listening_socket(const std::string& host, int port, int backlog,
                            std::string& err);

// Creates a blocking TCP socket connected to host:port.
// Returns -1 on failure, with a human-readable reason in err.
int connect_to(const std::string& host, int port, std::string& err);

// Formats "ip:port" for a connected descriptor's peer, for logging.
std::string peer_name(int fd);

}  // namespace net

#endif
