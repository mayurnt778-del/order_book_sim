#pragma once

#include <cstdint>

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };
enum class TimeInForce { GTC, IOC, FOK };

struct Order {
    std::uint64_t id{};
    Side side{Side::Buy};
    std::int64_t price{};       // Integer ticks. Market-order prices are ignored.
    std::uint64_t quantity{};   // Remaining live quantity.
    std::uint64_t seq{};        // Assigned by the matching engine on acceptance.
    OrderType type{OrderType::Limit};
    TimeInForce tif{TimeInForce::GTC};
};

struct Trade {
    std::uint64_t trade_id{};
    std::uint64_t maker_order_id{};
    std::uint64_t taker_order_id{};
    std::int64_t price{};
    std::uint64_t quantity{};
    std::uint64_t seq{};
};
