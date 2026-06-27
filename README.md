# Order Book System

A multi-threaded limit order book and matching engine in modern C++. Supports placing bids/asks, price-time priority matching, lazy cancellation, last-traded-price (LTP) tracking, and concurrent access from multiple threads.

## Build & Run

```bash
make
./order_book_demo
```

## Files

| File | Purpose |
|------|---------|
| [models.h](models.h) | Data types: `Order`, `Account`, `LtpTracker`, `OrderType` |
| [order_book.h](order_book.h) | `OrderBook` class interface |
| [order_book.cpp](order_book.cpp) | Matching engine, cancellation, display |
| [main.cpp](main.cpp) | Demo: single-threaded, concurrent, and race scenarios |

## Core Design

- **Bids** stored in `std::map<double, queue, std::greater<>>` — highest price first.
- **Asks** stored in `std::map<double, queue>` — lowest price first.
- **`order_lookup`** — `unordered_map<int, shared_ptr<Order>>` for O(1) cancel.
- **Lazy cancellation** — cancelled orders are zeroed and skipped during matching, avoiding queue traversal.
- **One mutex** (`book_mutex`) serializes all reads/writes; the matching engine sees a consistent snapshot.
- **Price-time priority** — older order's price sets the trade price when prices cross.

---

## OOP Concepts Demonstrated

### 1. Class & Object
A **class** is a blueprint; an **object** is its instance.
`OrderBook` (class) is instantiated as `book` and `race_book` in `main.cpp` — each is an independent object with its own bids, asks, and mutex.

### 2. Encapsulation
Binding data and the methods that operate on it inside one unit, hiding internals.
`OrderBook` keeps `bids`, `asks`, `order_lookup`, `book_mutex`, and `ltp` as `private` members. Outside code can only touch them through `placeOrder`, `cancelOrder`, `displayBook`, etc. — internal invariants stay safe.

### 3. Abstraction
Exposing *what* an object does, hiding *how*.
A user calls `book.placeOrder(order)` without knowing about queue layout, mutex locking, or the matching loop. The header [order_book.h](order_book.h) is the abstract contract; [order_book.cpp](order_book.cpp) is the hidden implementation.

### 4. Access Specifiers
`private:` — `bids`, `asks`, `matchOrders()`, `updateLtp()` — only the class touches them.
`public:` — `placeOrder`, `cancelOrder`, `displayBook`, `checkInvariants` — the user-facing API.

### 5. Constructors
Special methods that initialize objects.
`Order(int oid, int aid, OrderType t, double p, int q, long long ts)` uses a **member initializer list** to set all six fields in one shot. `Account` and `LtpTracker` likewise initialize through constructors.

### 6. Inheritance (not used — by design)
The project intentionally avoids inheritance: there is no `BidOrder : Order` hierarchy. Instead, `OrderType` (an `enum class`) tags whether an order is a bid or ask. This is a deliberate **composition over inheritance** choice — simpler, no virtual dispatch cost in the hot matching path.

### 7. Polymorphism
- **Compile-time (templates)**: `std::map`, `std::queue`, `std::shared_ptr`, `std::unordered_map`, `std::lock_guard` all work on any type — one piece of generic code, many concrete types.
- **Operator overloading**: `std::greater<double>` is passed as the bids map's comparator, overloading the ordering relation so the map sorts in descending order.

### 8. `enum class` (Strongly-Typed Enum)
`enum class OrderType { BID, ASK };` — scoped, type-safe. You must write `OrderType::BID`, and it won't implicitly convert to `int`, eliminating a whole category of bugs.

### 9. `struct` vs `class`
`struct Order` and `struct LtpTracker` are public-by-default lightweight data carriers. `class OrderBook` and `class Account` use the keyword that conveys "this has invariants to protect" — purely a stylistic but semantically meaningful distinction.

### 10. Composition (HAS-A relationship)
`OrderBook` **has** a `LtpTracker`, **has** maps of `Order` pointers, **has** a `mutex`. `Account` **has** an `unordered_set` of active order IDs. Objects are built by combining smaller objects rather than extending a base class.

### 11. RAII (Resource Acquisition Is Initialization)
A C++ idiom where resource lifetime is bound to object lifetime.
`std::lock_guard<std::mutex> lock(book_mutex);` acquires the mutex in its constructor and releases it in its destructor — locks can never leak, even if an exception fires.
`std::shared_ptr<Order>` does the same for heap memory: reference-counted, auto-deleted.

### 12. Smart Pointers (Ownership Semantics)
`std::shared_ptr<Order>` lets the order live as long as either the price queue or `order_lookup` still references it. No `new`/`delete`, no leaks. Created via `std::make_shared<Order>(...)` in `main.cpp`'s `makeOrder()` helper.

### 13. `const` Correctness & References
Internal helpers iterate with `auto& kv : bids` — references avoid copying entire queues. Read-only access patterns use `const` references where mutation isn't needed.

### 14. The `this` Pointer (implicit)
Inside `OrderBook::placeOrder`, references to `bids`, `asks`, `order_lookup` implicitly resolve through `this->`, distinguishing each object's own state.

### 15. Separation of Interface and Implementation
`.h` declares the API; `.cpp` defines it. Users `#include "order_book.h"` only — implementation details can change without recompiling clients (modulo the private members, which is a known C++ trade-off).

### 16. Modularity
Three translation units ([models.h](models.h), [order_book.cpp](order_book.cpp), [main.cpp](main.cpp)) compiled separately and linked. Each has a single, focused responsibility.

### 17. Member Functions: Public API vs Private Helpers
Public: `placeOrder`, `cancelOrder`, `displayBook`, `checkInvariants`.
Private helpers (assume the lock is already held): `matchOrders()`, `updateLtp()`, `printBidsNoLock()`, `printAsksNoLock()`, `printLtpNoLock()` — this naming convention documents the locking contract.

### 18. Lambdas & Functors (function objects)
`buyer_thread`, `seller_thread`, `aggressor_thread`, `matcher`, `canceller` in [main.cpp](main.cpp) are lambdas — anonymous function objects that capture local state by reference (`[&]`). Each `std::thread` is constructed by passing one of these callable objects.

### 19. Thread Safety as a Class Invariant
The mutex isn't bolted on; it's a class member, enforced by every public method. Concurrency safety becomes a property of the type, not of the caller.

### 20. Single Responsibility Principle
`Order` = pure data. `OrderBook` = matching & state. `LtpTracker` = price aggregation per second. `Account` = user balance + order tracking. Each type does one thing.

---

## Demo Scenarios in `main.cpp`

1. **Initial book** — places non-crossing bids/asks to populate price levels.
2. **Crossing order** — a buy at $101 sweeps the resting asks at $101; matching engine prints trades.
3. **Cancellation** — order #6 cancelled via lazy deletion.
4. **Concurrency** — three threads (buyer, seller, aggressor) place orders simultaneously; the mutex keeps the book consistent.
5. **Race: cancel vs. match** — 60 resting asks, one matcher thread, two canceller threads. Each cancel either *wins* (still resting) or *loses* (already matched). `checkInvariants()` verifies live-order count consistency at the end.
