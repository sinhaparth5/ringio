// 4KB random read/write IOPS scaling across 1-32 threads, plus tail latency
// tracking (p50/p95/p99/p99.9), for ringio's SQPOLL pipeline and three
// baselines: POSIX pread/pwrite, libaio, and plain io_uring without SQPOLL.
// Each baseline isolates a different cost SQPOLL is meant to remove: POSIX
// pays a syscall per op, libaio pays one per submit/reap round trip instead
// of per op, plain io_uring still submits via syscall but batches waiting,
// and SQPOLL removes the per-op syscall entirely as long as its kernel
// thread stays awake.
//
// Every backend here runs one file, one set of buffers, and (where
// applicable) one io_uring ring per thread, rather than sharing a single
// ring across threads. liburing doesn't make concurrent io_uring_submit
// calls from multiple threads safe against a shared SQ ring without extra
// locking ringio doesn't provide, so per-thread instances is the correct
// comparison, not a shortcut: it mirrors the "one ring per core" deployment
// pattern these engines are actually used in.
//
// One asymmetry worth flagging when reading the numbers: SqpollEngine's
// buffers are pre-registered, page-locked, and addressed as fixed buffers,
// matching how it's meant to run; the three baselines use plain heap memory
// and buffered I/O, matching how those APIs are normally used. This
// benchmark compares each backend at its own best practice, not on
// identical buffer handling.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <benchmark/benchmark.h>

#if defined(RINGIO_HAVE_LIBURING)
#include <liburing.h>
#include <optional>
#include <span>
#include <system_error>

#include "ringio/detail/buffer_pool.hpp"
#include "ringio/detail/spsc_ring.hpp"
#include "ringio/io_completion.hpp"
#include "ringio/io_request.hpp"
#include "ringio/sqpoll_engine.hpp"
#endif

#if defined(RINGIO_HAVE_LIBAIO)
#include <libaio.h>
#endif

namespace {

constexpr std::uint32_t kBlockSize = 4096;
// 64 MiB of address space to scatter the "random" 4KB ops across, so
// consecutive ops on the same thread don't land on the same page and let
// the page cache turn this into a no-op benchmark.
constexpr std::uint64_t kFileBlocks = 16384;
constexpr std::uint64_t kFileBytes = kFileBlocks * kBlockSize;

std::uint64_t RandomBlockOffset(std::minstd_rand& rng) {
  std::uniform_int_distribution<std::uint64_t> dist(0, kFileBlocks - 1);
  return dist(rng) * kBlockSize;
}

// Opens a private, pre-sized scratch file for one thread to hammer. The
// path is unlinked immediately -- the fd stays valid and nothing is left on
// disk, same pattern as the sqpoll_engine tests.
int MakeScratchFile() {
  char path[] = "/tmp/ringio_bench_scratch_XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd < 0) {
    return -1;
  }
  ::unlink(path);
  if (::ftruncate(fd, static_cast<off_t>(kFileBytes)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Turns a thread's collected per-op latencies into p50/p95/p99/p99.9
// counters (microseconds). kAvgThreads makes multi-thread runs report the
// average of each thread's own percentiles rather than summing them, which
// is the closer-to-meaningful reading for a latency figure, though it's
// still an approximation -- a true cross-thread percentile would need every
// thread's raw samples pooled before sorting, which Google Benchmark's
// per-thread counter model doesn't give us.
void ReportPercentiles(benchmark::State& state, std::vector<double>& latencies_us) {
  if (latencies_us.empty()) {
    return;
  }
  std::sort(latencies_us.begin(), latencies_us.end());
  const auto at = [&](double p) {
    const auto idx = static_cast<std::size_t>(p * static_cast<double>(latencies_us.size() - 1));
    return latencies_us[idx];
  };
  state.counters["p50_us"] = benchmark::Counter(at(0.50), benchmark::Counter::kAvgThreads);
  state.counters["p95_us"] = benchmark::Counter(at(0.95), benchmark::Counter::kAvgThreads);
  state.counters["p99_us"] = benchmark::Counter(at(0.99), benchmark::Counter::kAvgThreads);
  state.counters["p99_9_us"] = benchmark::Counter(at(0.999), benchmark::Counter::kAvgThreads);
}

// ---------------------------------------------------------------------------
// Baseline: POSIX pread/pwrite. One syscall per op -- the cost every other
// backend here is trying to avoid.
// ---------------------------------------------------------------------------
void BM_PosixPreadPwriteIops(benchmark::State& state) {
  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create scratch file");
    return;
  }
  std::vector<char> buf(kBlockSize, 'x');
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  std::vector<double> latencies_us;
  bool write_turn = true;

  for (auto _ : state) {
    const std::uint64_t offset = RandomBlockOffset(rng);
    const auto start = std::chrono::steady_clock::now();
    if (write_turn) {
      benchmark::DoNotOptimize(
          ::pwrite(fd, buf.data(), kBlockSize, static_cast<off_t>(offset)));
    } else {
      benchmark::DoNotOptimize(
          ::pread(fd, buf.data(), kBlockSize, static_cast<off_t>(offset)));
    }
    const auto end = std::chrono::steady_clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    write_turn = !write_turn;
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * kBlockSize);
  ::close(fd);
}
BENCHMARK(BM_PosixPreadPwriteIops)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->Threads(32)
    ->UseRealTime();

#if defined(RINGIO_HAVE_LIBAIO)
// ---------------------------------------------------------------------------
// Baseline: libaio. One io_submit/io_getevents round trip per op -- still
// synchronous at queue depth 1 like the others here, but the kernel work
// happens through the AIO ring instead of a blocking syscall doing the I/O
// inline.
// ---------------------------------------------------------------------------
void BM_LibaioIops(benchmark::State& state) {
  ::io_context_t ctx{};
  if (::io_setup(8, &ctx) != 0) {
    state.SkipWithError("io_setup failed");
    return;
  }
  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create scratch file");
    ::io_destroy(ctx);
    return;
  }
  std::vector<char> buf(kBlockSize, 'x');
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  std::vector<double> latencies_us;
  bool write_turn = true;

  for (auto _ : state) {
    const std::uint64_t offset = RandomBlockOffset(rng);
    ::iocb cb{};
    ::iocb* cbs[1] = {&cb};
    if (write_turn) {
      ::io_prep_pwrite(&cb, fd, buf.data(), kBlockSize, static_cast<long long>(offset));
    } else {
      ::io_prep_pread(&cb, fd, buf.data(), kBlockSize, static_cast<long long>(offset));
    }

    const auto start = std::chrono::steady_clock::now();
    ::io_submit(ctx, 1, cbs);
    ::io_event event{};
    ::io_getevents(ctx, 1, 1, &event, nullptr);
    const auto end = std::chrono::steady_clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    write_turn = !write_turn;
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * kBlockSize);
  ::close(fd);
  ::io_destroy(ctx);
}
BENCHMARK(BM_LibaioIops)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->Threads(32)
    ->UseRealTime();
#endif  // RINGIO_HAVE_LIBAIO

#if defined(RINGIO_HAVE_LIBURING)
// ---------------------------------------------------------------------------
// Baseline: plain io_uring, no SQPOLL. Submission still costs a syscall
// (io_uring_submit) and so does waiting (io_uring_wait_cqe); this isolates
// what SQPOLL alone buys over stock io_uring, separate from the fixed-file/
// fixed-buffer registration ringio also does.
// ---------------------------------------------------------------------------
void BM_PlainIoUringIops(benchmark::State& state) {
  ::io_uring ring{};
  if (::io_uring_queue_init(64, &ring, 0) < 0) {
    state.SkipWithError("io_uring_queue_init failed");
    return;
  }
  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create scratch file");
    ::io_uring_queue_exit(&ring);
    return;
  }
  std::vector<char> buf(kBlockSize, 'x');
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  std::vector<double> latencies_us;
  bool write_turn = true;

  for (auto _ : state) {
    const std::uint64_t offset = RandomBlockOffset(rng);
    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
    if (write_turn) {
      ::io_uring_prep_write(sqe, fd, buf.data(), kBlockSize, static_cast<off_t>(offset));
    } else {
      ::io_uring_prep_read(sqe, fd, buf.data(), kBlockSize, static_cast<off_t>(offset));
    }

    const auto start = std::chrono::steady_clock::now();
    ::io_uring_submit(&ring);
    ::io_uring_cqe* cqe = nullptr;
    ::io_uring_wait_cqe(&ring, &cqe);
    ::io_uring_cqe_seen(&ring, cqe);
    const auto end = std::chrono::steady_clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    write_turn = !write_turn;
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * kBlockSize);
  ::close(fd);
  ::io_uring_queue_exit(&ring);
}
BENCHMARK(BM_PlainIoUringIops)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->Threads(32)
    ->UseRealTime();

// ---------------------------------------------------------------------------
// ringio's own pipeline: SqpollEngine, fixed files, fixed buffers. Queue
// depth 1 -- push a request, drain it into the SQ ring, then spin
// harvest_completions until it comes back -- so what's measured is
// round-trip completion latency, not queueing behavior behind a deeper
// pipeline.
// ---------------------------------------------------------------------------
void BM_SqpollIops(benchmark::State& state) {
  std::optional<ringio::SqpollEngine> engine;
  try {
    engine.emplace(64);
  } catch (const std::system_error&) {
    state.SkipWithError("SQPOLL io_uring unavailable in this environment");
    return;
  }

  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create scratch file");
    return;
  }
  if (engine->register_files(std::span<const int>(&fd, 1)) != 0) {
    state.SkipWithError("register_files failed");
    ::close(fd);
    return;
  }
  ringio::BufferPool pool(2, kBlockSize);
  if (engine->register_buffers(pool) != 0) {
    state.SkipWithError("register_buffers failed");
    ::close(fd);
    return;
  }

  ringio::detail::SpscRing<ringio::IoRequest, 8> submit_queue;
  ringio::detail::SpscRing<ringio::IoCompletion, 8> completions;
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  std::vector<double> latencies_us;
  std::uint64_t token = 0;
  bool write_turn = true;

  for (auto _ : state) {
    const std::uint32_t buf = pool.acquire();
    ringio::IoRequest req{};
    req.op = write_turn ? ringio::IoOp::kWrite : ringio::IoOp::kRead;
    req.fixed_fd_index = 0;
    req.buffer_index = buf;
    req.offset = RandomBlockOffset(rng);
    req.length = kBlockSize;
    req.token = ++token;

    const auto start = std::chrono::steady_clock::now();
    submit_queue.try_push(req);
    engine->drain_and_submit(submit_queue, pool);
    ringio::IoCompletion completion{};
    bool got = false;
    while (!got) {
      engine->harvest_completions(completions);
      got = completions.try_pop(completion);
    }
    const auto end = std::chrono::steady_clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());

    pool.release(buf);
    write_turn = !write_turn;
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * kBlockSize);
  ::close(fd);
}
BENCHMARK(BM_SqpollIops)
    ->Threads(1)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->Threads(32)
    ->UseRealTime();
#endif  // RINGIO_HAVE_LIBURING

}  // namespace
