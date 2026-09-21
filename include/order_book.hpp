#pragma once

#include "order.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// Phase 1 reference engine: ordered maps give correct best-price selection and
// std::list gives FIFO plus O(1) removal once an order has been located.
class OrderBook {
public:
    explicit OrderBook(std::size_t index_reserve = 0);

    std::vector<Trade> addOrder(Order order);
    bool cancelOrder(std::uint64_t order_id);
    bool reduceOrder(std::uint64_t order_id, std::uint64_t new_quantity);
    std::optional<std::int64_t> bestBid() const;
    std::optional<std::int64_t> bestAsk() const;
    std::vector<std::pair<std::int64_t, std::uint64_t>> depth(Side side,
                                                               std::size_t levels) const;

    // Convenience replacement. Repricing or increasing size is cancel + add,
    // so the replacement is assigned a new sequence and loses queue priority.
    std::vector<Trade> modifyOrder(std::uint64_t order_id,
                                   std::int64_t new_price,
                                   std::uint64_t new_quantity);

    // Exposed for the invariant fuzz test; production callers need not use it.
    bool validateInvariants() const;

private:
    struct PriceLevel {
        std::list<Order> orders;
        std::uint64_t total_quantity{0};
    };

    using BidLevels = std::map<std::int64_t, PriceLevel, std::greater<std::int64_t>>;
    using AskLevels = std::map<std::int64_t, PriceLevel, std::less<std::int64_t>>;

    struct Locator {
        Side side;
        BidLevels::iterator bid_level;
        AskLevels::iterator ask_level;
        std::list<Order>::iterator order;
    };

    bool canCross(const Order& incoming) const;
    bool fokFillable(const Order& incoming) const;
    void restOrder(Order order);
    Trade makeTrade(const Order& maker, const Order& taker, std::uint64_t quantity);

    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<std::uint64_t, Locator> id_index_;
    std::uint64_t next_order_seq_{0};
    std::uint64_t next_trade_id_{0};
    std::uint64_t next_trade_seq_{0};
};
