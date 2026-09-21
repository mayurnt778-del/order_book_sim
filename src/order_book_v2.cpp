#include "order_book_v2.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace {

std::size_t leastSignificantSetBit(std::uint64_t word) {
    assert(word != 0);
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long index = 0;
    _BitScanForward64(&index, word);
    return static_cast<std::size_t>(index);
#elif defined(_MSC_VER)
    std::size_t index = 0;
    while ((word & (std::uint64_t{1} << index)) == 0) {
        ++index;
    }
    return index;
#else
    return static_cast<std::size_t>(__builtin_ctzll(word));
#endif
}

std::size_t mostSignificantSetBit(std::uint64_t word) {
    assert(word != 0);
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long index = 0;
    _BitScanReverse64(&index, word);
    return static_cast<std::size_t>(index);
#elif defined(_MSC_VER)
    std::size_t index = 63U;
    while ((word & (std::uint64_t{1} << index)) == 0) {
        --index;
    }
    return index;
#else
    return 63U - static_cast<std::size_t>(__builtin_clzll(word));
#endif
}

}  // namespace

OrderBookV2::OrderBookV2(std::int64_t min_price,
                         std::int64_t max_price,
                         std::size_t max_live_orders)
    : min_price_(min_price),
      max_price_(max_price),
      pool_(max_live_orders) {
    if (min_price > max_price) {
        throw std::invalid_argument("min_price must not exceed max_price");
    }
    const auto distance = static_cast<std::uint64_t>(max_price_) -
                          static_cast<std::uint64_t>(min_price_);
    if (distance == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument("configured tick range is too large");
    }
    const auto span = distance + 1U;
    if (span > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("configured tick range is too large");
    }
    // Fixed range trades memory for O(1) price-level lookup and cached best prices.
    levels_.resize(static_cast<std::size_t>(span));
    occupied_.resize(levels_.size() / 64U + (levels_.size() % 64U != 0U), 0);
    id_index_.reserve(max_live_orders);
}

std::vector<Trade> OrderBookV2::addOrder(Order incoming) {
    std::vector<Trade> trades;
    if (incoming.quantity == 0 || id_index_.find(incoming.id) != id_index_.end() ||
        (incoming.type == OrderType::Limit && !isPriceInRange(incoming.price))) {
        // Assumption: V2 rejects a limit price outside its configured bounded tick range.
        return trades;
    }

    // Preflight a possible residual before mutation so pool exhaustion is atomic.
    if (incoming.type == OrderType::Limit && incoming.tif == TimeInForce::GTC &&
        !pool_.hasAvailableNode() && !fokFillable(incoming)) {
        throw std::runtime_error("OrderBookV2 node pool exhausted");
    }

    // Assumption: time priority is assigned at engine ingress, never trusted from clients.
    incoming.seq = ++next_order_seq_;
    if (incoming.tif == TimeInForce::FOK && !fokFillable(incoming)) {
        return trades;
    }

    // TODO: apply exchange-specific self-trade prevention before creating a trade.
    while (incoming.quantity != 0 && canCross(incoming)) {
        const std::size_t level_index = incoming.side == Side::Buy ? best_ask_ : best_bid_;
        PriceLevel& level = levels_[level_index];
        while (incoming.quantity != 0 && level.head != OrderNodePool::npos) {
            const std::size_t node_index = level.head;
            OrderNodePool::Node& resting_node = pool_.node(node_index);
            const std::uint64_t fill = std::min(incoming.quantity, resting_node.order.quantity);
            trades.push_back(makeTrade(resting_node.order, incoming, fill));
            incoming.quantity -= fill;
            resting_node.order.quantity -= fill;
            level.total_quantity -= fill;
            if (resting_node.order.quantity == 0) {
                const std::uint64_t resting_id = resting_node.order.id;
                const Side resting_side = resting_node.order.side;
                id_index_.erase(resting_id);
                popHeadAndRelease(resting_side, level_index);
            }
        }
    }

    if (incoming.quantity != 0 && incoming.type == OrderType::Limit && incoming.tif == TimeInForce::GTC) {
        restOrder(incoming);
    }
    return trades;
}

bool OrderBookV2::cancelOrder(std::uint64_t order_id) {
    const auto found = id_index_.find(order_id);
    if (found == id_index_.end()) {
        return false;
    }
    const Locator locator = found->second;
    PriceLevel& level = levels_[locator.level_index];
    level.total_quantity -= pool_.node(locator.node_index).order.quantity;
    id_index_.erase(found);
    unlinkAndRelease(locator);
    return true;
}

bool OrderBookV2::reduceOrder(std::uint64_t order_id, std::uint64_t new_quantity) {
    const auto found = id_index_.find(order_id);
    if (found == id_index_.end()) {
        return false;
    }
    const Locator locator = found->second;
    OrderNodePool::Node& node = pool_.node(locator.node_index);
    if (new_quantity >= node.order.quantity) {
        return false;
    }
    if (new_quantity == 0) {
        return cancelOrder(order_id);
    }
    levels_[locator.level_index].total_quantity -= node.order.quantity - new_quantity;
    node.order.quantity = new_quantity;
    return true;
}

std::optional<std::int64_t> OrderBookV2::bestBid() const {
    return best_bid_ == OrderNodePool::npos ? std::nullopt
                                             : std::optional<std::int64_t>(indexToPrice(best_bid_));
}

std::optional<std::int64_t> OrderBookV2::bestAsk() const {
    return best_ask_ == OrderNodePool::npos ? std::nullopt
                                             : std::optional<std::int64_t>(indexToPrice(best_ask_));
}

std::vector<std::pair<std::int64_t, std::uint64_t>> OrderBookV2::depth(
    Side side,
    std::size_t requested_levels) const {
    std::vector<std::pair<std::int64_t, std::uint64_t>> result;
    result.reserve(requested_levels);
    if (side == Side::Buy) {
        for (std::size_t i = best_bid_; i != OrderNodePool::npos && result.size() < requested_levels;
             i = i == 0 ? OrderNodePool::npos : findOccupiedAtOrBelow(i - 1U)) {
            result.emplace_back(indexToPrice(i), levels_[i].total_quantity);
        }
    } else {
        for (std::size_t i = best_ask_; i != OrderNodePool::npos && result.size() < requested_levels;
             i = i + 1U == levels_.size() ? OrderNodePool::npos : findOccupiedAtOrAbove(i + 1U)) {
            result.emplace_back(indexToPrice(i), levels_[i].total_quantity);
        }
    }
    return result;
}

std::vector<Trade> OrderBookV2::modifyOrder(std::uint64_t order_id,
                                             std::int64_t new_price,
                                             std::uint64_t new_quantity) {
    const auto found = id_index_.find(order_id);
    if (found == id_index_.end()) {
        return {};
    }
    const Order existing = pool_.node(found->second.node_index).order;
    if (new_quantity == 0) {
        cancelOrder(order_id);
        return {};
    }
    if (!isPriceInRange(new_price)) {
        // Reject an invalid replacement atomically; keep the current resting order.
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

bool OrderBookV2::validateInvariants() const {
    std::size_t observed_orders = 0;
    std::size_t observed_bid_levels = 0;
    std::size_t observed_ask_levels = 0;
    std::size_t expected_best_bid = OrderNodePool::npos;
    std::size_t expected_best_ask = OrderNodePool::npos;
    for (std::size_t level_index = 0; level_index < levels_.size(); ++level_index) {
        const PriceLevel& level = levels_[level_index];
        if ((level.head != OrderNodePool::npos) != levelOccupied(level_index)) {
            return false;
        }
        if ((level.head == OrderNodePool::npos) != (level.tail == OrderNodePool::npos)) {
            return false;
        }
        std::uint64_t total = 0;
        std::size_t previous = OrderNodePool::npos;
        std::size_t node_index = level.head;
        std::size_t traversed = 0;
        Side level_side = Side::Buy;
        if (node_index != OrderNodePool::npos) {
            level_side = pool_.node(node_index).order.side;
            if (level_side == Side::Buy) {
                ++observed_bid_levels;
                expected_best_bid = level_index;
            } else {
                ++observed_ask_levels;
                if (expected_best_ask == OrderNodePool::npos) {
                    expected_best_ask = level_index;
                }
            }
        }
        while (node_index != OrderNodePool::npos) {
            if (++traversed > pool_.capacity()) {
                return false;
            }
            const OrderNodePool::Node& node = pool_.node(node_index);
            if (!node.active || node.prev != previous || node.order.quantity == 0 ||
                node.order.side != level_side || total > UINT64_MAX - node.order.quantity) {
                return false;
            }
            total += node.order.quantity;
            const auto found = id_index_.find(node.order.id);
            if (found == id_index_.end() || found->second.side != node.order.side ||
                found->second.level_index != level_index || found->second.node_index != node_index) {
                return false;
            }
            previous = node_index;
            node_index = node.next;
            ++observed_orders;
        }
        if (total != level.total_quantity || (level.tail != OrderNodePool::npos && previous != level.tail)) {
            return false;
        }
    }
    for (const auto& [id, locator] : id_index_) {
        if (locator.node_index >= pool_.capacity() || !pool_.node(locator.node_index).active ||
            pool_.node(locator.node_index).order.id != id) {
            return false;
        }
    }
    const auto bid = bestBid();
    const auto ask = bestAsk();
    return observed_orders == id_index_.size() && observed_bid_levels == active_bid_levels_ &&
           observed_ask_levels == active_ask_levels_ && best_bid_ == expected_best_bid &&
           best_ask_ == expected_best_ask && (!bid || !ask || *bid < *ask);
}

bool OrderBookV2::isPriceInRange(std::int64_t price) const {
    return price >= min_price_ && price <= max_price_;
}

std::size_t OrderBookV2::priceToIndex(std::int64_t price) const {
    return static_cast<std::size_t>(price - min_price_);
}

std::int64_t OrderBookV2::indexToPrice(std::size_t index) const {
    return min_price_ + static_cast<std::int64_t>(index);
}

bool OrderBookV2::canCross(const Order& incoming) const {
    if (incoming.side == Side::Buy) {
        return best_ask_ != OrderNodePool::npos &&
               (incoming.type == OrderType::Market || incoming.price >= indexToPrice(best_ask_));
    }
    return best_bid_ != OrderNodePool::npos &&
           (incoming.type == OrderType::Market || incoming.price <= indexToPrice(best_bid_));
}

bool OrderBookV2::fokFillable(const Order& incoming) const {
    std::uint64_t remaining = incoming.quantity;
    if (incoming.side == Side::Buy) {
        for (std::size_t i = best_ask_; i != OrderNodePool::npos; ++i) {
            if (incoming.type == OrderType::Limit && indexToPrice(i) > incoming.price) {
                break;
            }
            if (levels_[i].total_quantity >= remaining) {
                return true;
            }
            remaining -= levels_[i].total_quantity;
            if (i + 1 == levels_.size()) {
                break;
            }
        }
    } else {
        for (std::size_t i = best_bid_; i != OrderNodePool::npos;) {
            if (incoming.type == OrderType::Limit && indexToPrice(i) < incoming.price) {
                break;
            }
            if (levels_[i].total_quantity >= remaining) {
                return true;
            }
            remaining -= levels_[i].total_quantity;
            if (i == 0) {
                break;
            }
            --i;
        }
    }
    return false;
}

void OrderBookV2::restOrder(Order order) {
    const std::size_t level_index = priceToIndex(order.price);
    PriceLevel& level = levels_[level_index];
    const bool was_empty = level.head == OrderNodePool::npos;
    const std::size_t node_index = pool_.acquire(order);
    OrderNodePool::Node& node = pool_.node(node_index);
    node.prev = level.tail;
    if (level.tail == OrderNodePool::npos) {
        level.head = node_index;
    } else {
        pool_.node(level.tail).next = node_index;
    }
    level.tail = node_index;
    level.total_quantity += order.quantity;
    id_index_.emplace(order.id, Locator{order.side, level_index, node_index});
    if (was_empty) {
        setLevelOccupied(level_index);
        if (order.side == Side::Buy) {
            ++active_bid_levels_;
        } else {
            ++active_ask_levels_;
        }
    }

    if (order.side == Side::Buy && (best_bid_ == OrderNodePool::npos || level_index > best_bid_)) {
        best_bid_ = level_index;
    }
    if (order.side == Side::Sell && (best_ask_ == OrderNodePool::npos || level_index < best_ask_)) {
        best_ask_ = level_index;
    }
}

void OrderBookV2::unlinkAndRelease(const Locator& locator) {
    PriceLevel& level = levels_[locator.level_index];
    OrderNodePool::Node& node = pool_.node(locator.node_index);
    if (node.prev == OrderNodePool::npos) {
        level.head = node.next;
    } else {
        pool_.node(node.prev).next = node.next;
    }
    if (node.next == OrderNodePool::npos) {
        level.tail = node.prev;
    } else {
        pool_.node(node.next).prev = node.prev;
    }
    pool_.release(locator.node_index);

    if (level.head == OrderNodePool::npos) {
        level.tail = OrderNodePool::npos;
        level.total_quantity = 0;
        clearLevelOccupied(locator.level_index);
        if (locator.side == Side::Buy) {
            --active_bid_levels_;
            if (best_bid_ == locator.level_index) {
                refreshBestBid();
            }
        } else {
            --active_ask_levels_;
            if (best_ask_ == locator.level_index) {
                refreshBestAsk();
            }
        }
    }
}

void OrderBookV2::popHeadAndRelease(Side side, std::size_t level_index) {
    PriceLevel& level = levels_[level_index];
    const std::size_t node_index = level.head;
    const std::size_t next = pool_.node(node_index).next;
    level.head = next;
    if (next == OrderNodePool::npos) {
        level.tail = OrderNodePool::npos;
    } else {
        pool_.node(next).prev = OrderNodePool::npos;
    }
    pool_.release(node_index);

    if (level.head == OrderNodePool::npos) {
        level.total_quantity = 0;
        clearLevelOccupied(level_index);
        if (side == Side::Buy) {
            --active_bid_levels_;
            if (best_bid_ == level_index) {
                refreshBestBid();
            }
        } else {
            --active_ask_levels_;
            if (best_ask_ == level_index) {
                refreshBestAsk();
            }
        }
    }
}

bool OrderBookV2::levelOccupied(std::size_t level_index) const {
    return (occupied_[level_index >> 6U] & (std::uint64_t{1} << (level_index & 63U))) != 0;
}

void OrderBookV2::setLevelOccupied(std::size_t level_index) {
    occupied_[level_index >> 6U] |= std::uint64_t{1} << (level_index & 63U);
}

void OrderBookV2::clearLevelOccupied(std::size_t level_index) {
    occupied_[level_index >> 6U] &= ~(std::uint64_t{1} << (level_index & 63U));
}

std::size_t OrderBookV2::findOccupiedAtOrBelow(std::size_t level_index) const {
    const std::size_t start_word = level_index >> 6U;
    const std::size_t start_bit = level_index & 63U;
    for (std::size_t word_index = start_word + 1U; word_index-- > 0;) {
        std::uint64_t word = occupied_[word_index];
        if (word_index == start_word && start_bit != 63U) {
            word &= (std::uint64_t{1} << (start_bit + 1U)) - 1U;
        }
        if (word != 0) {
            return (word_index << 6U) + mostSignificantSetBit(word);
        }
    }
    return OrderNodePool::npos;
}

std::size_t OrderBookV2::findOccupiedAtOrAbove(std::size_t level_index) const {
    const std::size_t start_word = level_index >> 6U;
    const std::size_t start_bit = level_index & 63U;
    for (std::size_t word_index = start_word; word_index < occupied_.size(); ++word_index) {
        std::uint64_t word = occupied_[word_index];
        if (word_index == start_word && start_bit != 0) {
            word &= ~((std::uint64_t{1} << start_bit) - 1U);
        }
        if (word != 0) {
            return (word_index << 6U) + leastSignificantSetBit(word);
        }
    }
    return OrderNodePool::npos;
}

void OrderBookV2::refreshBestBid() {
    best_bid_ = active_bid_levels_ == 0 ? OrderNodePool::npos : findOccupiedAtOrBelow(best_bid_);
}

void OrderBookV2::refreshBestAsk() {
    best_ask_ = active_ask_levels_ == 0 ? OrderNodePool::npos : findOccupiedAtOrAbove(best_ask_);
}

Trade OrderBookV2::makeTrade(const Order& maker, const Order& taker, std::uint64_t quantity) {
    return Trade{++next_trade_id_, maker.id, taker.id, maker.price, quantity, ++next_trade_seq_};
}
