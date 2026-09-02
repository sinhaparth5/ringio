#include "ringio/sqpoll_engine.hpp"

// SqpollEngine, and therefore this whole file's content, only exists when
// liburing was found at configure time.
#if defined(RINGIO_HAVE_LIBURING)

#include <optional>
#include <system_error>

#include <benchmark/benchmark.h>

#include "ringio/detail/spsc_ring.hpp"
#include "ringio/io_completion.hpp"

namespace {

using ringio::IoCompletion;
using ringio::SqpollEngine;
using ringio::detail::SpscRing;

// Steady-state hot-path cost of polling for completions when none are
// pending: the common case for a worker checking in between submitting new
// requests. io_uring_for_each_cqe walks zero entries and the CQ tail is
// never touched, so this is close to harvest_completions()'s floor.
void BM_HarvestCompletionsEmpty(benchmark::State& state) {
  std::optional<SqpollEngine> engine;
  try {
    engine.emplace(32);
  } catch (const std::system_error&) {
    state.SkipWithError("SQPOLL io_uring unavailable in this environment");
    return;
  }

  SpscRing<IoCompletion, 32> completions;
  for (auto _ : state) {
    benchmark::DoNotOptimize(engine->harvest_completions(completions));
  }
}
BENCHMARK(BM_HarvestCompletionsEmpty);

}  // namespace

#endif  // RINGIO_HAVE_LIBURING
