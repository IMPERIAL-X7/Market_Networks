#include "client_shell.hpp"

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "network/tcp_client.hpp"

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

void print_messages(const std::vector<std::string>& messages) {
    for (const std::string& message : messages) {
        std::printf("<<< %s\n", message.c_str());
    }
    if (!messages.empty()) std::fflush(stdout);
}

}  // namespace

int run_client_shell(const ClientShellOptions& options) {
    signal(SIGPIPE, SIG_IGN);

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    TCPClient client(options.host, options.port);
    if (!client.connect_to_server()) {
        std::fprintf(stderr, "%s: %s\n", options.role_name,
                     client.error().c_str());
        return 1;
    }

    std::printf("%s connected to %s:%d.\n%s\n", options.role_name,
                options.host.c_str(), options.port, options.help_text.c_str());
    std::fflush(stdout);

    for (const std::string& command : options.initial_commands) {
        std::printf(">>> %s\n", command.c_str());
        std::fflush(stdout);
        if (!client.send_message(command + "\n")) {
            std::fprintf(stderr, "%s: %s\n", options.role_name,
                         client.error().c_str());
            return 1;
        }
    }

    std::string stdin_buffer;
    bool stdin_open = true;
    bool sent_quit = false;
    int status = 0;

    while (!g_stop) {
        struct pollfd watched[2];
        int count = 0;

        const int socket_slot = count;
        watched[count].fd = client.fd();
        watched[count].events = POLLIN;
        watched[count].revents = 0;
        ++count;

        int stdin_slot = -1;
        if (stdin_open) {
            stdin_slot = count;
            watched[count].fd = STDIN_FILENO;
            watched[count].events = POLLIN;
            watched[count].revents = 0;
            ++count;
        }

        if (::poll(watched, static_cast<nfds_t>(count), -1) < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "%s: poll() failed: %s\n", options.role_name,
                         std::strerror(errno));
            status = 1;
            break;
        }

        // Server messages first: an execution notification may arrive at any
        // time, independently of anything this client just sent.
        if (watched[socket_slot].revents & (POLLIN | POLLHUP | POLLERR)) {
            std::vector<std::string> messages;
            const bool alive = client.receive_messages(messages);
            print_messages(messages);
            if (!alive) {
                std::printf("Connection closed by the Exchange Server.\n");
                std::fflush(stdout);
                break;
            }
        }

        if (stdin_slot < 0) continue;
        if (!(watched[stdin_slot].revents & (POLLIN | POLLHUP))) continue;

        char buffer[4096];
        ssize_t received = ::read(STDIN_FILENO, buffer, sizeof(buffer));

        if (received < 0) {
            if (errno == EINTR) continue;
            stdin_open = false;
            continue;
        }

        if (received == 0) {
            // End of input: leave the exchange politely, then keep reading
            // until the server closes its side.
            stdin_open = false;
            if (!sent_quit) {
                sent_quit = true;
                client.send_message("QUIT\n");
                client.shutdown_writes();
            }
            continue;
        }

        stdin_buffer.append(buffer, static_cast<std::size_t>(received));

        std::size_t newline;
        while ((newline = stdin_buffer.find('\n')) != std::string::npos) {
            std::string line = stdin_buffer.substr(0, newline);
            stdin_buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (!client.send_message(line + "\n")) {
                std::fprintf(stderr, "%s: %s\n", options.role_name,
                             client.error().c_str());
                g_stop = 1;
                status = 1;
                break;
            }

            if (line == "QUIT") {
                // Half-close so the server sees a clean FIN while any final
                // messages it has already queued can still be read.
                sent_quit = true;
                stdin_open = false;
                client.shutdown_writes();
                break;
            }
        }
    }

    if (!sent_quit) client.shutdown_writes();
    client.disconnect();
    return status;
}
