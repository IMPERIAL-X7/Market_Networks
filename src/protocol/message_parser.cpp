#include "message_parser.hpp"

#include <cctype>
#include <climits>
#include <sstream>

namespace protocol {

const char* const kInstruments[2] = {"JNST", "IMCT"};

bool is_valid_instrument(const std::string& instrument) {
    for (const char* name : kInstruments) {
        if (instrument == name) return true;
    }
    return false;
}

const char* verb(CommandType type) {
    switch (type) {
        case CommandType::LOGIN: return "LOGIN";
        case CommandType::BUY: return "BUY";
        case CommandType::SELL: return "SELL";
        case CommandType::CANCEL: return "CANCEL";
        case CommandType::SUBSCRIBE: return "SUBSCRIBE";
        case CommandType::UNSUBSCRIBE: return "UNSUBSCRIBE";
        case CommandType::QUIT: return "QUIT";
        case CommandType::UNKNOWN: break;
    }
    return "UNKNOWN";
}

// Strictly parses a decimal integer. The protocol permits digits only: no
// sign, no decimal point, no whitespace, no thousands separators. Values that
// would overflow a long are rejected rather than wrapped.
static bool parse_integer(const std::string& text, long* out) {
    if (text.empty() || text.size() > 18) return false;
    long value = 0;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

static CommandType verb_of(const std::string& token) {
    if (token == "LOGIN") return CommandType::LOGIN;
    if (token == "BUY") return CommandType::BUY;
    if (token == "SELL") return CommandType::SELL;
    if (token == "CANCEL") return CommandType::CANCEL;
    if (token == "SUBSCRIBE") return CommandType::SUBSCRIBE;
    if (token == "UNSUBSCRIBE") return CommandType::UNSUBSCRIBE;
    if (token == "QUIT") return CommandType::QUIT;
    return CommandType::UNKNOWN;
}

static ParsedCommand reject(ParsedCommand cmd, const std::string& reason) {
    cmd.valid = false;
    cmd.error = reason;
    return cmd;
}

ParsedCommand parse(const std::string& line) {
    ParsedCommand cmd;

    std::istringstream stream(line);
    std::string token;
    if (!(stream >> token)) return reject(cmd, "Empty message");

    cmd.type = verb_of(token);
    while (stream >> token) cmd.args.push_back(token);

    if (cmd.type == CommandType::UNKNOWN) {
        return reject(cmd, "Unknown command");
    }

    switch (cmd.type) {
        case CommandType::LOGIN: {
            if (cmd.args.size() != 1) {
                return reject(cmd, "Usage: LOGIN <username>");
            }
            if (cmd.args[0].size() > 64) {
                return reject(cmd, "Username too long");
            }
            break;
        }

        case CommandType::BUY:
        case CommandType::SELL: {
            if (cmd.args.size() != 3) {
                return reject(cmd, std::string("Usage: ") + verb(cmd.type) +
                                       " <instrument> <quantity> <price>");
            }
            if (!is_valid_instrument(cmd.args[0])) {
                return reject(cmd, "Unknown instrument " + cmd.args[0]);
            }
            if (!parse_integer(cmd.args[1], &cmd.quantity) || cmd.quantity <= 0) {
                return reject(cmd, "Quantity must be a positive integer");
            }
            if (!parse_integer(cmd.args[2], &cmd.price) || cmd.price <= 0) {
                return reject(cmd, "Price must be a positive integer");
            }
            cmd.instrument = cmd.args[0];
            break;
        }

        case CommandType::CANCEL: {
            if (cmd.args.size() != 1) {
                return reject(cmd, "Usage: CANCEL <order_id>");
            }
            if (!parse_integer(cmd.args[0], &cmd.order_id)) {
                return reject(cmd, "Order id must be a non-negative integer");
            }
            break;
        }

        case CommandType::SUBSCRIBE:
        case CommandType::UNSUBSCRIBE: {
            if (cmd.args.size() != 1) {
                return reject(cmd, std::string("Usage: ") + verb(cmd.type) +
                                       " <instrument>");
            }
            if (!is_valid_instrument(cmd.args[0])) {
                return reject(cmd, "Unknown instrument " + cmd.args[0]);
            }
            cmd.instrument = cmd.args[0];
            break;
        }

        case CommandType::QUIT: {
            if (!cmd.args.empty()) return reject(cmd, "Usage: QUIT");
            break;
        }

        case CommandType::UNKNOWN:
            return reject(cmd, "Unknown command");
    }

    cmd.valid = true;
    return cmd;
}

// --- Response formatting ---------------------------------------------------

static std::string join(const std::string& head, const std::string& instrument,
                        long quantity, long price) {
    return head + " " + instrument + " " + std::to_string(quantity) + " " +
           std::to_string(price) + "\n";
}

std::string ok() { return "OK\n"; }

std::string error(const std::string& reason) { return "ERROR " + reason + "\n"; }

std::string order_accepted(long order_id) {
    return "ORDER_ACCEPTED " + std::to_string(order_id) + "\n";
}

std::string order_cancelled(long order_id) {
    return "ORDER_CANCELLED " + std::to_string(order_id) + "\n";
}

std::string bought(const std::string& instrument, long quantity, long price) {
    return join("BOUGHT", instrument, quantity, price);
}

std::string sold(const std::string& instrument, long quantity, long price) {
    return join("SOLD", instrument, quantity, price);
}

std::string trade(const std::string& instrument, long quantity, long price) {
    return join("TRADE", instrument, quantity, price);
}

}  // namespace protocol
