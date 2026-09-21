#include "order_book.hpp"
#include "order_book_v2.hpp"
#include "test_framework.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void runInvariantTests(TestSuite& suite);

namespace {

Order limit(std::uint64_t id,
            Side side,
            std::int64_t price,
            std::uint64_t quantity,
            TimeInForce tif = TimeInForce::GTC) {
    return Order{id, side, price, quantity, 0, OrderType::Limit, tif};
}

Order market(std::uint64_t id, Side side, std::uint64_t quantity, TimeInForce tif = TimeInForce::GTC) {
    return Order{id, side, 0, quantity, 0, OrderType::Market, tif};
}

template <typename Book>
void runMatchingCases(TestSuite& suite, const std::string& label) {
    {
        Book book;
        const auto trades = book.addOrder(limit(1, Side::Buy, 100, 10));
        EXPECT_TRUE(suite, trades.empty());
        EXPECT_EQ(suite, book.bestBid().value(), 100);
        EXPECT_EQ(suite, book.depth(Side::Buy, 1).at(0).second, 10U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 10));
        const auto trades = book.addOrder(limit(2, Side::Buy, 101, 10));
        EXPECT_EQ(suite, trades.size(), 1U);
        EXPECT_EQ(suite, trades.at(0).maker_order_id, 1U);
        EXPECT_EQ(suite, trades.at(0).taker_order_id, 2U);
        EXPECT_EQ(suite, trades.at(0).price, 101);
        EXPECT_TRUE(suite, !book.bestAsk().has_value());
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 10));
        const auto trades = book.addOrder(limit(2, Side::Buy, 101, 4));
        EXPECT_EQ(suite, trades.size(), 1U);
        EXPECT_EQ(suite, trades.at(0).quantity, 4U);
        EXPECT_EQ(suite, book.depth(Side::Sell, 1).at(0).second, 6U);
        const auto second = book.addOrder(limit(3, Side::Buy, 101, 6));
        EXPECT_EQ(suite, second.at(0).maker_order_id, 1U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 5));
        book.addOrder(limit(2, Side::Sell, 101, 5));
        const auto trades = book.addOrder(limit(3, Side::Buy, 101, 7));
        EXPECT_EQ(suite, trades.size(), 2U);
        EXPECT_EQ(suite, trades.at(0).maker_order_id, 1U);
        EXPECT_EQ(suite, trades.at(0).quantity, 5U);
        EXPECT_EQ(suite, trades.at(1).maker_order_id, 2U);
        EXPECT_EQ(suite, trades.at(1).quantity, 2U);
        EXPECT_EQ(suite, book.depth(Side::Sell, 1).at(0).second, 3U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 3));
        book.addOrder(limit(2, Side::Sell, 102, 4));
        const auto trades = book.addOrder(limit(3, Side::Buy, 102, 7));
        EXPECT_EQ(suite, trades.size(), 2U);
        EXPECT_EQ(suite, trades.at(0).price, 101);
        EXPECT_EQ(suite, trades.at(1).price, 102);
        EXPECT_TRUE(suite, !book.bestAsk().has_value());
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 3));
        book.addOrder(limit(2, Side::Sell, 102, 4));
        const auto trades = book.addOrder(limit(3, Side::Buy, 102, 7, TimeInForce::FOK));
        EXPECT_EQ(suite, trades.size(), 2U);
        EXPECT_EQ(suite, trades.at(0).quantity, 3U);
        EXPECT_EQ(suite, trades.at(1).quantity, 4U);
        EXPECT_TRUE(suite, !book.bestAsk().has_value());
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 3));
        const auto trades = book.addOrder(market(2, Side::Buy, 10));
        EXPECT_EQ(suite, trades.size(), 1U);
        EXPECT_EQ(suite, trades.at(0).quantity, 3U);
        EXPECT_TRUE(suite, !book.bestBid().has_value());
        EXPECT_TRUE(suite, !book.bestAsk().has_value());
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        const auto trades = book.addOrder(limit(1, Side::Buy, 100, 10, TimeInForce::IOC));
        EXPECT_TRUE(suite, trades.empty());
        EXPECT_TRUE(suite, !book.bestBid().has_value());
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 3));
        const auto before = book.depth(Side::Sell, 5);
        const auto trades = book.addOrder(limit(2, Side::Buy, 101, 4, TimeInForce::FOK));
        EXPECT_TRUE(suite, trades.empty());
        EXPECT_EQ(suite, book.depth(Side::Sell, 5), before);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 5));
        EXPECT_TRUE(suite, book.cancelOrder(1));
        const auto trades = book.addOrder(limit(2, Side::Buy, 101, 5));
        EXPECT_TRUE(suite, trades.empty());
        EXPECT_EQ(suite, book.depth(Side::Buy, 1).at(0).second, 5U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 5));
        book.addOrder(limit(2, Side::Sell, 101, 5));
        EXPECT_TRUE(suite, book.reduceOrder(1, 3));
        const auto trades = book.addOrder(limit(3, Side::Buy, 101, 4));
        EXPECT_EQ(suite, trades.size(), 2U);
        EXPECT_EQ(suite, trades.at(0).maker_order_id, 1U);
        EXPECT_EQ(suite, trades.at(0).quantity, 3U);
        EXPECT_EQ(suite, trades.at(1).maker_order_id, 2U);
        EXPECT_EQ(suite, trades.at(1).quantity, 1U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Buy, 99, 2));
        book.addOrder(limit(2, Side::Buy, 100, 3));
        book.addOrder(limit(3, Side::Buy, 98, 4));
        const auto levels = book.depth(Side::Buy, 2);
        EXPECT_EQ(suite, levels.size(), 2U);
        EXPECT_EQ(suite, levels.at(0).first, 100);
        EXPECT_EQ(suite, levels.at(0).second, 3U);
        EXPECT_EQ(suite, levels.at(1).first, 99);
        EXPECT_EQ(suite, levels.at(1).second, 2U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        Book book;
        book.addOrder(limit(1, Side::Sell, 101, 5));
        book.addOrder(limit(2, Side::Sell, 101, 5));
        const auto replacement_trades = book.modifyOrder(1, 101, 6);
        EXPECT_TRUE(suite, replacement_trades.empty());
        const auto trades = book.addOrder(limit(3, Side::Buy, 101, 6));
        EXPECT_EQ(suite, trades.size(), 2U);
        EXPECT_EQ(suite, trades.at(0).maker_order_id, 2U);
        EXPECT_EQ(suite, trades.at(0).quantity, 5U);
        EXPECT_EQ(suite, trades.at(1).maker_order_id, 1U);
        EXPECT_EQ(suite, trades.at(1).quantity, 1U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    std::cout << "Passed matching cases for " << label << '\n';
}

void runV2SpecificCases(TestSuite& suite) {
    {
        OrderBookV2 book(99, 101, 2);
        const auto rejected = book.addOrder(limit(1, Side::Buy, 102, 2));
        EXPECT_TRUE(suite, rejected.empty());
        EXPECT_TRUE(suite, !book.bestBid().has_value());
        book.addOrder(limit(2, Side::Buy, 100, 2));
        const auto rejected_modify = book.modifyOrder(2, 102, 3);
        EXPECT_TRUE(suite, rejected_modify.empty());
        EXPECT_EQ(suite, book.depth(Side::Buy, 1).at(0).second, 2U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    {
        OrderBookV2 book(99, 101, 1);
        book.addOrder(limit(1, Side::Sell, 100, 1));
        bool exhausted = false;
        try {
            book.addOrder(limit(2, Side::Buy, 101, 2));
        } catch (const std::runtime_error&) {
            exhausted = true;
        }
        EXPECT_TRUE(suite, exhausted);
        EXPECT_EQ(suite, book.depth(Side::Sell, 1).at(0).second, 1U);
        EXPECT_TRUE(suite, book.validateInvariants());
    }
    std::cout << "Passed V2 range and exhaustion cases\n";
}

}  // namespace

int main() {
    TestSuite suite;
    runMatchingCases<OrderBook>(suite, "Phase 1");
    runMatchingCases<OrderBookV2>(suite, "Phase 2");
    runV2SpecificCases(suite);
    runInvariantTests(suite);
    if (suite.failures() != 0) {
        std::cerr << suite.failures() << " test expectation(s) failed\n";
        return 1;
    }
    std::cout << "All order-book tests passed.\n";
    return 0;
}
