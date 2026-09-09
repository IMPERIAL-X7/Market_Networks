#include "network/tcp_server.hpp"
#include "network/client_session.hpp"
#include "protocol/message_parser.hpp"
#include "trading/order_book.hpp"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>

// Helper to safely send a string over a socket
void send_msg(int fd, const std::string& msg) {
    send(fd, msg.c_str(), msg.length(), 0);
}

// Helper to clean up state when a client disconnects
void handle_disconnect(int fd, std::unordered_map<int, ClientSession>& sessions, 
                       std::unordered_map<std::string, std::unordered_set<int>>& subscribers) {
    auto it = sessions.find(fd);
    if (it != sessions.end()) {
        for (const std::string& inst : it->second.subscriptions) {
            subscribers[inst].erase(fd);
        }
        sessions.erase(it);
    }
    close(fd);
    std::cout << "Client FD " << fd << " disconnected.\n";
}

int main() {
    int port = 5000;
    TCPServer server(port);
    server.start();

    std::vector<struct pollfd> fds;
    fds.push_back({server.get_server_fd(), POLLIN, 0});
    
    std::unordered_map<int, ClientSession> sessions;
    std::unordered_map<std::string, std::unordered_set<int>> subscribers;
    OrderBook order_book;

    std::cout << "Exchange Server running. Waiting for events...\n";

    while (true) {
        int ready = poll(fds.data(), fds.size(), -1);
        if (ready < 0) break;

        for (int i = fds.size() - 1; i >= 0; --i) {
            if (fds[i].revents & POLLIN) {
                if (fds[i].fd == server.get_server_fd()) {
                    int client_fd = server.accept_connection();
                    if (client_fd >= 0) {
                        std::cout << "New client connected: FD " << client_fd << "\n";
                        fds.push_back({client_fd, POLLIN, 0});
                        sessions[client_fd] = ClientSession(client_fd);
                    }
                } else {
                    int client_fd = fds[i].fd;
                    char raw_buffer[1024];
                    int bytes_received = recv(client_fd, raw_buffer, sizeof(raw_buffer), 0);

                    if (bytes_received <= 0) {
                        handle_disconnect(client_fd, sessions, subscribers);
                        fds.erase(fds.begin() + i);
                    } else {
                        ClientSession& session = sessions[client_fd];
                        session.buffer.append(raw_buffer, bytes_received);
                        
                        size_t pos;
                        while ((pos = session.buffer.find('\n')) != std::string::npos) {
                            std::string message = session.buffer.substr(0, pos);
                            session.buffer.erase(0, pos + 1);
                            
                            ParsedCommand cmd = ProtocolHandler::parse(message);
                            if (!cmd.valid) {
                                send_msg(client_fd, "ERROR " + cmd.error_msg + "\n");
                                continue;
                            }

                            std::string response;
                            bool disconnect = false;
                            
                            // Validate state transitions (LOGIN, SUBSCRIBE)
                            if (ProtocolHandler::handle_session_command(client_fd, cmd, sessions, subscribers, response, disconnect)) {
                                if (disconnect) {
                                    handle_disconnect(client_fd, sessions, subscribers);
                                    fds.erase(fds.begin() + i);
                                    break; // Stop parsing buffer if client quit
                                } 
                                else if (cmd.type == "BUY" || cmd.type == "SELL") {
                                    Side side = (cmd.type == "BUY") ? Side::BUY : Side::SELL;
                                    long qty = std::stol(cmd.args[1]);
                                    long price = std::stol(cmd.args[2]);
                                    std::vector<TradeExecution> execs;
                                    
                                    // 1. Generate Order ID & execute matches
                                    int order_id = order_book.add_order(client_fd, cmd.args[0], side, qty, price, execs);
                                    
                                    // 2. Send immediate ORDER_ACCEPTED acknowledgment
                                    send_msg(client_fd, "ORDER_ACCEPTED " + std::to_string(order_id) + "\n");

                                    // 3. Dispatch asynchronous execution notifications
                                    for (const auto& exec : execs) {
                                        send_msg(exec.buyer_fd, "BOUGHT " + exec.instrument + " " + std::to_string(exec.quantity) + " " + std::to_string(exec.price) + "\n");
                                        send_msg(exec.seller_fd, "SOLD " + exec.instrument + " " + std::to_string(exec.quantity) + " " + std::to_string(exec.price) + "\n");
                                        
                                        std::string trade_msg = "TRADE " + exec.instrument + " " + std::to_string(exec.quantity) + " " + std::to_string(exec.price) + "\n";
                                        for (int sub_fd : subscribers[exec.instrument]) {
                                            send_msg(sub_fd, trade_msg);
                                        }
                                    }
                                } 
                                else if (cmd.type == "CANCEL") {
                                    int order_id = std::stoi(cmd.args[0]);
                                    if (order_book.cancel_order(order_id)) {
                                        send_msg(client_fd, "ORDER_CANCELLED " + std::to_string(order_id) + "\n");
                                    } else {
                                        send_msg(client_fd, "ERROR Invalid CANCEL order id\n");
                                    }
                                } 
                                else {
                                    // Handles OK response for LOGIN, SUBSCRIBE, UNSUBSCRIBE
                                    send_msg(client_fd, response);
                                }
                            } else {
                                send_msg(client_fd, response); // Send role violation errors
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}