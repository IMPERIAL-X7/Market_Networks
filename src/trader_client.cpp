#include "network/tcp_client.hpp"
#include <iostream>
#include <thread>
#include <atomic>

std::atomic<bool> running{true};

void receive_loop(TCPClient* client) {
    while (running) {
        std::string msg = client->receive_message();
        if (msg.empty()) {
            std::cout << "Server disconnected.\n";
            running = false;
            break;
        }
        std::cout << msg; 
    }
}

int main() {
    TCPClient client("127.0.0.1", 5000);
    
    if (!client.connect_to_server()) {
        std::cerr << "Failed to connect to server.\n";
        return 1;
    }

    // Launch background thread to listen for async server broadcasts (BOUGHT, SOLD, TRADE)
    std::thread receiver(receive_loop, &client);

    std::string line;
    while (running && std::getline(std::cin, line)) {
        if (line == "QUIT") break;
        client.send_message(line + "\n");
    }

    running = false;
    client.disconnect();
    if (receiver.joinable()) {
        receiver.join();
    }
    
    return 0;
}