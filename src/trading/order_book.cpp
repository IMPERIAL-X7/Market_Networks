#include "order_book.hpp"
#include <algorithm>

int OrderBook::add_order(int client_fd, const std::string& inst, Side side, long qty, long price, std::vector<TradeExecution>& executions) {
    int order_id = next_order_id++;
    Order new_order{order_id, client_fd, inst, side, qty, price};

    auto& opposites = (side == Side::BUY) ? sell_orders[inst] : buy_orders[inst];
    
    auto it = opposites.begin();
    while (it != opposites.end() && new_order.quantity > 0) {
        // Matches ONLY if the price is exactly identical
        if (it->price == new_order.price) {
            long traded_qty = std::min(new_order.quantity, it->quantity);
            
            executions.push_back({
                (side == Side::BUY) ? client_fd : it->client_fd,
                (side == Side::BUY) ? it->client_fd : client_fd,
                inst, traded_qty, price
            });

            new_order.quantity -= traded_qty;
            it->quantity -= traded_qty;

            // Remove fully executed opposite orders
            if (it->quantity == 0) {
                order_pointers.erase(it->id);
                it = opposites.erase(it);
            } else {
                ++it; // Move to next order if the current one still has volume
            }
        } else {
            ++it;
        }
    }

    // If the new order wasn't fully filled, place it in the book
    if (new_order.quantity > 0) {
        auto& sames = (side == Side::BUY) ? buy_orders[inst] : sell_orders[inst];
        sames.push_back(new_order);
        
        // Grab an iterator to the newly inserted element
        auto inserted_it = sames.end();
        --inserted_it;
        order_pointers[order_id] = inserted_it;
    }

    return order_id;
}

bool OrderBook::cancel_order(int order_id) {
    auto it = order_pointers.find(order_id);
    if (it == order_pointers.end()) return false;
    
    auto list_it = it->second;
    const std::string& inst = list_it->instrument;
    
    if (list_it->side == Side::BUY) {
        buy_orders[inst].erase(list_it);
    } else {
        sell_orders[inst].erase(list_it);
    }
    
    order_pointers.erase(it);
    return true;
}