#include "order_book.hpp"
#include "order_book_v2.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <iostream>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

template <typename Book>
void fuzz(TestSuite& suite, Book& book, const char* name) {
    constexpr std::size_t operations = 100'000;
    std::mt19937_64 rng(0xC0FFEEU);
    struct LiveOrder {
        std::uint64_t quantity;
        std::size_t position;
        Side side;
        std::int64_t price;
    };
    std::unordered_map<std::uint64_t, LiveOrder> live_orders;
    live_orders.reserve(operations);
    std::vector<std::uint64_t> live_ids;
    live_ids.reserve(operations);
    std::uint64_t next_id = 1;
    std::uint64_t trades_seen = 0;

    const auto remove_live = [&](std::size_t position) {
        const std::uint64_t removed_id = live_ids.at(position);
        const std::uint64_t moved_id = live_ids.back();
        live_ids.at(position) = moved_id;
        live_orders.at(moved_id).position = position;
        live_ids.pop_back();
        live_orders.erase(removed_id);
    };

    for (std::size_t i = 0; i < operations; ++i) {
        const auto operation = rng() % 100U;
        // Keep the independently verified book small enough to run a complete
        // structural scan after every one of the 100,000 random operations.
        const bool can_add = live_orders.size() < 256;
        if ((operation < 55U && can_add) || live_orders.empty()) {
            const Side side = (rng() & 1U) == 0U ? Side::Buy : Side::Sell;
            // Overlapping bands create random trades as well as resting orders.
            const std::int64_t price = 95 + static_cast<std::int64_t>(rng() % 11U);
            const std::uint64_t quantity = 1 + rng() % 100U;
            const std::uint64_t id = next_id++;
            const auto trades = book.addOrder(Order{id, side, price, quantity, 0, OrderType::Limit,
                                                     TimeInForce::GTC});
            std::uint64_t filled = 0;
            for (const Trade& trade : trades) {
                ++trades_seen;
                const auto maker = live_orders.find(trade.maker_order_id);
                EXPECT_TRUE(suite, maker != live_orders.end());
                if (maker == live_orders.end()) {
                    continue;
                }
                EXPECT_TRUE(suite, trade.quantity <= maker->second.quantity);
                filled += trade.quantity;
                if (trade.quantity == maker->second.quantity) {
                    remove_live(maker->second.position);
                } else {
                    maker->second.quantity -= trade.quantity;
                }
            }
            EXPECT_TRUE(suite, filled <= quantity);
            if (filled != quantity) {
                live_orders.emplace(id, LiveOrder{quantity - filled, live_ids.size(), side, price});
                live_ids.push_back(id);
            }
        } else {
            const std::size_t target = static_cast<std::size_t>(rng() % live_ids.size());
            const std::uint64_t id = live_ids.at(target);
            const LiveOrder live = live_orders.at(id);
            if (operation < 80U) {
                EXPECT_TRUE(suite, book.cancelOrder(id));
                remove_live(target);
            } else {
                const std::uint64_t reduced = live.quantity == 1 ? 0 : rng() % live.quantity;
                EXPECT_TRUE(suite, book.reduceOrder(id, reduced));
                if (reduced == 0) {
                    remove_live(target);
                } else {
                    live_orders.at(id).quantity = reduced;
                }
            }
        }
        EXPECT_TRUE(suite, book.validateInvariants());
        const auto bid = book.bestBid();
        const auto ask = book.bestAsk();
        EXPECT_TRUE(suite, !bid || !ask || *bid < *ask);
    }
    EXPECT_TRUE(suite, trades_seen > 0);
    std::cout << "Passed 100,000-operation invariant fuzz for " << name << '\n';
}

}  // namespace

void runInvariantTests(TestSuite& suite) {
    OrderBook phase1(100'000);
    fuzz(suite, phase1, "Phase 1");
    OrderBookV2 phase2(80, 120, 512);
    fuzz(suite, phase2, "Phase 2");
}
