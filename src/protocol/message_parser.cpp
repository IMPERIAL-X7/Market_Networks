#include "message_parser.hpp"
#include <sstream>

ParsedCommand ProtocolHandler::parse(const std::string& line) {
    ParsedCommand cmd;
    std::stringstream ss(line);
    std::string token;

    if (!(ss >> cmd.type)) {
        cmd.valid = false;
        cmd.error_msg = "Empty command";
        return cmd;
    }

    while (ss >> token) {
        cmd.args.push_back(token);
    }
    cmd.valid = true;
    return cmd;
}

static bool is_valid_instrument(const std::string& inst) {
    return inst == "JNST" || inst == "IMCT";
}

static bool is_positive_integer(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(c)) return false;
    }
    try {
        long val = std::stol(s);
        return val > 0;
    } catch (...) {
        return false;
    }
}

static bool is_non_negative_integer(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(c)) return false;
    }
    return true; // Since it's all digits, it naturally includes 0
}

bool ProtocolHandler::handle_session_command(
    int client_fd,
    const ParsedCommand& cmd,
    std::unordered_map<int, ClientSession>& sessions,
    std::unordered_map<std::string, std::unordered_set<int>>& subscribers,
    std::string& out_response,
    bool& should_disconnect
) {
    should_disconnect = false;
    ClientSession& session = sessions[client_fd];

    if (cmd.type == "QUIT") {
        should_disconnect = true;
        return true;
    }

    // Role Enforcement & Dispatch
    if (cmd.type == "LOGIN") {
        if (session.role == ClientRole::MARKET_DATA) {
            out_response = "ERROR Market-Data client cannot login as trader\n";
            return false;
        }
        if (cmd.args.size() != 1) {
            out_response = "ERROR Invalid LOGIN syntax\n";
            return false;
        }
        std::string user = cmd.args[0];
        // Ensure username is unique among active traders
        for (const auto& pair : sessions) {
            if (pair.second.role == ClientRole::TRADER && pair.second.username == user) {
                out_response = "ERROR Username already taken\n";
                return false;
            }
        }
        session.role = ClientRole::TRADER;
        session.username = user;
        out_response = "OK\n";
        return true;
    } 
    else if (cmd.type == "SUBSCRIBE" || cmd.type == "UNSUBSCRIBE") {
        if (session.role == ClientRole::TRADER) {
            out_response = "ERROR Trader client cannot subscribe to market data\n";
            return false;
        }
        if (cmd.args.size() != 1 || !is_valid_instrument(cmd.args[0])) {
            out_response = "ERROR Invalid instrument\n";
            return false;
        }
        
        std::string inst = cmd.args[0];
        session.role = ClientRole::MARKET_DATA;

        if (cmd.type == "SUBSCRIBE") {
            session.subscriptions.insert(inst);
            subscribers[inst].insert(client_fd);
        } else {
            session.subscriptions.erase(inst);
            subscribers[inst].erase(client_fd);
        }
        out_response = "OK\n";
        return true;
    }

    // Trading Commands (BUY, SELL, CANCEL) require active Trader status
    if (session.role != ClientRole::TRADER) {
        out_response = "ERROR Must login as trader first\n";
        return false;
    }

    if (cmd.type == "BUY" || cmd.type == "SELL") {
        if (cmd.args.size() != 3 || !is_valid_instrument(cmd.args[0]) ||
            !is_positive_integer(cmd.args[1]) || !is_positive_integer(cmd.args[2])) {
            out_response = "ERROR Invalid order parameters\n";
            return false;
        }
        return true; // Validated; ready for OrderBook execution
    } 
    else if (cmd.type == "CANCEL") {
        if (cmd.args.size() != 1 || !is_non_negative_integer(cmd.args[0])) {
            out_response = "ERROR Invalid CANCEL order id\n";
            return false;
        }
        return true; // Validated; ready for OrderBook cancellation
    }

    out_response = "ERROR Unknown command\n";
    return false;
}