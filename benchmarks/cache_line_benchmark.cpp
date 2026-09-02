#include "ringio/detail/cache_line.hpp"

#include <atomic>
#include <cstdint>

#include <benchmark/benchmark.h>

namespace {

// A scaffolding benchmark: exercises the build/link path for Phase 1. Real
// false-sharing vs. padded-counter throughput comparisons land once
// Phase 3's submission ring gives us real head/tail pointers to contend on.
void BM_CacheLinePaddedIncrement(benchmark::State& state) {
  ringio::detail::CacheLinePadded<std::atomic<std::uint64_t>> counter(0);
  for (auto _ : state) {
    counter.get().fetch_add(1, std::memory_order_relaxed);
  }
}
BENCHMARK(BM_CacheLinePaddedIncrement);

}  // namespace

BENCHMARK_MAIN();
