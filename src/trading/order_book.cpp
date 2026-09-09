#include "order_book.hpp"

#include <algorithm>

void OrderBook::erase_at(long order_id, const Location& location) {
    location.queue->erase(location.it);
    index_.erase(order_id);
}

long OrderBook::add_order(int owner_fd, const std::string& instrument,
                          Side side, long quantity, long price,
                          std::vector<TradeExecution>& executions) {
    const long order_id = next_order_id_++;
    long remaining = quantity;

    std::list<Order>& opposite =
        (side == Side::BUY) ? sells_[instrument] : buys_[instrument];

    auto it = opposite.begin();
    while (it != opposite.end() && remaining > 0) {
        if (it->price != price) {
            ++it;
            continue;
        }

        const long traded = std::min(remaining, it->quantity);

        TradeExecution execution;
        execution.buyer_fd = (side == Side::BUY) ? owner_fd : it->owner_fd;
        execution.seller_fd = (side == Side::BUY) ? it->owner_fd : owner_fd;
        execution.instrument = instrument;
        execution.quantity = traded;
        execution.price = price;
        executions.push_back(execution);

        remaining -= traded;
        it->quantity -= traded;

        if (it->quantity == 0) {
            index_.erase(it->id);
            it = opposite.erase(it);
        } else {
            ++it;  // partially filled resting order keeps its queue position
        }
    }

    // Whatever could not be matched rests in the book and stays cancellable.
    if (remaining > 0) {
        std::list<Order>& own =
            (side == Side::BUY) ? buys_[instrument] : sells_[instrument];
        Order order;
        order.id = order_id;
        order.owner_fd = owner_fd;
        order.instrument = instrument;
        order.side = side;
        order.quantity = remaining;
        order.price = price;
        own.push_back(order);

        Location location;
        location.queue = &own;
        location.it = std::prev(own.end());
        index_[order_id] = location;
    }

    return order_id;
}

bool OrderBook::cancel_order(long order_id, int requester_fd,
                             std::string& error) {
    auto it = index_.find(order_id);
    if (it == index_.end()) {
        // Covers unknown ids as well as orders that were fully executed or
        // already cancelled, which the protocol treats the same way.
        error = "No such order with unfilled quantity: " +
                std::to_string(order_id);
        return false;
    }

    if (it->second.it->owner_fd != requester_fd) {
        error = "Order " + std::to_string(order_id) +
                " belongs to another trader";
        return false;
    }

    erase_at(order_id, it->second);
    return true;
}

void OrderBook::remove_orders_of(int owner_fd) {
    for (auto it = index_.begin(); it != index_.end();) {
        if (it->second.it->owner_fd == owner_fd) {
            it->second.queue->erase(it->second.it);
            it = index_.erase(it);
        } else {
            ++it;
        }
    }
}
