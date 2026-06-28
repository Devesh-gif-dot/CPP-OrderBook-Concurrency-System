#pragma once

// =====================================================================
//  rl_env.h
//
//  Multi-agent RL training environment built on top of OrderBook.
//
//  This entire file (and rl_env.cpp) was added so the existing
//  OrderBook can be driven by reinforcement-learning agents without
//  touching the core matching engine. The only change made to the
//  original sources was a TradeCallback hook in order_book.{h,cpp},
//  used here to capture fills.
//
//  What this layer provides on top of OrderBook:
//    * Per-agent active-order limit (default 5) and balance enforcement
//    * Discrete price grid (e.g. {1,2,...,10}) chosen by the user
//    * Deterministic simulation clock (no wall-clock timestamps in
//      training)
//    * step(agent_id, action) / observe() / reset() API suitable for
//      a gymnasium-style wrapper
//    * Fill bookkeeping per agent -> realized PnL for rewards
//
//  Concurrency: the underlying OrderBook is already thread-safe, so
//  2-3 agents can step the env from separate threads. An additional
//  env_mutex serializes the env-side bookkeeping (accounts, limits)
//  so multi-agent updates stay consistent.
// =====================================================================

#include "order_book.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>

namespace rl_env {

// ---- Action space ---------------------------------------------------

enum class ActionType {
    HOLD,           // do nothing this step
    PLACE_BID,      // buy at prices[price_idx], size = quantity
    PLACE_ASK,      // sell at prices[price_idx], size = quantity
    CANCEL_OLDEST,  // cancel the agent's oldest still-resting order
};

struct Action {
    ActionType type;
    int        price_idx;  // index into the env's discrete price grid; ignored for HOLD/CANCEL
    int        quantity;   // ignored for HOLD/CANCEL
};

// ---- Per-step return -------------------------------------------------

struct TradeFill {
    int    order_id;              // the agent's own order that was filled
    int    counterparty_order_id;
    double price;
    int    quantity;
    bool   is_buy;                // true if the agent's filled side was a bid
};

// ---- Per-agent bookkeeping ------------------------------------------

struct AgentState {
    int                  agent_id;
    double               balance;            // cash
    int                  inventory;          // net long position (units)
    double               realized_pnl;       // cumulative realized PnL
    std::vector<int>     active_order_ids;   // currently resting; insertion order
};

// ---- Observation passed to the policy network ------------------------

struct BookObservation {
    std::vector<int> bid_depth;  // size = num discrete prices, units resting at each level
    std::vector<int> ask_depth;
    double           best_bid;   // 0.0 if no bids
    double           best_ask;   // 0.0 if no asks
    long long        sim_step;
};

// ---- Result of step() ------------------------------------------------

struct StepResult {
    bool                   accepted;     // false if action violated limit / balance
    std::string            reject_reason;
    std::vector<TradeFill> fills;        // fills triggered by this action (may be empty)
};

// =====================================================================
//  TradingEnv
// =====================================================================
class TradingEnv {
public:
    // discrete_prices: e.g. {1.0, 2.0, ..., 10.0} for a 10-level grid.
    // max_active_orders_per_agent: training limit (the "5" from the
    // user's question — pass whatever you want here, no need to
    // subclass OrderBook).
    TradingEnv(int n_agents,
               int max_active_orders_per_agent,
               std::vector<double> discrete_prices,
               double initial_balance);

    // Reset everything: book is empty, accounts re-funded, sim clock 0.
    void reset();

    // Apply an action for one agent and run matching. Returns the
    // fills (for both sides involving this agent), plus an accept/
    // reject flag. Rejections happen when:
    //   * agent_id is unknown
    //   * PLACE_*: agent has max_active_orders open
    //   * PLACE_BID: balance < price * quantity
    //   * PLACE_ASK: inventory < quantity (no shorting in this default)
    //   * CANCEL_OLDEST: agent has nothing to cancel
    //   * price_idx out of range
    //   * quantity <= 0
    StepResult step(int agent_id, const Action& action);

    // Global book snapshot. Same for every agent.
    BookObservation observe() const;

    // Per-agent snapshot.
    AgentState getAgentState(int agent_id) const;

    long long simStep() const { return sim_step_.load(); }

    const std::vector<double>& priceGrid() const { return prices_; }
    int maxActiveOrders() const { return max_orders_; }

private:
    // Trade callback fired by OrderBook. Updates whichever agent(s)
    // own the filled orders.
    void onTrade(const TradeEvent& ev);

    int n_agents_;
    int max_orders_;
    double initial_balance_;
    std::vector<double> prices_;

    std::unique_ptr<OrderBook> book_;

    // order_id -> agent_id, for routing fills back to the right account.
    std::unordered_map<int, int> order_owner_;

    // Per-agent state, keyed by agent_id (0..n_agents-1).
    std::unordered_map<int, AgentState> agents_;

    // Fills produced during the most recent step(), keyed by agent_id.
    // Populated by onTrade, drained by step before returning.
    std::unordered_map<int, std::vector<TradeFill>> pending_fills_;

    std::atomic<int>       next_order_id_;
    std::atomic<long long> sim_step_;

    mutable std::mutex env_mutex_;
};

}  // namespace rl_env
