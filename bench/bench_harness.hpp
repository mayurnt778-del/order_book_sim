#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

struct LatencyStats {
    std::uint64_t p50_ns{};
    std::uint64_t p90_ns{};
    std::uint64_t p99_ns{};
    std::uint64_t p999_ns{};
    std::uint64_t max_ns{};
};

struct ThroughputStats {
    LatencyStats latency;
    double orders_per_second{};
};

inline std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double quantile) {
    if (sorted.empty()) {
        return 0;
    }
    const auto rank = static_cast<std::size_t>(std::ceil(quantile * sorted.size()));
    return sorted.at(std::min(sorted.size() - 1, std::max<std::size_t>(1, rank) - 1));
}

inline LatencyStats summarize(std::vector<std::uint64_t> samples) {
    std::sort(samples.begin(), samples.end());
    return LatencyStats{percentile(samples, 0.50), percentile(samples, 0.90), percentile(samples, 0.99),
                        percentile(samples, 0.999), samples.empty() ? 0 : samples.back()};
}

template <typename Operation, typename BetweenSamples>
LatencyStats measureLatency(std::size_t warmup,
                            std::size_t samples,
                            Operation&& operation,
                            BetweenSamples&& between_samples) {
    for (std::size_t i = 0; i < warmup; ++i) {
        operation();
        between_samples();
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        const auto started = std::chrono::steady_clock::now();
        operation();
        const auto finished = std::chrono::steady_clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
        between_samples();
    }
    return summarize(std::move(latencies));
}

inline std::string formatLatency(const LatencyStats& value) {
    return std::to_string(value.p50_ns) + "/" + std::to_string(value.p90_ns) + "/" +
           std::to_string(value.p99_ns) + "/" + std::to_string(value.p999_ns) + "/" +
           std::to_string(value.max_ns);
}

inline void printComparisonRow(const std::string& metric,
                               const LatencyStats& phase1,
                               const LatencyStats& phase2) {
    std::cout << std::left << std::setw(27) << metric << " | " << std::setw(27)
              << formatLatency(phase1) << " | " << formatLatency(phase2) << '\n';
}
