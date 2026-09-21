#include "order_book.hpp"

#include <algorithm>
#include <unordered_set>

OrderBook::OrderBook(std::size_t index_reserve) {
    if (index_reserve != 0) {
        id_index_.reserve(index_reserve);
    }
}

std::vector<Trade> OrderBook::addOrder(Order incoming) {
    std::vector<Trade> trades;
    if (incoming.quantity == 0 || id_index_.find(incoming.id) != id_index_.end()) {
        // Assumption: a duplicate live order ID is rejected without side effects.
        return trades;
    }

    // Assumption: the engine, not the client, owns the monotonic time-priority sequence.
    incoming.seq = ++next_order_seq_;
    if (incoming.tif == TimeInForce::FOK && !fokFillable(incoming)) {
        return trades;
    }

    // TODO: apply exchange-specific self-trade prevention before creating a trade.
    while (incoming.quantity != 0 && canCross(incoming)) {
        if (incoming.side == Side::Buy) {
            auto level_it = asks_.begin();
            PriceLevel& level = level_it->second;
            while (incoming.quantity != 0 && !level.orders.empty()) {
                Order& resting = level.orders.front();
                const std::uint64_t fill = std::min(incoming.quantity, resting.quantity);
                trades.push_back(makeTrade(resting, incoming, fill));
                incoming.quantity -= fill;
                resting.quantity -= fill;
                level.total_quantity -= fill;
                if (resting.quantity == 0) {
                    id_index_.erase(resting.id);
                    level.orders.pop_front();
                }
            }
            if (level.orders.empty()) {
                asks_.erase(level_it);
            }
        } else {
            auto level_it = bids_.begin();
            PriceLevel& level = level_it->second;
            while (incoming.quantity != 0 && !level.orders.empty()) {
                Order& resting = level.orders.front();
                const std::uint64_t fill = std::min(incoming.quantity, resting.quantity);
                trades.push_back(makeTrade(resting, incoming, fill));
                incoming.quantity -= fill;
                resting.quantity -= fill;
                level.total_quantity -= fill;
                if (resting.quantity == 0) {
                    id_index_.erase(resting.id);
                    level.orders.pop_front();
                }
            }
            if (level.orders.empty()) {
                bids_.erase(level_it);
            }
        }
    }

    // FOK preflight guarantees full execution; market and IOC remainders never rest.
    if (incoming.quantity != 0 && incoming.type == OrderType::Limit && incoming.tif == TimeInForce::GTC) {
        restOrder(incoming);
    }
    return trades;
}

bool OrderBook::cancelOrder(std::uint64_t order_id) {
    const auto found = id_index_.find(order_id);
    if (found == id_index_.end()) {
        return false;
    }

    const Locator locator = found->second;
    if (locator.side == Side::Buy) {
        PriceLevel& level = locator.bid_level->second;
        level.total_quantity -= locator.order->quantity;
        level.orders.erase(locator.order);
        id_index_.erase(found);
        if (level.orders.empty()) {
            // Erasing by the stored map iterator is O(1) amortized.
            bids_.erase(locator.bid_level);
        }
    } else {
        PriceLevel& level = locator.ask_level->second;
        level.total_quantity -= locator.order->quantity;
        level.orders.erase(locator.order);
        id_index_.erase(found);
        if (level.orders.empty()) {
            asks_.erase(locator.ask_level);
        }
    }
    return true;
}

bool OrderBook::reduceOrder(std::uint64_t order_id, std::uint64_t new_quantity) {
    const auto found = id_index_.find(order_id);
    if (found == id_index_.end()) {
        return false;
    }
    const Locator locator = found->second;
    const std::uint64_t current_quantity = locator.order->quantity;
    if (new_quantity >= current_quantity) {
        return false;
    }
    if (new_quantity == 0) {
        // Assumption: reducing to zero is a cancel, which is common exchange behavior.
        return cancelOrder(order_id);
    }

    if (locator.side == Side::Buy) {
        locator.bid_level->second.total_quantity -= current_quantity - new_quantity;
    } else {
        locator.ask_level->second.total_quantity -= current_quantity - new_quantity;
    }
    locator.order->quantity = new_quantity;
    return true;
}

std::optional<std::int64_t> OrderBook::bestBid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<std::int64_t> OrderBook::bestAsk() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::vector<std::pair<std::int64_t, std::uint64_t>> OrderBook::depth(Side side,
                                                                       std::size_t levels) const {
    std::vector<std::pair<std::int64_t, std::uint64_t>> result;
    result.reserve(levels);
    if (side == Side::Buy) {
        for (auto it = bids_.begin(); it != bids_.end() && result.size() < levels; ++it) {
            result.emplace_back(it->first, it->second.total_quantity);
        }
    } else {
        for (auto it = asks_.begin(); it != asks_.end() && result.size() < levels; ++it) {
            result.emplace_back(it->first, it->second.total_quantity);
        }
    }
    return result;
}

std::vector<Trade> OrderBook::modifyOrder(std::uint64_t order_id,
                                           std::int64_t new_price,
                                           std::uint64_t new_quantity) {
    const auto found = id_index_.find(order_id);
    if (found == id_index_.end()) {
        return {};
    }
    const Order existing = *found->second.order;
    if (new_quantity == 0) {
        cancelOrder(order_id);
        return {};
    }
    if (new_price == existing.price && new_quantity < existing.quantity) {
        reduceOrder(order_id, new_quantity);
        return {};
    }
    if (new_price == existing.price && new_quantity == existing.quantity) {
        return {};
    }

    cancelOrder(order_id);
    Order replacement = existing;
    replacement.price = new_price;
    replacement.quantity = new_quantity;
    replacement.type = OrderType::Limit;
    replacement.tif = TimeInForce::GTC;
    return addOrder(replacement);
}

bool OrderBook::validateInvariants() const {
    std::size_t observed_orders = 0;
    const auto check_levels = [&](const auto& book, Side side) {
        for (auto level_it = book.begin(); level_it != book.end(); ++level_it) {
            const PriceLevel& level = level_it->second;
            if (level.orders.empty()) {
                return false;
            }
            std::uint64_t total = 0;
            for (auto order_it = level.orders.begin(); order_it != level.orders.end(); ++order_it) {
                if (order_it->quantity == 0 || total > UINT64_MAX - order_it->quantity) {
                    return false;
                }
                total += order_it->quantity;
                const auto found = id_index_.find(order_it->id);
                if (found == id_index_.end() || found->second.side != side ||
                    &(*found->second.order) != &(*order_it)) {
                    return false;
                }
                if (side == Side::Buy && found->second.bid_level->first != level_it->first) {
                    return false;
                }
                if (side == Side::Sell && found->second.ask_level->first != level_it->first) {
                    return false;
                }
                ++observed_orders;
            }
            if (total != level.total_quantity) {
                return false;
            }
        }
        return true;
    };

    if (!check_levels(bids_, Side::Buy) || !check_levels(asks_, Side::Sell)) {
        return false;
    }
    const auto bid = bestBid();
    const auto ask = bestAsk();
    return observed_orders == id_index_.size() && (!bid || !ask || *bid < *ask);
}

bool OrderBook::canCross(const Order& incoming) const {
    if (incoming.side == Side::Buy) {
        return !asks_.empty() &&
               (incoming.type == OrderType::Market || incoming.price >= asks_.begin()->first);
    }
    return !bids_.empty() &&
           (incoming.type == OrderType::Market || incoming.price <= bids_.begin()->first);
}

bool OrderBook::fokFillable(const Order& incoming) const {
    std::uint64_t remaining = incoming.quantity;
    if (incoming.side == Side::Buy) {
        for (const auto& [price, level] : asks_) {
            if (incoming.type == OrderType::Limit && price > incoming.price) {
                break;
            }
            if (level.total_quantity >= remaining) {
                return true;
            }
            remaining -= level.total_quantity;
        }
    } else {
        for (const auto& [price, level] : bids_) {
            if (incoming.type == OrderType::Limit && price < incoming.price) {
                break;
            }
            if (level.total_quantity >= remaining) {
                return true;
            }
            remaining -= level.total_quantity;
        }
    }
    return false;
}

void OrderBook::restOrder(Order order) {
    if (order.side == Side::Buy) {
        auto level_it = bids_.try_emplace(order.price).first;
        PriceLevel& level = level_it->second;
        level.orders.push_back(order);
        auto order_it = std::prev(level.orders.end());
        level.total_quantity += order.quantity;
        id_index_.emplace(order.id, Locator{Side::Buy, level_it, asks_.end(), order_it});
    } else {
        auto level_it = asks_.try_emplace(order.price).first;
        PriceLevel& level = level_it->second;
        level.orders.push_back(order);
        auto order_it = std::prev(level.orders.end());
        level.total_quantity += order.quantity;
        id_index_.emplace(order.id, Locator{Side::Sell, bids_.end(), level_it, order_it});
    }
}

Trade OrderBook::makeTrade(const Order& maker, const Order& taker, std::uint64_t quantity) {
    return Trade{++next_trade_id_, maker.id, taker.id, maker.price, quantity, ++next_trade_seq_};
}
