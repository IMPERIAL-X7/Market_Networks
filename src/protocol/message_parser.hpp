#ifndef MESSAGE_PARSER_HPP
#define MESSAGE_PARSER_HPP

#include <string>
#include <vector>

namespace protocol {

// The two instruments the exchange supports.
extern const char* const kInstruments[2];
bool is_valid_instrument(const std::string& instrument);

enum class CommandType {
    UNKNOWN,
    LOGIN,
    BUY,
    SELL,
    CANCEL,
    SUBSCRIBE,
    UNSUBSCRIBE,
    QUIT,
};

// One parsed application-level message.
//
// `valid` reports only syntactic validity: the verb is known, the argument
// count is right, and numeric arguments are well-formed integers in range.
// Whether the sending client is *allowed* to issue the command is a separate,
// role-dependent question answered by the server.
struct ParsedCommand {
    CommandType type = CommandType::UNKNOWN;
    std::vector<std::string> args;

    bool valid = false;
    std::string error;  // reason text for an ERROR response

    // Decoded numeric fields, filled in for the commands that carry them.
    std::string instrument;
    long quantity = 0;
    long price = 0;
    long order_id = 0;
};

// Parses one line (without its terminating newline) into a command.
ParsedCommand parse(const std::string& line);

// Renders a command type back to its wire verb, for logging.
const char* verb(CommandType type);

// --- Response formatting -------------------------------------------------
// Every helper returns a complete, newline-terminated application message.

std::string ok();
std::string error(const std::string& reason);
std::string order_accepted(long order_id);
std::string order_cancelled(long order_id);
std::string bought(const std::string& instrument, long quantity, long price);
std::string sold(const std::string& instrument, long quantity, long price);
std::string trade(const std::string& instrument, long quantity, long price);

}  // namespace protocol

#endif
