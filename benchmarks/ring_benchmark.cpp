#include "ringio/detail/mpmc_ring.hpp"
#include "ringio/detail/spsc_ring.hpp"

#include <cstdint>

#include <benchmark/benchmark.h>

namespace {

using ringio::detail::MpmcRing;
using ringio::detail::SpscRing;

// Single-threaded push/pop round trip: no contention, so this is close to
// each ring's best case. The gap between the two here is pure algorithm
// overhead (Vyukov's per-slot sequence CAS vs. a plain load/store pair).
void BM_SpscRingPushPop(benchmark::State& state) {
  SpscRing<std::uint64_t, 1024> ring;
  for (auto _ : state) {
    ring.try_push(1);
    std::uint64_t out = 0;
    benchmark::DoNotOptimize(ring.try_pop(out));
  }
}
BENCHMARK(BM_SpscRingPushPop);

void BM_MpmcRingPushPop(benchmark::State& state) {
  MpmcRing<std::uint64_t, 1024> ring;
  for (auto _ : state) {
    ring.try_push(1);
    std::uint64_t out = 0;
    benchmark::DoNotOptimize(ring.try_pop(out));
  }
}
BENCHMARK(BM_MpmcRingPushPop);

// MpmcRing under real contention: threads share one ring, each pushing and
// immediately popping (its own or another thread's item — the ring doesn't
// pair them up), so both cursors are hot across cores.
void BM_MpmcRingPushPopContended(benchmark::State& state) {
  static MpmcRing<std::uint64_t, 1024> ring;
  for (auto _ : state) {
    while (!ring.try_push(1)) {
    }
    std::uint64_t out = 0;
    while (!ring.try_pop(out)) {
    }
    benchmark::DoNotOptimize(out);
  }
}
BENCHMARK(BM_MpmcRingPushPopContended)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

}  // namespace
