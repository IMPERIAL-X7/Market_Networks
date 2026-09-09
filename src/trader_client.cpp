// The Socket Exchange - Trader Client.
//
// usage: trader_client [host] [port] [username]
//
// Submits and cancels orders, and receives both direct responses and the
// asynchronous BOUGHT / SOLD notifications produced when a resting order is
// matched later by someone else's order.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "client_shell.hpp"

namespace {

void usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [host] [port] [username]\n"
                 "\n"
                 "  host, port   Exchange Server address (default 127.0.0.1 "
                 "5000)\n"
                 "  username     if given, LOGIN <username> is sent on "
                 "connect\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    ClientShellOptions options;
    options.host = "127.0.0.1";
    options.port = 5000;
    options.role_name = "Trader Client";
    options.help_text =
        "Commands: LOGIN <username> | BUY <instrument> <qty> <price> | "
        "SELL <instrument> <qty> <price> | CANCEL <order_id> | QUIT\n"
        "Instruments: JNST, IMCT.  Lines you type are sent as-is; server "
        "messages are printed with '<<<'.";

    if (argc > 4) {
        usage(argv[0]);
        return 2;
    }
    if (argc >= 2) options.host = argv[1];
    if (argc >= 3) {
        options.port = std::atoi(argv[2]);
        if (options.port <= 0 || options.port > 65535) {
            std::fprintf(stderr, "trader_client: invalid port '%s'\n", argv[2]);
            return 2;
        }
    }
    if (argc >= 4) {
        options.initial_commands.push_back(std::string("LOGIN ") + argv[3]);
    }

    return run_client_shell(options);
}
