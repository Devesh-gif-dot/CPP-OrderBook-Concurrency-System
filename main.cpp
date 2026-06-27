#include "order_book.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <atomic>

static long long nowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::shared_ptr<Order> makeOrder(int id, int acct, OrderType t,
                                        double price, int qty) {
    return std::make_shared<Order>(id, acct, t, price, qty, nowMicros());
}

int main() {
    OrderBook book;

    std::cout << "============================================\n";
    std::cout << "        ORDER BOOK SYSTEM - DEMO\n";
    std::cout << "============================================\n";

    // ---- Step 1: Build initial book (no crosses yet) ----
    std::cout << "\n[1] Placing initial bids and asks (no crossing)...\n";

    book.placeOrder(makeOrder(1, 101, OrderType::BID, 100.00, 10));
    book.placeOrder(makeOrder(2, 102, OrderType::BID,  99.50, 20));
    book.placeOrder(makeOrder(3, 103, OrderType::BID, 100.00, 15));
    book.placeOrder(makeOrder(4, 104, OrderType::BID,  99.00,  5));

    book.placeOrder(makeOrder(5, 105, OrderType::ASK, 101.00,  8));
    book.placeOrder(makeOrder(6, 106, OrderType::ASK, 101.50, 12));
    book.placeOrder(makeOrder(7, 107, OrderType::ASK, 102.00, 10));
    book.placeOrder(makeOrder(8, 108, OrderType::ASK, 101.00,  7));

    book.displayBook();

    // ---- Step 2: Place a crossing order → matching engine fires ----
    std::cout << "\n[2] Placing crossing BUY 20 @ $101.00 (should match asks at 101)...\n";
    book.placeOrder(makeOrder(9, 109, OrderType::BID, 101.00, 20));
    book.displayBook();

    // ---- Step 3: Cancel a resting order (lazy deletion) ----
    std::cout << "\n[3] Cancelling order #6 (ASK 12 @ $101.50)...\n";
    book.cancelOrder(6);
    book.displayBook();

    // ---- Step 4: Concurrency demo with 3 threads ----
    // The book's mutex serializes all placeOrder calls, so the
    // matching engine sees a consistent book even though three
    // threads are pushing orders at the same time.
    std::cout << "\n============================================\n";
    std::cout << "  [4] CONCURRENCY DEMO - 3 threads placing orders\n";
    std::cout << "============================================\n";

    std::atomic<int> next_id{200};

    auto buyer_thread = [&]() {
        for (int i = 0; i < 4; ++i) {
            int id = next_id.fetch_add(1);
            book.placeOrder(makeOrder(id, 201, OrderType::BID,
                                      99.80 + i * 0.10, 5));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    auto seller_thread = [&]() {
        for (int i = 0; i < 4; ++i) {
            int id = next_id.fetch_add(1);
            book.placeOrder(makeOrder(id, 202, OrderType::ASK,
                                      101.20 + i * 0.10, 4));
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    };

    auto aggressor_thread = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        for (int i = 0; i < 3; ++i) {
            int id = next_id.fetch_add(1);
            // A buyer willing to pay up — should cross some asks.
            book.placeOrder(makeOrder(id, 203, OrderType::BID,
                                      101.40, 6));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };

    std::vector<std::thread> threads;
    threads.emplace_back(buyer_thread);
    threads.emplace_back(seller_thread);
    threads.emplace_back(aggressor_thread);

    for (auto& t : threads) t.join();

    std::cout << "\n--- Final Book State ---";
    book.displayBook();

    // ---- Step 5: Race - cancel orders while matching is happening ----
    // We pre-populate 60 resting asks at $50, then race:
    //   * one "matcher" thread fires 12 crossing bids (each eats 5 asks)
    //   * two "canceller" threads try to cancel those exact asks
    // Outcomes:
    //   - cancel wins:  it grabbed the mutex first  -> returns true
    //   - cancel loses: order was already matched   -> returns false
    // The mutex guarantees no torn state and no double-fills.
    std::cout << "\n============================================\n";
    std::cout << "  [5] RACE: cancellation vs. matching\n";
    std::cout << "============================================\n";

    OrderBook race_book;
    const int RESTING_COUNT = 60;
    const int CROSS_BIDS    = 12;
    const int CROSS_QTY     = 5;

    // Pre-fill the book with 60 resting asks at the same price.
    for (int i = 0; i < RESTING_COUNT; ++i) {
        race_book.placeOrder(makeOrder(5000 + i, 301,
                                       OrderType::ASK, 50.00, 1));
    }
    std::cout << "Pre-populated " << RESTING_COUNT
              << " resting asks (qty 1 each @ $50.00)\n";

    std::atomic<int> trades_attempted{0};
    std::atomic<int> cancel_won{0};
    std::atomic<int> cancel_lost{0};

    auto matcher = [&]() {
        for (int i = 0; i < CROSS_BIDS; ++i) {
            race_book.placeOrder(makeOrder(7000 + i, 302,
                                           OrderType::BID, 50.00, CROSS_QTY));
            trades_attempted += CROSS_QTY;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    };

    auto canceller = [&](int start, int end) {
        for (int id = 5000 + start; id < 5000 + end; ++id) {
            if (race_book.cancelOrder(id)) {
                cancel_won.fetch_add(1);
            } else {
                cancel_lost.fetch_add(1);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    };

    std::thread t_match(matcher);
    std::thread t_cancel1(canceller,  0, 30);
    std::thread t_cancel2(canceller, 30, 60);

    t_match.join();
    t_cancel1.join();
    t_cancel2.join();

    std::cout << "\n--- Race results ---\n";
    std::cout << "Buy qty submitted:    " << trades_attempted.load() << "\n";
    std::cout << "Cancel attempts:      " << (cancel_won + cancel_lost) << "\n";
    std::cout << "  Cancel WON race:    " << cancel_won.load()
              << "  (order was still resting)\n";
    std::cout << "  Cancel LOST race:   " << cancel_lost.load()
              << "  (order already matched or gone)\n";

    bool ok = race_book.checkInvariants();
    std::cout << "Invariant check:      " << (ok ? "PASS" : "FAIL")
              << "  (live orders in queues == order_lookup.size())\n";

    std::cout << "\n--- Race book final state ---";
    race_book.displayBook();

    std::cout << "\n=== Demo complete ===\n";
    return 0;
}
