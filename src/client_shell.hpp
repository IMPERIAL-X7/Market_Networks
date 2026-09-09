#ifndef CLIENT_SHELL_HPP
#define CLIENT_SHELL_HPP

#include <string>
#include <vector>

// The interactive loop shared by the Trader Client and the Market-Data Client.
//
// Both clients need exactly the same thing from the network: send the lines the
// user types, and print server messages the moment they arrive, whether or not
// the user has asked for anything. A single poll() over standard input and the
// socket gives that without threads.
struct ClientShellOptions {
    std::string host;
    int port = 0;
    const char* role_name = "Client";

    // Commands sent automatically right after connecting, e.g. the LOGIN
    // derived from argv or one SUBSCRIBE per requested instrument.
    std::vector<std::string> initial_commands;

    // Printed after connecting, listing the commands this client type accepts.
    std::string help_text;
};

// Returns a process exit status.
int run_client_shell(const ClientShellOptions& options);

#endif
