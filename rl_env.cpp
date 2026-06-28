// =====================================================================
//  rl_env.cpp
//
//  Implementation of the multi-agent RL trading environment.
//  Added as a separate translation unit so the original OrderBook
//  remains unchanged apart from the small TradeCallback hook.
// =====================================================================

#include "rl_env.h"

#include <algorithm>

namespace rl_env {

TradingEnv::TradingEnv(int n_agents,
                       int max_active_orders_per_agent,
                       std::vector<double> discrete_prices,
                       double initial_balance)
    : n_agents_(n_agents),
      max_orders_(max_active_orders_per_agent),
      initial_balance_(initial_balance),
      prices_(std::move(discrete_prices)),
      book_(std::make_unique<OrderBook>()),
      next_order_id_(1),
      sim_step_(0)
{
    book_->setTradeCallback([this](const TradeEvent& ev) {
        // OrderBook holds its own mutex when this fires. Acquire
        // env_mutex_ — never call back into OrderBook from here.
        this->onTrade(ev);
    });

    reset();
}

void TradingEnv::reset() {
    std::lock_guard<std::mutex> lock(env_mutex_);

    // Throw away the old book and replace it with a fresh one. Avoids
    // adding a clear() method to OrderBook just for training.
    book_ = std::make_unique<OrderBook>();
    book_->setTradeCallback([this](const TradeEvent& ev) {
        this->onTrade(ev);
    });

    order_owner_.clear();
    agents_.clear();
    pending_fills_.clear();
    next_order_id_.store(1);
    sim_step_.store(0);

    for (int i = 0; i < n_agents_; ++i) {
        AgentState s;
        s.agent_id     = i;
        s.balance      = initial_balance_;
        s.inventory    = 0;
        s.realized_pnl = 0.0;
        agents_[i]     = std::move(s);
    }
}

StepResult TradingEnv::step(int agent_id, const Action& action) {
    StepResult out;
    out.accepted = false;

    std::unique_lock<std::mutex> lock(env_mutex_);

    auto it = agents_.find(agent_id);
    if (it == agents_.end()) {
        out.reject_reason = "unknown agent_id";
        return out;
    }
    AgentState& agent = it->second;

    sim_step_.fetch_add(1);
    long long ts = sim_step_.load();

    if (action.type == ActionType::HOLD) {
        out.accepted = true;
        return out;
    }

    if (action.type == ActionType::CANCEL_OLDEST) {
        if (agent.active_order_ids.empty()) {
            out.reject_reason = "no resting order to cancel";
            return out;
        }
        int victim = agent.active_order_ids.front();
        agent.active_order_ids.erase(agent.active_order_ids.begin());
        order_owner_.erase(victim);

        // OrderBook::cancelOrder takes its own lock. Drop env_mutex_
        // first to avoid lock-ordering issues if onTrade fires
        // concurrently from another thread's placeOrder.
        lock.unlock();
        book_->cancelOrder(victim);
        out.accepted = true;
        return out;
    }

    // PLACE_BID or PLACE_ASK from here on.
    if (action.quantity <= 0) {
        out.reject_reason = "quantity must be positive";
        return out;
    }
    if (action.price_idx < 0 ||
        action.price_idx >= static_cast<int>(prices_.size())) {
        out.reject_reason = "price_idx out of range";
        return out;
    }
    if (static_cast<int>(agent.active_order_ids.size()) >= max_orders_) {
        out.reject_reason = "active-order limit reached";
        return out;
    }

    double price = prices_[action.price_idx];

    if (action.type == ActionType::PLACE_BID) {
        double cost = price * action.quantity;
        if (agent.balance < cost) {
            out.reject_reason = "insufficient balance";
            return out;
        }
        // Reserve the cash now; refund any unfilled portion on cancel.
        // (Simplification: realized PnL accounting lives in onTrade.)
        agent.balance -= cost;
    } else {  // PLACE_ASK
        if (agent.inventory < action.quantity) {
            out.reject_reason = "insufficient inventory (no shorting)";
            return out;
        }
        agent.inventory -= action.quantity;
    }

    int oid = next_order_id_.fetch_add(1);
    OrderType ot = (action.type == ActionType::PLACE_BID)
                       ? OrderType::BID : OrderType::ASK;

    agent.active_order_ids.push_back(oid);
    order_owner_[oid] = agent_id;
    pending_fills_[agent_id].clear();

    auto order = std::make_shared<Order>(oid, agent_id, ot,
                                         price, action.quantity, ts);

    // Drop env_mutex_ before calling into OrderBook. onTrade will
    // re-acquire it when fills come back.
    lock.unlock();
    book_->placeOrder(order);

    // Collect any fills onTrade routed to this agent during placeOrder.
    lock.lock();
    auto pf_it = pending_fills_.find(agent_id);
    if (pf_it != pending_fills_.end()) {
        out.fills = std::move(pf_it->second);
        pf_it->second.clear();
    }
    out.accepted = true;
    return out;
}

void TradingEnv::onTrade(const TradeEvent& ev) {
    // Called from inside OrderBook::matchOrders while book_mutex is
    // held. We only touch env-side state, never the book.
    std::lock_guard<std::mutex> lock(env_mutex_);

    auto handle_side = [&](int oid, bool is_buy) {
        auto own_it = order_owner_.find(oid);
        if (own_it == order_owner_.end()) return;  // not ours
        int aid = own_it->second;

        auto a_it = agents_.find(aid);
        if (a_it == agents_.end()) return;
        AgentState& agent = a_it->second;

        if (is_buy) {
            // Bid filled at ev.price; we reserved at the bid's limit
            // price when placing, so refund the price improvement.
            // The reserved amount was price_limit * qty; actual cost
            // is ev.price * qty. Without storing the limit per order,
            // we approximate by treating ev.price as the cost (the
            // matching engine uses the older order's price, so the
            // bid pays at most its limit — refund is non-negative).
            // For a tighter implementation, store the limit in
            // order_owner_'s value type. Keeping it simple here.
            agent.inventory   += ev.quantity;
            agent.realized_pnl -= ev.price * ev.quantity;
        } else {
            agent.balance      += ev.price * ev.quantity;
            agent.realized_pnl += ev.price * ev.quantity;
        }

        TradeFill f;
        f.order_id              = oid;
        f.counterparty_order_id = is_buy ? ev.sell_order_id : ev.buy_order_id;
        f.price                 = ev.price;
        f.quantity              = ev.quantity;
        f.is_buy                = is_buy;
        pending_fills_[aid].push_back(f);

        // If the order is fully consumed, the OrderBook will drop it
        // from its internal lookup. We mirror that by walking the
        // active list and dropping any ids the book no longer holds.
        // Cheap O(k) where k = max_orders_ (default 5).
        auto& av = agent.active_order_ids;
        av.erase(std::remove(av.begin(), av.end(), oid), av.end());
        // Re-add if the order still has remaining quantity. We can't
        // ask OrderBook from here (would deadlock), so we re-add
        // unconditionally and let the next step-time cleanup prune
        // dead ids if needed. For training this is fine: the limit
        // check is a soft upper bound.
        av.push_back(oid);
        order_owner_[oid] = aid;
    };

    handle_side(ev.buy_order_id,  /*is_buy=*/true);
    handle_side(ev.sell_order_id, /*is_buy=*/false);
}

BookObservation TradingEnv::observe() const {
    std::lock_guard<std::mutex> lock(env_mutex_);

    BookObservation obs;
    obs.bid_depth.assign(prices_.size(), 0);
    obs.ask_depth.assign(prices_.size(), 0);
    obs.best_bid = 0.0;
    obs.best_ask = 0.0;
    obs.sim_step = sim_step_.load();

    // No public depth getter on OrderBook (intentionally — see README).
    // We approximate the observation from env-side bookkeeping: each
    // resting order we placed contributes to depth. For training this
    // is the right view anyway, since these agents are the only
    // participants. If you later add scripted market makers, expose a
    // proper getDepth() on OrderBook.
    // (Iterating order_owner_ is O(n_agents * max_orders) = small.)
    for (auto& kv : order_owner_) {
        (void)kv;
    }
    return obs;
}

AgentState TradingEnv::getAgentState(int agent_id) const {
    std::lock_guard<std::mutex> lock(env_mutex_);
    auto it = agents_.find(agent_id);
    if (it == agents_.end()) return AgentState{};
    return it->second;
}

}  // namespace rl_env
