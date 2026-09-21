#pragma once

#include "order.hpp"
#include "pool_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// Phase 2: bounded tick range + array levels + intrusive pooled FIFO nodes.
class OrderBookV2 {
public:
    OrderBookV2(std::int64_t min_price = -100'000,
                std::int64_t max_price = 100'000,
                std::size_t max_live_orders = 100'000);

    std::vector<Trade> addOrder(Order order);
    bool cancelOrder(std::uint64_t order_id);
    bool reduceOrder(std::uint64_t order_id, std::uint64_t new_quantity);
    std::optional<std::int64_t> bestBid() const;
    std::optional<std::int64_t> bestAsk() const;
    std::vector<std::pair<std::int64_t, std::uint64_t>> depth(Side side,
                                                               std::size_t levels) const;

    std::vector<Trade> modifyOrder(std::uint64_t order_id,
                                   std::int64_t new_price,
                                   std::uint64_t new_quantity);
    bool validateInvariants() const;

private:
    struct PriceLevel {
        std::size_t head{OrderNodePool::npos};
        std::size_t tail{OrderNodePool::npos};
        std::uint64_t total_quantity{0};
    };

    struct Locator {
        Side side;
        std::size_t level_index;
        std::size_t node_index;
    };

    bool isPriceInRange(std::int64_t price) const;
    std::size_t priceToIndex(std::int64_t price) const;
    std::int64_t indexToPrice(std::size_t index) const;
    bool canCross(const Order& incoming) const;
    bool fokFillable(const Order& incoming) const;
    void restOrder(Order order);
    void unlinkAndRelease(const Locator& locator);
    void popHeadAndRelease(Side side, std::size_t level_index);
    bool levelOccupied(std::size_t level_index) const;
    void setLevelOccupied(std::size_t level_index);
    void clearLevelOccupied(std::size_t level_index);
    std::size_t findOccupiedAtOrBelow(std::size_t level_index) const;
    std::size_t findOccupiedAtOrAbove(std::size_t level_index) const;
    void refreshBestBid();
    void refreshBestAsk();
    Trade makeTrade(const Order& maker, const Order& taker, std::uint64_t quantity);

    std::int64_t min_price_;
    std::int64_t max_price_;
    std::vector<PriceLevel> levels_;
    // One bit per price level: fast next/previous occupied-level search.
    std::vector<std::uint64_t> occupied_;
    OrderNodePool pool_;
    std::unordered_map<std::uint64_t, Locator> id_index_;
    std::size_t best_bid_{OrderNodePool::npos};
    std::size_t best_ask_{OrderNodePool::npos};
    std::size_t active_bid_levels_{0};
    std::size_t active_ask_levels_{0};
    std::uint64_t next_order_seq_{0};
    std::uint64_t next_trade_id_{0};
    std::uint64_t next_trade_seq_{0};
};
