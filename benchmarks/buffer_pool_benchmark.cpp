#include "ringio/detail/buffer_pool.hpp"

#include <cstdint>

#include <benchmark/benchmark.h>

namespace {

using ringio::BufferPool;

// Single-threaded acquire/release round trip: the pool never contends with
// itself here, so this is close to the CAS's best case.
void BM_BufferPoolAcquireRelease(benchmark::State& state) {
  BufferPool pool(64, 4096);
  for (auto _ : state) {
    std::uint32_t index = pool.acquire();
    benchmark::DoNotOptimize(index);
    pool.release(index);
  }
}
BENCHMARK(BM_BufferPoolAcquireRelease);

// Same round trip under contention: threads share one pool sized well
// below the thread count, so most acquires race the free list.
void BM_BufferPoolAcquireReleaseContended(benchmark::State& state) {
  static BufferPool pool(8, 4096);
  for (auto _ : state) {
    std::uint32_t index;
    do {
      index = pool.acquire();
    } while (index == BufferPool::kInvalidIndex);
    benchmark::DoNotOptimize(index);
    pool.release(index);
  }
}
BENCHMARK(BM_BufferPoolAcquireReleaseContended)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

}  // namespace
