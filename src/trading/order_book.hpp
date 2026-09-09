#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

#include <string>
#include <list>
#include <unordered_map>
#include <vector>

enum class Side { BUY, SELL };

// Represents an active order residing in the book
struct Order {
    int id;
    int client_fd; // Tracks the owner for routing BOUGHT/SOLD notifications
    std::string instrument;
    Side side;
    long quantity;
    long price;
};

// Represents a matched transaction to be broadcasted
struct TradeExecution {
    int buyer_fd;
    int seller_fd;
    std::string instrument;
    long quantity;
    long price;
};

class OrderBook {
private:
    int next_order_id = 0;
    
    // Separate chronological lists per instrument to avoid iterating irrelevant orders
    std::unordered_map<std::string, std::list<Order>> buy_orders;
    std::unordered_map<std::string, std::list<Order>> sell_orders;
    
    // Maps an order ID directly to its memory node in the list for O(1) removals
    std::unordered_map<int, std::list<Order>::iterator> order_pointers;

public:
    // Returns the generated order ID and populates the executions vector if matches occur
    int add_order(int client_fd, const std::string& instrument, Side side, long quantity, long price, std::vector<TradeExecution>& executions);
    
    // Returns true if successfully cancelled, false if not found
    bool cancel_order(int order_id);
};

#endif