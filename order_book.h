#pragma once

#include "models.h"

#include <map>
#include <queue>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <functional>

// ---- Added for RL Agents Environment (see rl_env.h) ----
// Emitted by the matching engine on every fill so an external
// observer (e.g. TradingEnv) can compute rewards without re-running
// the matcher itself.
struct TradeEvent {
    int    buy_order_id;
    int    sell_order_id;
    double price;
    int    quantity;
    long long timestamp;
};

using TradeCallback = std::function<void(const TradeEvent&)>;
// --------------------------------------------------------

class OrderBook {
private:
    // Bids: highest price first (descending). Best bid is at begin().
    std::map<double, std::queue<std::shared_ptr<Order>>, std::greater<double>> bids;

    // Asks: lowest price first (ascending). Best ask is at begin().
    std::map<double, std::queue<std::shared_ptr<Order>>> asks;

    // O(1) lookup by order_id for fast cancellation.
    std::unordered_map<int, std::shared_ptr<Order>> order_lookup;

    // One mutex protects bids, asks, order_lookup, and ltp together.
    // Simple and safe: any read/write of the book must hold this lock.
    std::mutex book_mutex;

    LtpTracker ltp;

    // Set by setTradeCallback; invoked from matchOrders while the
    // book_mutex is held. Added for RL Agents Environment.
    TradeCallback trade_cb;

    // Internal helpers assume book_mutex is already held.
    void matchOrders();
    void updateLtp(double trade_price, long long current_epoch_second);
    void printBidsNoLock();
    void printAsksNoLock();
    void printLtpNoLock();

public:
    void placeOrder(std::shared_ptr<Order> order);

    // Returns true if the order was still resting and got cancelled.
    // Returns false if it was already fully matched, never existed, or
    // was cancelled by someone else — i.e. the cancel "lost the race".
    bool cancelOrder(int order_id);

    // Consistency check: walks the book and verifies the live-order
    // count in the queues matches order_lookup.size(). Returns true if
    // the invariant holds.
    bool checkInvariants();

    // Display: each one locks the book briefly to take a snapshot.
    void displayBids();
    void displayAsks();
    void displayBook();
    void displayLtp();

    // ---- Added for RL Agents Environment ----
    // Register a callback fired on each trade. Called with the
    // book_mutex held, so the callback must be short and must not
    // call back into OrderBook (would deadlock).
    void setTradeCallback(TradeCallback cb);
};
