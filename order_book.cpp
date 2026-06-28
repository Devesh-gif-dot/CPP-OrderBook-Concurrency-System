#include "order_book.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <vector>

void OrderBook::placeOrder(std::shared_ptr<Order> order) {
    std::lock_guard<std::mutex> lock(book_mutex);

    order_lookup[order->order_id] = order;

    if (order->type == OrderType::BID) {
        bids[order->price].push(order);
    } else {
        asks[order->price].push(order);
    }

    matchOrders();
}

bool OrderBook::cancelOrder(int order_id) {
    std::lock_guard<std::mutex> lock(book_mutex);

    auto it = order_lookup.find(order_id);
    if (it == order_lookup.end()) {
        // Lost the race: order was already fully matched (and removed
        // from lookup), already cancelled, or never existed.
        return false;
    }

    // Lazy deletion: zero the quantity. The matching engine will
    // pop it next time it surfaces. Avoids walking std::queue.
    it->second->quantity = 0;
    order_lookup.erase(it);
    return true;
}

bool OrderBook::checkInvariants() {
    std::lock_guard<std::mutex> lock(book_mutex);

    size_t live_in_queues = 0;
    for (auto& kv : bids) {
        std::queue<std::shared_ptr<Order>> snap = kv.second;
        while (!snap.empty()) {
            if (snap.front()->quantity > 0) ++live_in_queues;
            snap.pop();
        }
    }
    for (auto& kv : asks) {
        std::queue<std::shared_ptr<Order>> snap = kv.second;
        while (!snap.empty()) {
            if (snap.front()->quantity > 0) ++live_in_queues;
            snap.pop();
        }
    }
    return live_in_queues == order_lookup.size();
}

void OrderBook::matchOrders() {
    while (!bids.empty() && !asks.empty()) {
        auto bid_iter = bids.begin();
        auto ask_iter = asks.begin();

        if (bid_iter->first < ask_iter->first) {
            break;
        }

        auto& bid_queue = bid_iter->second;
        auto& ask_queue = ask_iter->second;

        if (bid_queue.empty()) { bids.erase(bid_iter); continue; }
        if (ask_queue.empty()) { asks.erase(ask_iter); continue; }

        auto bid_order = bid_queue.front();
        auto ask_order = ask_queue.front();

        if (bid_order->quantity == 0) {
            bid_queue.pop();
            order_lookup.erase(bid_order->order_id);
            if (bid_queue.empty()) bids.erase(bid_iter);
            continue;
        }
        if (ask_order->quantity == 0) {
            ask_queue.pop();
            order_lookup.erase(ask_order->order_id);
            if (ask_queue.empty()) asks.erase(ask_iter);
            continue;
        }

        // Older order (lower timestamp) sets the trade price.
        double trade_price = (bid_order->timestamp <= ask_order->timestamp)
                             ? bid_order->price
                             : ask_order->price;
        int trade_qty = std::min(bid_order->quantity, ask_order->quantity);

        long long current_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        updateLtp(trade_price, current_sec);

        std::cout << "[TRADE] qty=" << trade_qty
                  << " @ $" << std::fixed << std::setprecision(2) << trade_price
                  << "  buy#" << bid_order->order_id
                  << " <-> sell#" << ask_order->order_id << "\n";

        // Notify any registered observer (RL env). Added for RL Agents Environment.
        if (trade_cb) {
            TradeEvent ev{bid_order->order_id, ask_order->order_id,
                          trade_price, trade_qty, current_sec};
            trade_cb(ev);
        }

        bid_order->quantity -= trade_qty;
        ask_order->quantity -= trade_qty;

        if (bid_order->quantity == 0) {
            bid_queue.pop();
            order_lookup.erase(bid_order->order_id);
        }
        if (ask_order->quantity == 0) {
            ask_queue.pop();
            order_lookup.erase(ask_order->order_id);
        }

        if (bid_queue.empty()) bids.erase(bid_iter);
        if (ask_queue.empty()) asks.erase(ask_iter);
    }
}

void OrderBook::updateLtp(double trade_price, long long current_epoch_second) {
    if (!ltp.initialized || ltp.current_second != current_epoch_second) {
        ltp.current_second = current_epoch_second;
        ltp.min_price = trade_price;
        ltp.max_price = trade_price;
        ltp.initialized = true;
    } else {
        ltp.min_price = std::min(ltp.min_price, trade_price);
        ltp.max_price = std::max(ltp.max_price, trade_price);
    }
}

// ---- Display helpers (no lock; caller must hold book_mutex) ----

void OrderBook::printAsksNoLock() {
    std::cout << "\n--- ASKS (Sell Orders) ---\n";
    std::cout << std::setw(12) << "Price" << std::setw(12) << "Quantity" << "\n";
    std::cout << "------------------------\n";

    // Collect price levels with non-zero aggregated quantity.
    std::vector<std::pair<double, int>> levels;
    for (auto& kv : asks) {
        std::queue<std::shared_ptr<Order>> snapshot = kv.second;
        int total = 0;
        while (!snapshot.empty()) {
            total += snapshot.front()->quantity;
            snapshot.pop();
        }
        if (total > 0) levels.push_back({kv.first, total});
    }

    // Display highest ask at the top, best (lowest) ask just above bids.
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        std::cout << std::setw(12) << std::fixed << std::setprecision(2) << it->first
                  << std::setw(12) << it->second << "\n";
    }
    if (levels.empty()) std::cout << "  (empty)\n";
}

void OrderBook::printBidsNoLock() {
    std::cout << "\n--- BIDS (Buy Orders) ---\n";
    std::cout << std::setw(12) << "Price" << std::setw(12) << "Quantity" << "\n";
    std::cout << "------------------------\n";

    bool any = false;
    for (auto& kv : bids) {
        std::queue<std::shared_ptr<Order>> snapshot = kv.second;
        int total = 0;
        while (!snapshot.empty()) {
            total += snapshot.front()->quantity;
            snapshot.pop();
        }
        if (total > 0) {
            any = true;
            std::cout << std::setw(12) << std::fixed << std::setprecision(2) << kv.first
                      << std::setw(12) << total << "\n";
        }
    }
    if (!any) std::cout << "  (empty)\n";
}

void OrderBook::printLtpNoLock() {
    std::cout << "\n--- LTP TRACKER ---\n";
    if (ltp.initialized) {
        std::cout << "Second: " << ltp.current_second
                  << "  min=$" << std::fixed << std::setprecision(2) << ltp.min_price
                  << "  max=$" << ltp.max_price << "\n";
    } else {
        std::cout << "(no trades yet)\n";
    }
}

// ---- Public display methods (each take their own lock) ----

void OrderBook::displayBids() {
    std::lock_guard<std::mutex> lock(book_mutex);
    printBidsNoLock();
}

void OrderBook::displayAsks() {
    std::lock_guard<std::mutex> lock(book_mutex);
    printAsksNoLock();
}

void OrderBook::displayBook() {
    std::lock_guard<std::mutex> lock(book_mutex);
    printAsksNoLock();
    std::cout << "  ----- spread -----\n";
    printBidsNoLock();
    printLtpNoLock();
}

void OrderBook::displayLtp() {
    std::lock_guard<std::mutex> lock(book_mutex);
    printLtpNoLock();
}

// ---- Added for RL Agents Environment ----
void OrderBook::setTradeCallback(TradeCallback cb) {
    std::lock_guard<std::mutex> lock(book_mutex);
    trade_cb = std::move(cb);
}
