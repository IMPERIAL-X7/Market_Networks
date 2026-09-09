#ifndef CLIENT_SESSION_HPP
#define CLIENT_SESSION_HPP

#include <string>
#include <unordered_set>

enum class ClientRole {
    UNKNOWN,
    TRADER,
    MARKET_DATA
};

struct ClientSession {
    int fd;
    ClientRole role = ClientRole::UNKNOWN;
    
    // Byte buffer for TCP message framing
    std::string buffer;
    
    // Trader-specific state
    std::string username;
    
    // Market-Data-specific state
    std::unordered_set<std::string> subscriptions;

    ClientSession() : fd(-1) {}
    ClientSession(int file_descriptor) : fd(file_descriptor) {}
};

#endif