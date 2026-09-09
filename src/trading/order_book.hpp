#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

enum class Side { BUY, SELL };

// A resting order: one that has been accepted and still has unfilled quantity.
struct Order {
    long id = 0;
    int owner_fd = -1;  // routes BOUGHT / SOLD back to the submitting trader
    std::string instrument;
    Side side = Side::BUY;
    long quantity = 0;  // remaining, always > 0 while in the book
    long price = 0;
};

// One executed match, to be turned into BOUGHT / SOLD / TRADE messages.
struct TradeExecution {
    int buyer_fd = -1;
    int seller_fd = -1;
    std::string instrument;
    long quantity = 0;
    long price = 0;
};

// The exchange's order book.
//
// Matching rule for this assignment: two orders match only if they are for the
// same instrument, on opposite sides, and at exactly the same price. Resting
// orders at a given price are matched in submission order (FIFO), and the
// traded quantity is the smaller of the two remaining quantities.
class OrderBook {
public:
    // Assigns the next order id, matches the incoming order against the book,
    // appends any resulting executions, and rests the remainder. Returns the
    // new order's id.
    long add_order(int owner_fd, const std::string& instrument, Side side,
                   long quantity, long price,
                   std::vector<TradeExecution>& executions);

    // Cancels a resting order. Fails if the id is unknown, if the order is no
    // longer resting (fully executed or already cancelled), or if it belongs
    // to a different trader.
    bool cancel_order(long order_id, int requester_fd, std::string& error);

    // Drops every resting order owned by a disconnected client, so that a
    // reused descriptor number can never inherit a previous client's orders.
    void remove_orders_of(int owner_fd);

    std::size_t resting_order_count() const { return index_.size(); }

private:
    struct Location {
        std::list<Order>* queue;
        std::list<Order>::iterator it;
    };

    long next_order_id_ = 1;

    // Per-instrument, per-side FIFO queues. Keeping them separate means a new
    // order only scans orders it could actually match against.
    std::unordered_map<std::string, std::list<Order>> buys_;
    std::unordered_map<std::string, std::list<Order>> sells_;

    // order id -> position in its queue, for O(1) cancellation.
    std::unordered_map<long, Location> index_;

    void erase_at(long order_id, const Location& location);
};

#endif
