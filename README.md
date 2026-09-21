# Limit Order Book Simulator + Matching Engine

A single-threaded C++17 limit order book with price-time priority, market/limit orders,
GTC/IOC/FOK handling, correctness tests, invariant fuzzing, and a percentile-focused latency
benchmark. Prices are always signed integer ticks; the engine never compares floating-point prices.

## Build and run

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/tests
./build/bench              # default sustained run: 1,000,000 orders
./build/bench 200000       # optional smaller sustained run
```

The CMake project creates exactly these primary targets:

| Target | Kind | Purpose |
|---|---|---|
| `order_book` | static library | Phase 1 reference engine plus Phase 2 implementation headers |
| `tests` | executable | Matching behavior and invariant fuzz tests |
| `bench` | executable | Benchmarked engine sources compiled with `-O3 -DNDEBUG` (or `/O2` on MSVC) |

## Semantics

- Orders at the best eligible price fill first; within a level they fill FIFO.
- Trade price is always the resting maker's price.
- A market order matches until liquidity runs out, then discards its remainder.
- IOC matches immediately available liquidity and discards its remainder.
- FOK performs an eligible-liquidity dry run before any mutation. Insufficient liquidity returns no trades and leaves the book unchanged.
- `reduceOrder` accepts decreases only and retains position. `modifyOrder` turns a reprice or size increase into cancel + add, so it loses priority.

The engine assigns its own monotonic order sequence at ingress; callers cannot forge time priority. Duplicate IDs that are currently live are rejected without side effects. These, plus reducing to zero behaving as cancel, are documented as one-line assumptions in source comments.

## Data structures and complexity

| Operation | Phase 1: maps + lists | Phase 2: tick array + intrusive pool |
|---|---|---|
| Resting insert | `O(log P)` | `O(1)` |
| Best bid / ask | `O(1)` | `O(1)` cached index |
| Cancel / reduce | `O(1)` average | `O(1)` average |
| Match `F` resting orders | `O(F + L log P)` | `O(F + W)` to find the next occupied level |
| FOK preflight | `O(L)` | `O(L)` across eligible tick levels |
| Depth, `K` levels | `O(K)` | `O(K + W)` bitmap words |

`P` is the number of active price levels, `L` eligible levels scanned, and `F` maker orders filled. Phase 1's locator stores both the list iterator and the map iterator, so cancellation does not need a price lookup; erasing the stored map iterator is amortized constant time when it emptied a level.

Phase 2 uses a vector indexed by `price - min_price`, cached best bid/ask indices, a bitmap of occupied price levels, and a fixed-capacity intrusive doubly linked node pool. The bitmap finds the next non-empty level a 64-bit word at a time, avoiding a linear scan of an empty tick range after a level is consumed; compiler bit-scan intrinsics select the exact bit. Per-side active-level counters avoid any scan when an entire side becomes empty. Sparse `depth()` requests use the same bitmap rather than walking empty ticks. This improves locality, avoids per-order list-node allocations, and eliminates `std::map`/`std::list` from matching. The tradeoff is fixed memory proportional to the configured tick range and live-order capacity. `W` is the number of bitmap words searched (typically one; worst case `tick_range / 64`). V2 reserves its `unordered_map` buckets; an open-addressing ID index remains a possible future optimization for fully allocation-free index inserts.

## Tests

`tests` runs every required matching case for both engines:

- resting insert, full and partial fills, FIFO at a price level, and multi-level walks;
- market remainder discard, IOC no-rest behavior, and FOK no-mutation behavior;
- cancellation and priority-preserving reductions;
- deterministic 100,000-operation add/cancel/reduce fuzz tests for each engine, including random crossed-book fills;
- positive and negative FOK paths, multi-level depth, priority-losing `modifyOrder`, V2 range rejection, and atomic pool-exhaustion handling.

The fuzz harness performs complete structure checks after every operation: crossed-book validation, per-level aggregate validation, and each indexed live order's queue/node locator validation. It limits the randomized live set to 256 orders so that this exhaustive assertion remains practical at 100,000 operations.

## Benchmark method

The in-tree header-only harness uses `std::chrono::steady_clock`: it is monotonic and portable. Each microbenchmark warms up 10,000 operations, stores one nanosecond latency per timed operation in a flat vector, and reports p50, p90, p99, p99.9, and maximum latency. Resting setup for matched inserts occurs outside the timed interval; containers controlled by the harness are reserved before timing.

The sustained benchmark generates 1,000,000 limit orders after its warm-up: prices are normally distributed around a randomly drifting mid-price, and quantities follow an exponential distribution. It reports both full-run throughput and latency percentiles. The broader scenario suite adds a BBO quote lifecycle (add bid, add ask, cancel both), a 100-level market sweep, accepted and rejected FOK checks, modify/reprice churn, and sparse 10-level depth reads. These are representative matching-engine workloads rather than a vendor-specific market-data replay.

## Measured benchmark run

Recorded in this workspace with MSYS2 UCRT64 GCC 15.2.0, using the optimized `bench` configuration and its default one-million-order sustained workload. Values are `p50/p90/p99/p99.9/max`, in nanoseconds.

| Metric | Phase 1 | Phase 2 |
|---|---:|---:|
| Insert, no match | 100 / 100 / 200 / 31,400 / 86,200 | 100 / 100 / 100 / 9,700 / 34,200 |
| Insert, match 1 order | 100 / 200 / 200 / 300 / 1,000 | 100 / 100 / 200 / 600 / 12,900 |
| Insert, match 5 orders | 300 / 500 / 600 / 1,700 / 39,100 | 200 / 300 / 400 / 500 / 35,300 |
| Insert, match 50 orders | 1,700 / 2,300 / 3,100 / 7,800 / 89,300 | 1,200 / 1,700 / 2,600 / 13,000 / 131,500 |
| Cancel, arbitrary ID | 100 / 100 / 200 / 4,600 / 58,400 | 100 / 100 / 200 / 2,500 / 29,200 |
| BBO quote lifecycle (4 ops) | 200 / 300 / 300 / 400 / 7,800 | 100 / 200 / 200 / 700 / 1,100 |
| Market sweep, 100 levels | 5,500 / 7,700 / 10,700 / 33,800 / 275,700 | 2,800 / 4,000 / 5,200 / 25,100 / 80,700 |
| FOK accept, 10 levels | 600 / 900 / 1,100 / 1,900 / 5,800 | 500 / 600 / 800 / 2,600 / 232,300 |
| FOK reject, 10 levels | 0 / 100 / 100 / 100 / 500 | 100 / 100 / 100 / 100 / 5,600 |
| Modify/reprice | 100 / 200 / 300 / 300 / 1,300 | 100 / 100 / 200 / 500 / 3,600 |
| Depth, 10 sparse levels | 100 / 100 / 200 / 400 / 3,000 | 100 / 100 / 400 / 600 / 40,300 |
| Sustained `addOrder` | 100 / 300 / 700 / 4,100 / 81,400 | 100 / 200 / 500 / 2,700 / 295,200 |
| Sustained throughput | 4,824,736 orders/sec | 6,514,008 orders/sec |

Microbenchmarks, especially those near clock resolution, are noisy; use the benchmark as a comparison tool on the deployment machine rather than as a universal latency claim. The bitmap eliminates the former empty-range scan after a consumed level, so V2 now improves longer match walks and sustained throughput in this run. Both engines retain the public API's owning trade vector, which may allocate when a call produces its first fill.

## Future work

- Exchange-specific self-trade prevention (intentionally left as a TODO in the matching loop).
- Open-addressing ID index and fixed trade-output buffers for stricter allocation control.
- Multi-threaded ingress/market-data architecture around the intentionally single-threaded matching core.
