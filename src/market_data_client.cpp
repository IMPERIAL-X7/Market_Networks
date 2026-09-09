// The Socket Exchange - Market-Data Client.
//
// usage: market_data_client [host] [port] [instrument ...]
//
// Read-only with respect to trading: it subscribes to instruments and then
// receives TRADE updates that the Exchange Server pushes on its own, without
// the client asking again.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "client_shell.hpp"
#include "protocol/message_parser.hpp"

namespace {

void usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [host] [port] [instrument ...]\n"
                 "\n"
                 "  host, port   Exchange Server address (default 127.0.0.1 "
                 "5000)\n"
                 "  instrument   one or more of JNST, IMCT; each is "
                 "subscribed to on connect\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    ClientShellOptions options;
    options.host = "127.0.0.1";
    options.port = 5000;
    options.role_name = "Market-Data Client";
    options.help_text =
        "Commands: SUBSCRIBE <instrument> | UNSUBSCRIBE <instrument> | QUIT\n"
        "Instruments: JNST, IMCT.  TRADE updates arrive on their own; server "
        "messages are printed with '<<<'.";

    if (argc >= 2) options.host = argv[1];
    if (argc >= 3) {
        options.port = std::atoi(argv[2]);
        if (options.port <= 0 || options.port > 65535) {
            std::fprintf(stderr, "market_data_client: invalid port '%s'\n",
                         argv[2]);
            return 2;
        }
    }
    for (int i = 3; i < argc; ++i) {
        if (!protocol::is_valid_instrument(argv[i])) {
            std::fprintf(stderr, "market_data_client: unknown instrument '%s'\n",
                         argv[i]);
            usage(argv[0]);
            return 2;
        }
        options.initial_commands.push_back(std::string("SUBSCRIBE ") + argv[i]);
    }

    return run_client_shell(options);
}
