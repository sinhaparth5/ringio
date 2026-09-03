// 4KB random read/write IOPS, tail latency, and round-trip-call cost across
// queue depths and thread counts, for ringio's SQPOLL pipeline (both
// independent-ring and shared-poller) and three baselines: POSIX
// pread/pwrite, libaio, and plain io_uring without SQPOLL. Each baseline
// isolates a different cost SQPOLL is meant to remove: POSIX pays a syscall
// per op, libaio pays one per submit/reap round trip instead of per op,
// plain io_uring still submits via syscall but batches waiting, and SQPOLL
// removes the per-op syscall entirely as long as its kernel thread stays
// awake.
//
// Every backend here runs one file, one set of buffers, and (where
// applicable) one io_uring ring per thread, rather than sharing a single
// ring across threads. liburing doesn't make concurrent io_uring_submit
// calls from multiple threads safe against a shared SQ ring without extra
// locking ringio doesn't provide, so per-thread instances is the correct
// comparison, not a shortcut.
//
// BM_SqpollIops gives each thread its own SQPOLL kernel poller, which is
// what "one ring per thread" naively suggests but turns out to be wrong:
// each poller busy-spins on its own core, so a thread costs two cores, not
// one, and throughput collapses once the thread count outgrows that
// budget. BM_SqpollSharedPollerIops fixes that by having every thread past
// the first attach to the same poller instead of spawning its own -- see
// experiments/sqpoll_core_budget.cpp for how that was found, and
// SqpollEngine's `attach_to` constructor parameter for the fix.
//
// One asymmetry worth flagging when reading the numbers: SqpollEngine's
// buffers are pre-registered, page-locked, and addressed as fixed buffers,
// matching how it's meant to run; the three baselines use heap memory
// aligned only as far as O_DIRECT requires. This benchmark compares each
// backend at its own best practice, not on identical buffer handling.
//
// -----------------------------------------------------------------------
// O_DIRECT and queue depth
// -----------------------------------------------------------------------
// The scratch file opens with O_DIRECT (see MakeScratchFile), so every
// read/write here bypasses the page cache and actually reaches the block
// device instead of being served from DRAM after the first pass -- an
// earlier run of this suite without it produced 698K-1.3M "IOPS" for
// single-drive POSIX I/O, which is DRAM speed, not disk speed. See
// ROADMAP.md's "baseline comparison methodology" section for that finding.
// O_DIRECT requires every buffer, I/O length, and file offset to be
// aligned to the device's logical block size (4096 here) -- AlignedBuffer
// below and kBlockSize-multiple lengths/offsets satisfy that.
//
// libaio, plain io_uring, and both SqpollEngine backends keep `queue_depth`
// requests in flight at once (submit a batch, reap completions as they
// land, resubmit immediately) instead of the old submit-one-block-wait-one
// loop, swept across QD 1/8/32/64/128 -- see each function's comment.
// POSIX pread/pwrite doesn't get a QD axis: it's a blocking syscall with
// nothing to submit ahead of, so there's no pipeline to deepen. Its
// analogue for depth is concurrency, which its existing thread sweep
// already covers; giving it a synthetic QD parameter would just measure
// that same axis under a second name.
//
// Runtime/cost: crossing the full 1-32 thread sweep against the full QD
// sweep for four backends is 30 combinations each -- more VM time than the
// question needs. The four pipelined backends run at threads in {1, 4}
// (single-thread ceiling, and this box's full vCPU count) crossed with QD
// in {1, 8, 32, 64, 128}. POSIX keeps the full 1-32 thread sweep since it's
// cheap and has no QD dimension to cross it against.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
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
// 1 GiB of address space to scatter the "random" 4KB ops across. O_DIRECT
// is what actually keeps this suite off the page cache (see the file
// header); a working set well past a single page is defense in depth on
// top of that, not the primary mechanism.
constexpr std::uint64_t kFileBlocks = 262144;
constexpr std::uint64_t kFileBytes = kFileBlocks * kBlockSize;

// How long each pipelined burst runs before Google Benchmark's own
// iteration bookkeeping wraps around it. Short enough that the full QD x
// thread-count matrix across four backends stays a few minutes of VM time,
// long enough to hold a steady pipeline depth instead of measuring mostly
// ramp-up.
constexpr auto kBurstDuration = std::chrono::milliseconds(500);

// Bounded thread counts for the pipelined (QD-aware) backends -- see the
// file header's "Runtime/cost" note for why this doesn't cross the full
// 1-32 sweep POSIX uses.
constexpr int kPipelinedThreadCounts[] = {1, 4};

// Ring/context sizing shared by every backend that needs to hold at least
// `queue_depth` requests outstanding -- comfortably above the QD sweep's
// max (128), fixed so ring construction doesn't need runtime
// power-of-two rounding.
constexpr unsigned kRingEntries = 256;

std::uint64_t RandomBlockOffset(std::minstd_rand& rng) {
  std::uniform_int_distribution<std::uint64_t> dist(0, kFileBlocks - 1);
  return dist(rng) * kBlockSize;
}

// A page-aligned buffer sized to a multiple of kBlockSize -- O_DIRECT
// rejects reads/writes through an unaligned buffer with EINVAL, so every
// backend below that isn't already using ringio::BufferPool (which is
// aligned for its own reasons) allocates its I/O buffers through this
// instead of plain heap memory.
class AlignedBuffer {
 public:
  explicit AlignedBuffer(std::size_t size) {
    if (::posix_memalign(&ptr_, kBlockSize, size) != 0) {
      ptr_ = nullptr;
    }
  }
  ~AlignedBuffer() { std::free(ptr_); }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  char* data() { return static_cast<char*>(ptr_); }
  bool valid() const { return ptr_ != nullptr; }

 private:
  void* ptr_ = nullptr;
};

// A spin loop that never yields keeps its core 100% busy whether or not
// there's anything to do, which starves whatever else the scheduler wants
// to run on that core -- including, when threads outnumber vCPUs, a
// SQPOLL poller. CpuRelax is the cheap first tier of backoff (keeps wait
// latency low, gives the pipeline back to a hyperthread sibling without
// giving up the core); callers escalate to std::this_thread::yield() after
// enough consecutive empty passes.
#if defined(__x86_64__) || defined(__i386__)
inline void CpuRelax() { __builtin_ia32_pause(); }
#elif defined(__aarch64__)
inline void CpuRelax() { asm volatile("yield"); }
#else
inline void CpuRelax() {}
#endif

// Opens a private, pre-sized O_DIRECT scratch file for one thread to
// hammer. mkstemp doesn't take custom open flags, so this reserves a
// unique path with it, closes that handle, then reopens the same path
// with O_DIRECT -- avoiding a dependency on mkostemp (a GNU extension) for
// something this one-off. The path is unlinked immediately once reopened;
// the fd stays valid and nothing is left on disk.
//
// tmpfs (plain /tmp with no device mounted over it) doesn't support
// O_DIRECT, so this open call fails outright there instead of silently
// falling back to buffered I/O -- intentional: better to skip the
// benchmark than let it quietly go back to measuring DRAM. Mount real
// storage (e.g. a local NVMe SSD) over /tmp before running this suite; see
// CLAUDE.md's benchmark testing instructions.
int MakeScratchFile() {
  char path[] = "/tmp/ringio_bench_scratch_XXXXXX";
  const int tmp_fd = ::mkstemp(path);
  if (tmp_fd < 0) {
    return -1;
  }
  ::close(tmp_fd);
  const int fd = ::open(path, O_RDWR | O_DIRECT);
  ::unlink(path);
  if (fd < 0) {
    return -1;
  }
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

// Reports round trips (submit calls plus reap calls) per completed op.
// This is NOT a confirmed OS syscall count for the io_uring-based
// backends: liburing's io_uring_submit() only issues a real
// io_uring_enter syscall when the SQPOLL poller has gone idle (checked via
// a flag in the shared ring, not visible to this process), so whether a
// given submit call was a syscall or a plain memory write isn't something
// this in-process counter can see. For POSIX and libaio it is exact --
// pread/pwrite/io_submit/io_getevents are unconditionally syscalls. Read
// this as an upper bound on kernel entries for the io_uring backends, and
// an exact count for the other two.
void ReportCallsPerOp(benchmark::State& state, std::uint64_t calls, std::uint64_t ops) {
  state.counters["calls_per_op"] =
      ops > 0 ? static_cast<double>(calls) / static_cast<double>(ops) : 0.0;
}

// ---------------------------------------------------------------------------
// Baseline: POSIX pread/pwrite. One syscall per op -- the cost every other
// backend here is trying to avoid. No queue-depth axis; see the file
// header for why.
// ---------------------------------------------------------------------------
void BM_PosixPreadPwriteIops(benchmark::State& state) {
  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create O_DIRECT scratch file (needs real storage under /tmp)");
    return;
  }
  AlignedBuffer buf(kBlockSize);
  if (!buf.valid()) {
    state.SkipWithError("posix_memalign failed");
    ::close(fd);
    return;
  }
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  std::vector<double> latencies_us;
  bool write_turn = true;

  for (auto _ : state) {
    const std::uint64_t offset = RandomBlockOffset(rng);
    const auto start = std::chrono::steady_clock::now();
    if (write_turn) {
      benchmark::DoNotOptimize(::pwrite(fd, buf.data(), kBlockSize, static_cast<off_t>(offset)));
    } else {
      benchmark::DoNotOptimize(::pread(fd, buf.data(), kBlockSize, static_cast<off_t>(offset)));
    }
    const auto end = std::chrono::steady_clock::now();
    latencies_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
    write_turn = !write_turn;
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * kBlockSize);
  state.counters["calls_per_op"] = 1.0;
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
// Baseline: libaio, pipelined to `queue_depth` in flight. Submits a batch,
// blocks for at least one completion, drains whatever else has landed,
// then immediately resubmits the freed slots -- so the per-round-trip
// io_submit/io_getevents cost is amortized across the batch instead of
// paid once per op.
// ---------------------------------------------------------------------------
void BM_LibaioIops(benchmark::State& state) {
  const auto queue_depth = static_cast<unsigned>(state.range(0));

  ::io_context_t ctx{};
  if (::io_setup(static_cast<int>(queue_depth), &ctx) != 0) {
    state.SkipWithError("io_setup failed");
    return;
  }
  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create O_DIRECT scratch file (needs real storage under /tmp)");
    ::io_destroy(ctx);
    return;
  }

  std::vector<std::unique_ptr<AlignedBuffer>> buffers;
  buffers.reserve(queue_depth);
  for (unsigned i = 0; i < queue_depth; ++i) {
    buffers.push_back(std::make_unique<AlignedBuffer>(kBlockSize));
    if (!buffers.back()->valid()) {
      state.SkipWithError("posix_memalign failed");
      ::close(fd);
      ::io_destroy(ctx);
      return;
    }
  }
  std::vector<::iocb> cbs(queue_depth);
  std::vector<::iocb*> cb_ptrs(queue_depth);
  std::vector<std::chrono::steady_clock::time_point> submitted_at(queue_depth);
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  bool write_turn = true;
  std::uint64_t ops = 0;
  std::uint64_t calls = 0;
  std::vector<double> latencies_us;

  const auto prep = [&](unsigned slot) {
    const std::uint64_t offset = RandomBlockOffset(rng);
    if (write_turn) {
      ::io_prep_pwrite(&cbs[slot], fd, buffers[slot]->data(), kBlockSize,
                        static_cast<long long>(offset));
    } else {
      ::io_prep_pread(&cbs[slot], fd, buffers[slot]->data(), kBlockSize,
                       static_cast<long long>(offset));
    }
    cbs[slot].data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot));
    cb_ptrs[slot] = &cbs[slot];
    submitted_at[slot] = std::chrono::steady_clock::now();
    write_turn = !write_turn;
  };

  for (auto _ : state) {
    for (unsigned i = 0; i < queue_depth; ++i) {
      prep(i);
    }
    ::io_submit(ctx, static_cast<long>(queue_depth), cb_ptrs.data());
    ++calls;

    const auto deadline = std::chrono::steady_clock::now() + kBurstDuration;
    std::vector<::io_event> events(queue_depth);
    while (std::chrono::steady_clock::now() < deadline) {
      const int reaped =
          ::io_getevents(ctx, 1, static_cast<long>(queue_depth), events.data(), nullptr);
      ++calls;
      if (reaped <= 0) {
        continue;
      }
      const auto now = std::chrono::steady_clock::now();
      std::vector<::iocb*> refill;
      refill.reserve(static_cast<std::size_t>(reaped));
      for (int e = 0; e < reaped; ++e) {
        const auto slot = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(events[e].data));
        latencies_us.push_back(
            std::chrono::duration<double, std::micro>(now - submitted_at[slot]).count());
        ++ops;
        prep(slot);
        refill.push_back(&cbs[slot]);
      }
      ::io_submit(ctx, static_cast<long>(refill.size()), refill.data());
      ++calls;
    }
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  state.SetBytesProcessed(static_cast<std::int64_t>(ops) * kBlockSize);
  ReportCallsPerOp(state, calls, ops);
  ::close(fd);
  ::io_destroy(ctx);
}
BENCHMARK(BM_LibaioIops)
    ->Threads(kPipelinedThreadCounts[0])
    ->Threads(kPipelinedThreadCounts[1])
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(128)
    ->UseRealTime();
#endif  // RINGIO_HAVE_LIBAIO

#if defined(RINGIO_HAVE_LIBURING)
// ---------------------------------------------------------------------------
// Baseline: plain io_uring, no SQPOLL, pipelined to `queue_depth` in
// flight. Submission still costs a syscall (io_uring_submit) and so does
// waiting (io_uring_wait_cqe_nr); this isolates what SQPOLL alone buys
// over stock io_uring at the same queue depth, separate from the
// fixed-file/fixed-buffer registration ringio also does.
// ---------------------------------------------------------------------------
void BM_PlainIoUringIops(benchmark::State& state) {
  const auto queue_depth = static_cast<unsigned>(state.range(0));

  ::io_uring ring{};
  if (::io_uring_queue_init(kRingEntries, &ring, 0) < 0) {
    state.SkipWithError("io_uring_queue_init failed");
    return;
  }
  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create O_DIRECT scratch file (needs real storage under /tmp)");
    ::io_uring_queue_exit(&ring);
    return;
  }

  std::vector<std::unique_ptr<AlignedBuffer>> buffers;
  buffers.reserve(queue_depth);
  for (unsigned i = 0; i < queue_depth; ++i) {
    buffers.push_back(std::make_unique<AlignedBuffer>(kBlockSize));
    if (!buffers.back()->valid()) {
      state.SkipWithError("posix_memalign failed");
      ::close(fd);
      ::io_uring_queue_exit(&ring);
      return;
    }
  }
  std::vector<std::chrono::steady_clock::time_point> submitted_at(queue_depth);
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  bool write_turn = true;
  std::uint64_t ops = 0;
  std::uint64_t calls = 0;
  std::vector<double> latencies_us;

  const auto prep = [&](unsigned slot) {
    const std::uint64_t offset = RandomBlockOffset(rng);
    ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
    if (write_turn) {
      ::io_uring_prep_write(sqe, fd, buffers[slot]->data(), kBlockSize,
                             static_cast<off_t>(offset));
    } else {
      ::io_uring_prep_read(sqe, fd, buffers[slot]->data(), kBlockSize,
                            static_cast<off_t>(offset));
    }
    ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot)));
    submitted_at[slot] = std::chrono::steady_clock::now();
    write_turn = !write_turn;
  };

  for (auto _ : state) {
    for (unsigned i = 0; i < queue_depth; ++i) {
      prep(i);
    }
    ::io_uring_submit(&ring);
    ++calls;

    const auto deadline = std::chrono::steady_clock::now() + kBurstDuration;
    while (std::chrono::steady_clock::now() < deadline) {
      ::io_uring_cqe* cqe = nullptr;
      if (::io_uring_wait_cqe_nr(&ring, &cqe, 1) < 0) {
        continue;
      }
      ++calls;
      unsigned refilled = 0;
      while (true) {
        ::io_uring_cqe* peeked = nullptr;
        if (::io_uring_peek_cqe(&ring, &peeked) < 0) {
          break;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto slot =
            static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(::io_uring_cqe_get_data(peeked)));
        latencies_us.push_back(
            std::chrono::duration<double, std::micro>(now - submitted_at[slot]).count());
        ::io_uring_cqe_seen(&ring, peeked);
        ++ops;
        prep(slot);
        ++refilled;
      }
      if (refilled > 0) {
        ::io_uring_submit(&ring);
        ++calls;
      }
    }
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  state.SetBytesProcessed(static_cast<std::int64_t>(ops) * kBlockSize);
  ReportCallsPerOp(state, calls, ops);
  ::close(fd);
  ::io_uring_queue_exit(&ring);
}
BENCHMARK(BM_PlainIoUringIops)
    ->Threads(kPipelinedThreadCounts[0])
    ->Threads(kPipelinedThreadCounts[1])
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(128)
    ->UseRealTime();

// ---------------------------------------------------------------------------
// ringio's own pipeline: SqpollEngine, fixed files, fixed buffers, kept at
// `queue_depth` in flight. Each of the pool's `queue_depth` buffers is
// acquired once up front and never released back to the free list -- its
// slot index doubles as both the fixed-buffer index and the completion
// token, so a landed completion can be resubmitted at the same slot with
// no free-list round trip. harvest_completions/drain_and_submit are pure
// mmap-ring operations in steady state, not confirmed syscalls; see
// ReportCallsPerOp.
// ---------------------------------------------------------------------------
void BM_SqpollIops(benchmark::State& state) {
  const auto queue_depth = static_cast<unsigned>(state.range(0));

  std::optional<ringio::SqpollEngine> engine;
  try {
    engine.emplace(kRingEntries);
  } catch (const std::system_error&) {
    state.SkipWithError("SQPOLL io_uring unavailable in this environment");
    return;
  }

  const int fd = MakeScratchFile();
  if (fd < 0) {
    state.SkipWithError("failed to create O_DIRECT scratch file (needs real storage under /tmp)");
    return;
  }
  if (engine->register_files(std::span<const int>(&fd, 1)) != 0) {
    state.SkipWithError("register_files failed");
    ::close(fd);
    return;
  }
  ringio::BufferPool pool(queue_depth, kBlockSize);
  if (engine->register_buffers(pool) != 0) {
    state.SkipWithError("register_buffers failed");
    ::close(fd);
    return;
  }

  std::vector<std::uint32_t> slot_buf(queue_depth);
  for (unsigned i = 0; i < queue_depth; ++i) {
    slot_buf[i] = pool.acquire();
  }

  ringio::detail::SpscRing<ringio::IoRequest, 128> submit_queue;
  ringio::detail::SpscRing<ringio::IoCompletion, 128> completions;
  std::vector<std::chrono::steady_clock::time_point> submitted_at(queue_depth);
  std::minstd_rand rng(static_cast<unsigned>(state.thread_index() + 1));
  bool write_turn = true;
  std::uint64_t ops = 0;
  std::uint64_t calls = 0;
  std::vector<double> latencies_us;

  const auto make_request = [&](std::uint32_t buf_index) {
    ringio::IoRequest req{};
    req.op = write_turn ? ringio::IoOp::kWrite : ringio::IoOp::kRead;
    req.fixed_fd_index = 0;
    req.buffer_index = buf_index;
    req.offset = RandomBlockOffset(rng);
    req.length = kBlockSize;
    req.token = buf_index;
    write_turn = !write_turn;
    return req;
  };

  for (auto _ : state) {
    for (unsigned i = 0; i < queue_depth; ++i) {
      submit_queue.try_push(make_request(slot_buf[i]));
      submitted_at[slot_buf[i]] = std::chrono::steady_clock::now();
    }
    engine->drain_and_submit(submit_queue, pool, queue_depth);
    ++calls;

    const auto deadline = std::chrono::steady_clock::now() + kBurstDuration;
    unsigned idle_spins = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const std::size_t delivered = engine->harvest_completions(completions, queue_depth);
      ringio::IoCompletion completion{};
      unsigned refilled = 0;
      while (completions.try_pop(completion)) {
        const auto slot = static_cast<std::uint32_t>(completion.token);
        const auto now = std::chrono::steady_clock::now();
        latencies_us.push_back(
            std::chrono::duration<double, std::micro>(now - submitted_at[slot]).count());
        ++ops;
        submit_queue.try_push(make_request(slot));
        submitted_at[slot] = now;
        ++refilled;
      }
      if (refilled > 0) {
        engine->drain_and_submit(submit_queue, pool, queue_depth);
        ++calls;
        idle_spins = 0;
      } else if (delivered == 0) {
        if (++idle_spins > 256) {
          std::this_thread::yield();
          idle_spins = 0;
        } else {
          CpuRelax();
        }
      }
    }
  }

  ReportPercentiles(state, latencies_us);
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  state.SetBytesProcessed(static_cast<std::int64_t>(ops) * kBlockSize);
  ReportCallsPerOp(state, calls, ops);
  ::close(fd);
}
BENCHMARK(BM_SqpollIops)
    ->Threads(kPipelinedThreadCounts[0])
    ->Threads(kPipelinedThreadCounts[1])
    ->Arg(1)
    ->Arg(8)
    ->Arg(32)
    ->Arg(64)
    ->Arg(128)
    ->UseRealTime();

// ---------------------------------------------------------------------------
// Same pipeline as BM_SqpollIops, but N-1 of the engines attach to the
// Nth's kernel poller instead of spawning their own (SqpollEngine's
// `attach_to` constructor parameter) -- see that header comment and
// experiments/sqpoll_core_budget.cpp for why. Independent rings collapse
// once the thread count outgrows the box's core budget; sharing one poller
// keeps this scaling well past it.
//
// This doesn't use ->Threads(N). A shared-poller run needs the master
// engine built before any follower can attach to it, and ->Threads(N)'s
// per-thread setup runs concurrently with no ordering guarantee between
// threads, so it can't express that dependency without synchronization of
// its own. Instead this function builds all N engines itself, then fans
// out its own std::threads to drive them, the same structure
// experiments/sqpoll_core_budget.cpp used to answer this question in the
// first place. range(0) is the thread count and range(1) the queue depth,
// standing in for ->Threads(N)->Arg(QD) in the registration below.
// ---------------------------------------------------------------------------
void BM_SqpollSharedPollerIops(benchmark::State& state) {
  const int num_threads = static_cast<int>(state.range(0));
  const auto queue_depth = static_cast<unsigned>(state.range(1));

  std::optional<ringio::SqpollEngine> master;
  try {
    master.emplace(kRingEntries);
  } catch (const std::system_error&) {
    state.SkipWithError("SQPOLL io_uring unavailable in this environment");
    return;
  }
  std::vector<std::unique_ptr<ringio::SqpollEngine>> followers;
  followers.reserve(static_cast<std::size_t>(num_threads - 1));
  for (int i = 1; i < num_threads; ++i) {
    followers.push_back(std::make_unique<ringio::SqpollEngine>(kRingEntries, 1000, &*master));
  }

  // One BufferPool/ring set per thread, same as every other backend here --
  // only the poller is shared, not the SQ/CQ or the buffers. unique_ptr
  // because BufferPool and SpscRing are neither copyable nor movable, so
  // they can't live directly in a std::vector that might reallocate.
  struct ThreadContext {
    ringio::SqpollEngine* engine;
    int fd;
    ringio::BufferPool pool;
    std::vector<std::uint32_t> slot_buf;
    ringio::detail::SpscRing<ringio::IoRequest, 128> submit_queue;
    ringio::detail::SpscRing<ringio::IoCompletion, 128> completions;
    std::vector<std::chrono::steady_clock::time_point> submitted_at;
    std::minstd_rand rng;
    std::vector<double> latencies_us;
    std::uint64_t ops = 0;
    std::uint64_t calls = 0;
    bool write_turn = true;

    ThreadContext(ringio::SqpollEngine* e, int f, unsigned qd, unsigned seed)
        : engine(e), fd(f), pool(qd, kBlockSize), submitted_at(qd), rng(seed) {}
  };

  std::vector<std::unique_ptr<ThreadContext>> contexts;
  contexts.reserve(static_cast<std::size_t>(num_threads));
  for (int i = 0; i < num_threads; ++i) {
    ringio::SqpollEngine* engine =
        (i == 0) ? &*master : followers[static_cast<std::size_t>(i - 1)].get();
    const int fd = MakeScratchFile();
    if (fd < 0) {
      state.SkipWithError("failed to create O_DIRECT scratch file (needs real storage under /tmp)");
      return;
    }
    if (engine->register_files(std::span<const int>(&fd, 1)) != 0) {
      state.SkipWithError("register_files failed");
      ::close(fd);
      return;
    }
    contexts.push_back(
        std::make_unique<ThreadContext>(engine, fd, queue_depth, static_cast<unsigned>(i + 1)));
    if (engine->register_buffers(contexts.back()->pool) != 0) {
      state.SkipWithError("register_buffers failed");
      return;
    }
    contexts.back()->slot_buf.resize(queue_depth);
    for (unsigned s = 0; s < queue_depth; ++s) {
      contexts.back()->slot_buf[s] = contexts.back()->pool.acquire();
    }
  }

  for (auto _ : state) {
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(num_threads));
    for (int i = 0; i < num_threads; ++i) {
      workers.emplace_back([&contexts, i, queue_depth] {
        ThreadContext& ctx = *contexts[static_cast<std::size_t>(i)];

        const auto make_request = [&](std::uint32_t buf_index) {
          ringio::IoRequest req{};
          req.op = ctx.write_turn ? ringio::IoOp::kWrite : ringio::IoOp::kRead;
          req.fixed_fd_index = 0;
          req.buffer_index = buf_index;
          req.offset = RandomBlockOffset(ctx.rng);
          req.length = kBlockSize;
          req.token = buf_index;
          ctx.write_turn = !ctx.write_turn;
          return req;
        };

        for (unsigned s = 0; s < queue_depth; ++s) {
          ctx.submit_queue.try_push(make_request(ctx.slot_buf[s]));
          ctx.submitted_at[ctx.slot_buf[s]] = std::chrono::steady_clock::now();
        }
        ctx.engine->drain_and_submit(ctx.submit_queue, ctx.pool, queue_depth);
        ++ctx.calls;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        unsigned idle_spins = 0;
        while (std::chrono::steady_clock::now() < deadline) {
          const std::size_t delivered = ctx.engine->harvest_completions(ctx.completions, queue_depth);
          ringio::IoCompletion completion{};
          unsigned refilled = 0;
          while (ctx.completions.try_pop(completion)) {
            const auto slot = static_cast<std::uint32_t>(completion.token);
            const auto now = std::chrono::steady_clock::now();
            ctx.latencies_us.push_back(
                std::chrono::duration<double, std::micro>(now - ctx.submitted_at[slot]).count());
            ++ctx.ops;
            ctx.submit_queue.try_push(make_request(slot));
            ctx.submitted_at[slot] = now;
            ++refilled;
          }
          if (refilled > 0) {
            ctx.engine->drain_and_submit(ctx.submit_queue, ctx.pool, queue_depth);
            ++ctx.calls;
            idle_spins = 0;
          } else if (delivered == 0) {
            if (++idle_spins > 256) {
              std::this_thread::yield();
              idle_spins = 0;
            } else {
              CpuRelax();
            }
          }
        }
      });
    }
    for (auto& t : workers) t.join();
  }

  std::uint64_t total_ops = 0;
  std::uint64_t total_calls = 0;
  std::vector<double> all_latencies_us;
  for (const auto& ctx : contexts) {
    total_ops += ctx->ops;
    total_calls += ctx->calls;
    all_latencies_us.insert(all_latencies_us.end(), ctx->latencies_us.begin(),
                             ctx->latencies_us.end());
  }
  ReportPercentiles(state, all_latencies_us);
  state.SetItemsProcessed(static_cast<std::int64_t>(total_ops));
  state.SetBytesProcessed(static_cast<std::int64_t>(total_ops) * kBlockSize);
  ReportCallsPerOp(state, total_calls, total_ops);

  for (const auto& ctx : contexts) ::close(ctx->fd);
}
BENCHMARK(BM_SqpollSharedPollerIops)
    ->ArgNames({"threads", "queue_depth"})
    ->ArgsProduct({{kPipelinedThreadCounts[0], kPipelinedThreadCounts[1]}, {1, 8, 32, 64, 128}})
    ->UseRealTime();
#endif  // RINGIO_HAVE_LIBURING

}  // namespace
