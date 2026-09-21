#pragma once

#include "order.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

// Fixed-capacity node pool removes per-order list-node allocation from V2's hot path.
class OrderNodePool {
public:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    struct Node {
        Order order{};
        std::size_t prev{npos};
        std::size_t next{npos};
        bool active{false};
    };

    explicit OrderNodePool(std::size_t capacity) : nodes_(capacity) {
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            nodes_[i].next = (i + 1 < nodes_.size()) ? i + 1 : npos;
        }
        free_head_ = nodes_.empty() ? npos : 0;
    }

    std::size_t acquire(const Order& order) {
        if (free_head_ == npos) {
            // Assumption: exceeding configured live-order capacity is a caller configuration error.
            throw std::runtime_error("OrderBookV2 node pool exhausted");
        }
        const std::size_t index = free_head_;
        Node& node = nodes_[index];
        free_head_ = node.next;
        node.order = order;
        node.prev = npos;
        node.next = npos;
        node.active = true;
        return index;
    }

    void release(std::size_t index) {
        Node& node = nodes_.at(index);
        node.active = false;
        node.prev = npos;
        node.next = free_head_;
        free_head_ = index;
    }

    // All production indices originate in this pool; validation checks indices
    // explicitly, so checked access is not needed in the matching hot path.
    Node& node(std::size_t index) { return nodes_[index]; }
    const Node& node(std::size_t index) const { return nodes_[index]; }
    std::size_t capacity() const { return nodes_.size(); }
    bool hasAvailableNode() const { return free_head_ != npos; }

private:
    std::vector<Node> nodes_;
    std::size_t free_head_{npos};
};
