#include "bench_harness.hpp"
#include "order_book.hpp"
#include "order_book_v2.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr std::size_t kWarmup = 10'000;
constexpr std::size_t kLatencySamples = 20'000;
constexpr std::size_t kScenarioSamples = 10'000;
volatile std::uint64_t g_sink = 0;

struct PhaseResults {
    LatencyStats insert_no_match;
    LatencyStats match_1;
    LatencyStats match_5;
    LatencyStats match_50;
    LatencyStats cancel;
    LatencyStats quote_lifecycle;
    LatencyStats market_sweep_100;
    LatencyStats fok_accept_10;
    LatencyStats fok_reject_10;
    LatencyStats modify;
    LatencyStats depth_10;
    ThroughputStats sustained;
};

Order limit(std::uint64_t id,
            Side side,
            std::int64_t price,
            std::uint64_t quantity,
            TimeInForce tif = TimeInForce::GTC) {
    return Order{id, side, price, quantity, 0, OrderType::Limit, tif};
}

Order market(std::uint64_t id, Side side, std::uint64_t quantity) {
    return Order{id, side, 0, quantity, 0, OrderType::Market, TimeInForce::GTC};
}

template <typename Book>
Book makeBook(std::size_t expected_live_orders) {
    if constexpr (std::is_same_v<Book, OrderBook>) {
        return Book(expected_live_orders);
    } else {
        return Book(-20'000, 20'000, expected_live_orders);
    }
}

template <typename Book>
LatencyStats benchmarkInsertNoMatch() {
    Book book = makeBook<Book>(kWarmup + kLatencySamples + 100);
    std::uint64_t next_id = 1;
    const auto operation = [&] {
        g_sink += book.addOrder(limit(next_id++, Side::Buy, 9'900, 1)).size();
    };
    return measureLatency(kWarmup, kLatencySamples, operation, [] {});
}

template <typename Book>
LatencyStats benchmarkMatch(std::size_t fills) {
    Book book = makeBook<Book>(fills + 100);
    std::uint64_t next_id = 1;
    const auto populate = [&] {
        for (std::size_t i = 0; i < fills; ++i) {
            g_sink += book.addOrder(limit(next_id++, Side::Sell, 10'001, 1)).size();
        }
    };
    populate();
    const auto operation = [&] {
        g_sink += book.addOrder(limit(next_id++, Side::Buy, 10'001, fills)).size();
    };
    return measureLatency(kWarmup, kLatencySamples, operation, populate);
}

template <typename Book>
LatencyStats benchmarkCancel() {
    const std::size_t total = kWarmup + kLatencySamples;
    Book book = makeBook<Book>(total + 100);
    std::vector<std::uint64_t> ids;
    ids.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        const std::uint64_t id = static_cast<std::uint64_t>(i + 1);
        ids.push_back(id);
        g_sink += book.addOrder(limit(id, Side::Sell, 10'001, 1)).size();
    }
    std::mt19937_64 rng(0xBA5EBA11U);
    std::shuffle(ids.begin(), ids.end(), rng);
    std::size_t cursor = 0;
    const auto operation = [&] { g_sink += book.cancelOrder(ids[cursor++]) ? 1U : 0U; };
    return measureLatency(kWarmup, kLatencySamples, operation, [] {});
}

template <typename Book>
LatencyStats benchmarkQuoteLifecycle() {
    Book book = makeBook<Book>(100);
    std::uint64_t next_id = 1;
    const auto operation = [&] {
        const std::uint64_t bid_id = next_id++;
        const std::uint64_t ask_id = next_id++;
        g_sink += book.addOrder(limit(bid_id, Side::Buy, 9'999, 1)).size();
        g_sink += book.addOrder(limit(ask_id, Side::Sell, 10'001, 1)).size();
        g_sink += book.cancelOrder(bid_id) ? 1U : 0U;
        g_sink += book.cancelOrder(ask_id) ? 1U : 0U;
    };
    return measureLatency(kWarmup, kScenarioSamples, operation, [] {});
}

template <typename Book>
LatencyStats benchmarkMarketSweep(std::size_t levels) {
    Book book = makeBook<Book>(levels + 100);
    std::uint64_t next_id = 1;
    const auto populate = [&] {
        for (std::size_t i = 0; i < levels; ++i) {
            g_sink += book.addOrder(limit(next_id++, Side::Sell, 10'000 + static_cast<std::int64_t>(i), 1)).size();
        }
    };
    populate();
    const auto operation = [&] { g_sink += book.addOrder(market(next_id++, Side::Buy, levels)).size(); };
    return measureLatency(kWarmup, kScenarioSamples, operation, populate);
}

template <typename Book>
LatencyStats benchmarkFok(bool fillable, std::size_t levels) {
    Book book = makeBook<Book>(levels + 100);
    std::uint64_t next_id = 1;
    const auto populate = [&] {
        for (std::size_t i = 0; i < levels; ++i) {
            g_sink += book.addOrder(limit(next_id++, Side::Sell, 10'000 + static_cast<std::int64_t>(i), 1)).size();
        }
    };
    populate();
    const auto operation = [&] {
        const auto quantity = levels + (fillable ? 0U : 1U);
        g_sink += book.addOrder(limit(next_id++, Side::Buy, 10'000 + static_cast<std::int64_t>(levels - 1U),
                                      quantity, TimeInForce::FOK)).size();
    };
    return fillable ? measureLatency(kWarmup, kScenarioSamples, operation, populate)
                    : measureLatency(kWarmup, kScenarioSamples, operation, [] {});
}

template <typename Book>
LatencyStats benchmarkModify() {
    Book book = makeBook<Book>(100);
    book.addOrder(limit(1, Side::Buy, 9'998, 10));
    bool alternate = false;
    const auto operation = [&] {
        alternate = !alternate;
        g_sink += book.modifyOrder(1, alternate ? 9'999 : 9'998, 10).size();
    };
    return measureLatency(kWarmup, kScenarioSamples, operation, [] {});
}

template <typename Book>
LatencyStats benchmarkDepth() {
    Book book = makeBook<Book>(500);
    std::uint64_t next_id = 1;
    for (std::int64_t price = 9'000; price <= 9'990; price += 5) {
        g_sink += book.addOrder(limit(next_id++, Side::Buy, price, 1)).size();
    }
    const auto operation = [&] { g_sink += book.depth(Side::Buy, 10).size(); };
    return measureLatency(kWarmup, kScenarioSamples, operation, [] {});
}

std::vector<Order> makeWorkload(std::size_t count) {
    std::vector<Order> workload;
    workload.reserve(count);
    std::mt19937_64 rng(0x5EEDU);
    std::normal_distribution<double> around_mid(0.0, 18.0);
    std::exponential_distribution<double> quantity(1.0 / 12.0);
    std::uniform_int_distribution<int> drift(-1, 1);
    std::int64_t mid = 10'000;
    for (std::size_t i = 0; i < count; ++i) {
        mid += drift(rng);
        const auto price = mid + static_cast<std::int64_t>(std::llround(around_mid(rng)));
        const auto size = std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(std::ceil(quantity(rng))));
        const auto side = (rng() & 1U) == 0U ? Side::Buy : Side::Sell;
        workload.push_back(limit(static_cast<std::uint64_t>(i + 1), side, price, size));
    }
    return workload;
}

template <typename Book>
ThroughputStats benchmarkSustained(std::size_t order_count) {
    const auto workload = makeWorkload(kWarmup + order_count);
    Book book = makeBook<Book>(kWarmup + order_count + 100);
    for (std::size_t i = 0; i < kWarmup; ++i) {
        g_sink += book.addOrder(workload[i]).size();
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(order_count);
    const auto overall_started = std::chrono::steady_clock::now();
    for (std::size_t i = kWarmup; i < workload.size(); ++i) {
        const auto started = std::chrono::steady_clock::now();
        g_sink += book.addOrder(workload[i]).size();
        const auto finished = std::chrono::steady_clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
    }
    const auto elapsed = std::chrono::steady_clock::now() - overall_started;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return ThroughputStats{summarize(std::move(latencies)), static_cast<double>(order_count) / seconds};
}

template <typename Book>
PhaseResults runPhase(std::size_t sustained_orders) {
    return PhaseResults{benchmarkInsertNoMatch<Book>(), benchmarkMatch<Book>(1), benchmarkMatch<Book>(5),
                        benchmarkMatch<Book>(50), benchmarkCancel<Book>(), benchmarkQuoteLifecycle<Book>(),
                        benchmarkMarketSweep<Book>(100), benchmarkFok<Book>(true, 10),
                        benchmarkFok<Book>(false, 10), benchmarkModify<Book>(), benchmarkDepth<Book>(),
                        benchmarkSustained<Book>(sustained_orders)};
}

void printResults(const PhaseResults& phase1, const PhaseResults& phase2, std::size_t sustained_orders) {
    std::cout << "\nLatency percentiles in ns (p50/p90/p99/p99.9/max)\n";
    std::cout << std::left << std::setw(27) << "Metric" << " | " << std::setw(27) << "Phase 1" << " | Phase 2\n";
    std::cout << std::string(88, '-') << '\n';
    printComparisonRow("Insert, no match", phase1.insert_no_match, phase2.insert_no_match);
    printComparisonRow("Insert, match 1 order", phase1.match_1, phase2.match_1);
    printComparisonRow("Insert, match 5 orders", phase1.match_5, phase2.match_5);
    printComparisonRow("Insert, match 50 orders", phase1.match_50, phase2.match_50);
    printComparisonRow("Cancel, arbitrary ID", phase1.cancel, phase2.cancel);
    std::cout << "\nOrder-book workload scenarios (same units)\n";
    printComparisonRow("BBO quote lifecycle (4 ops)", phase1.quote_lifecycle, phase2.quote_lifecycle);
    printComparisonRow("Market sweep, 100 levels", phase1.market_sweep_100, phase2.market_sweep_100);
    printComparisonRow("FOK accept, 10 levels", phase1.fok_accept_10, phase2.fok_accept_10);
    printComparisonRow("FOK reject, 10 levels", phase1.fok_reject_10, phase2.fok_reject_10);
    printComparisonRow("Modify/reprice", phase1.modify, phase2.modify);
    printComparisonRow("Depth, 10 sparse levels", phase1.depth_10, phase2.depth_10);
    printComparisonRow("Sustained addOrder", phase1.sustained.latency, phase2.sustained.latency);
    std::cout << "\nSustained workload: " << sustained_orders << " timed orders after " << kWarmup
              << " warm-up orders\n";
    std::cout << "Orders/sec             | " << static_cast<std::uint64_t>(phase1.sustained.orders_per_second)
              << " | " << static_cast<std::uint64_t>(phase2.sustained.orders_per_second) << '\n';
    std::cout << "Timing source: std::chrono::steady_clock; it is portable and monotonic.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t sustained_orders = 1'000'000;
    if (argc == 2) {
        try {
            sustained_orders = std::stoull(argv[1]);
        } catch (const std::exception&) {
            std::cerr << "Usage: bench [sustained-order-count]\n";
            return 2;
        }
    }
    if (sustained_orders == 0) {
        std::cerr << "sustained-order-count must be positive\n";
        return 2;
    }

    std::cout << "Order book benchmark: warm-up=" << kWarmup << ", latency samples=" << kLatencySamples
              << ", sustained orders=" << sustained_orders << '\n';
    const auto phase1 = runPhase<OrderBook>(sustained_orders);
    const auto phase2 = runPhase<OrderBookV2>(sustained_orders);
    printResults(phase1, phase2, sustained_orders);
    std::cout << "Benchmark sink: " << g_sink << '\n';
    return 0;
}
