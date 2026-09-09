#ifndef CLIENT_SESSION_HPP
#define CLIENT_SESSION_HPP

#include <cstddef>
#include <string>
#include <unordered_set>

// A client's role is not announced up front: it is inferred from the first
// role-specific command it sends (LOGIN => trader, SUBSCRIBE => market data),
// and is then fixed for the lifetime of the connection.
enum class ClientRole { UNKNOWN, TRADER, MARKET_DATA };

// Per-connection state held by the Exchange Server.
//
// Both directions are buffered. `inbuf` reassembles application messages from
// a TCP byte stream that splits and coalesces them arbitrarily; `outbuf` holds
// bytes the server has produced but the kernel socket buffer could not accept
// yet, which is what keeps one slow reader from stalling every other client.
struct ClientSession {
    int fd = -1;
    ClientRole role = ClientRole::UNKNOWN;

    std::string inbuf;
    std::string outbuf;
    std::size_t out_start = 0;  // bytes of outbuf already written

    // Set once the peer has sent EOF or QUIT: no further input is read, and
    // the connection is closed as soon as outbuf has drained.
    bool closing = false;

    // True while outbuf is non-empty, used only to log backpressure once per
    // episode rather than once per queued message.
    bool backlogged = false;

    // Mirrors the write-readiness interest currently registered with the event
    // loop, so the server only issues a registration syscall when it changes.
    bool write_interest = false;
    bool read_interest = true;

    std::string peer;  // "ip:port", for logging

    // Trader state
    std::string username;
    bool logged_in = false;

    // Market-data state
    std::unordered_set<std::string> subscriptions;

    ClientSession() = default;
    explicit ClientSession(int file_descriptor) : fd(file_descriptor) {}

    std::size_t pending_bytes() const { return outbuf.size() - out_start; }
    bool has_pending_output() const { return pending_bytes() > 0; }

    // Human-readable identity for log lines.
    std::string label() const {
        std::string who = "fd " + std::to_string(fd);
        if (!username.empty()) who += " (" + username + ")";
        return who;
    }
};

#endif
