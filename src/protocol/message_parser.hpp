#ifndef MESSAGE_PARSER_HPP
#define MESSAGE_PARSER_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "../network/client_session.hpp"

struct ParsedCommand {
    std::string type;
    std::vector<std::string> args;
    bool valid = false;
    std::string error_msg;
};

class ProtocolHandler {
public:
    static ParsedCommand parse(const std::string& line);
    
    // Validates arguments and executes session-level transitions
    static bool handle_session_command(
        int client_fd,
        const ParsedCommand& cmd,
        std::unordered_map<int, ClientSession>& sessions,
        std::unordered_map<std::string, std::unordered_set<int>>& subscribers,
        std::string& out_response,
        bool& should_disconnect
    );
};

#endif