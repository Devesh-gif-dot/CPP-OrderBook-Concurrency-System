#pragma once

#include <unordered_set>

enum class OrderType {
    BID,
    ASK
};

struct Order {
    int order_id;
    int account_id;
    OrderType type;
    double price;
    int quantity;
    long long timestamp;

    Order(int oid, int aid, OrderType t, double p, int q, long long ts)
        : order_id(oid), account_id(aid), type(t),
          price(p), quantity(q), timestamp(ts) {}
};

class Account {
public:
    int account_id;
    double balance;
    std::unordered_set<int> active_order_ids;

    Account(int id, double initial_balance)
        : account_id(id), balance(initial_balance) {}
};

struct LtpTracker {
    long long current_second;
    double min_price;
    double max_price;
    bool initialized;

    LtpTracker()
        : current_second(0), min_price(0.0),
          max_price(0.0), initialized(false) {}
};
