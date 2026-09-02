// Standalone investigation harness for the SQPOLL core-budget question
// found while reading Phase 5's IOPS results: an SqpollEngine's kernel
// poller thread busy-polls continuously, so it costs a full extra core per
// ring, not a share of one. This tests whether that cost is fixed or
// avoidable, before anything gets changed in the library itself.
//
// It talks to raw liburing directly rather than going through
// ringio::SqpollEngine, because the three things under test --
// IORING_SETUP_ATTACH_WQ, sq_thread_idle, and IORING_SETUP_SQ_AFF /
// sq_thread_cpu pinning -- aren't parameters SqpollEngine exposes today.
// The point of this program is to find out whether any of them are worth
// exposing, not to exercise the library as it stands.
//
// Usage: sqpoll_core_budget <mode> <threads> [idle_ms]
//
//   baseline  N independent SQPOLL rings, one kernel poller each, no
//             pinning, sq_thread_idle=1000 (SqpollEngine's own default).
//             Reproduces the Phase 5 collapse as a same-run control.
//   attach    Thread 0 owns a normal SQPOLL ring; threads 1..N-1 attach to
//             it with IORING_SETUP_ATTACH_WQ, sharing its kernel poller
//             instead of spawning their own.
//   idle      N independent rings like baseline, but sq_thread_idle comes
//             from the idle_ms argument instead of defaulting to 1000.
//   pinned    N independent rings, but the application thread for ring i
//             is pinned to core 2*i and its poller to core 2*i+1 via
//             IORING_SETUP_SQ_AFF, so the "one ring costs two cores"
//             budget is enforced instead of left to the scheduler.
//
// Each thread owns its own 64MB scratch file (mkstemp'd, unlinked
// immediately) and does single-outstanding-op writes/reads of a 4KB block
// at random offsets -- the same methodology as BM_SqpollIops in
// benchmarks/iops_throughput_benchmark.cpp, so the numbers line up with
// the Phase 5 results. Output is one CSV line per run:
// mode,threads,idle_ms,total_iops,avg_p50_us,avg_p99_us

#include <liburing.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace {

constexpr std::uint32_t kBlockSize = 4096;
constexpr std::uint64_t kFileBlocks = 16384;
constexpr std::uint64_t kFileBytes = kFileBlocks * kBlockSize;
constexpr auto kRunDuration = std::chrono::seconds(3);

int MakeScratchFile() {
  char path[] = "/tmp/ringio_experiment_scratch_XXXXXX";
  const int fd = ::mkstemp(path);
  if (fd < 0) {
    std::perror("mkstemp");
    std::exit(1);
  }
  ::unlink(path);
  if (::ftruncate(fd, static_cast<off_t>(kFileBytes)) != 0) {
    std::perror("ftruncate");
    std::exit(1);
  }
  return fd;
}

struct ThreadResult {
  double iops = 0.0;
  double p50_us = 0.0;
  double p99_us = 0.0;
};

void PinToCore(std::thread& t, int core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core, &cpuset);
  if (const int rc = ::pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
      rc != 0) {
    std::fprintf(stderr, "warning: pthread_setaffinity_np(core=%d) failed: %s\n", core,
                 std::strerror(rc));
  }
}

// Single-outstanding-op write/read loop for a fixed wall-clock duration,
// matching BM_SqpollIops's round-trip-latency methodology.
ThreadResult RunWorkload(io_uring& ring, int fd) {
  std::vector<char> buf(kBlockSize, 'x');
  std::minstd_rand rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist(0, kFileBlocks - 1);

  std::vector<double> latencies_us;
  latencies_us.reserve(1 << 16);

  const auto deadline = std::chrono::steady_clock::now() + kRunDuration;
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t ops = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    const std::uint64_t offset = dist(rng) * kBlockSize;
    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring);
    const auto op_start = std::chrono::steady_clock::now();
    if (ops % 2 == 0) {
      ::io_uring_prep_write(sqe, fd, buf.data(), kBlockSize, static_cast<off_t>(offset));
    } else {
      ::io_uring_prep_read(sqe, fd, buf.data(), kBlockSize, static_cast<off_t>(offset));
    }
    ::io_uring_submit(&ring);
    io_uring_cqe* cqe = nullptr;
    ::io_uring_wait_cqe(&ring, &cqe);
    const auto op_end = std::chrono::steady_clock::now();
    ::io_uring_cqe_seen(&ring, cqe);
    latencies_us.push_back(std::chrono::duration<double, std::micro>(op_end - op_start).count());
    ++ops;
  }

  const auto wall =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::sort(latencies_us.begin(), latencies_us.end());
  ThreadResult result;
  result.iops = static_cast<double>(ops) / wall;
  if (!latencies_us.empty()) {
    result.p50_us = latencies_us[latencies_us.size() * 50 / 100];
    result.p99_us = latencies_us[latencies_us.size() * 99 / 100];
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <baseline|attach|idle|pinned> <threads> [idle_ms]\n", argv[0]);
    return 2;
  }
  const std::string mode = argv[1];
  const int num_threads = std::atoi(argv[2]);
  const unsigned idle_ms = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1000;

  std::vector<int> fds(static_cast<std::size_t>(num_threads));
  for (auto& fd : fds) fd = MakeScratchFile();

  std::vector<io_uring> rings(static_cast<std::size_t>(num_threads));

  auto init_ring = [&](int i) {
    io_uring_params params{};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = idle_ms;

    if (mode == "attach" && i > 0) {
      params.flags |= IORING_SETUP_ATTACH_WQ;
      params.wq_fd = static_cast<unsigned>(rings[0].ring_fd);
    }
    if (mode == "pinned") {
      params.flags |= IORING_SETUP_SQ_AFF;
      params.sq_thread_cpu = static_cast<unsigned>(2 * i + 1);
    }

    if (const int rc = ::io_uring_queue_init_params(256, &rings[static_cast<std::size_t>(i)],
                                                      &params);
        rc < 0) {
      std::fprintf(stderr, "io_uring_queue_init_params(thread %d) failed: %s\n", i,
                   std::strerror(-rc));
      std::exit(1);
    }
  };

  // Ring 0 goes first unconditionally: "attach" mode needs its ring_fd
  // before any follower can compute wq_fd from it.
  init_ring(0);
  for (int i = 1; i < num_threads; ++i) init_ring(i);

  std::vector<ThreadResult> results(static_cast<std::size_t>(num_threads));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(num_threads));

  for (int i = 0; i < num_threads; ++i) {
    workers.emplace_back([&, i] {
      results[static_cast<std::size_t>(i)] =
          RunWorkload(rings[static_cast<std::size_t>(i)], fds[static_cast<std::size_t>(i)]);
    });
    if (mode == "pinned") PinToCore(workers.back(), 2 * i);
  }
  for (auto& t : workers) t.join();

  double total_iops = 0.0;
  std::vector<double> p50s, p99s;
  for (const auto& r : results) {
    total_iops += r.iops;
    p50s.push_back(r.p50_us);
    p99s.push_back(r.p99_us);
  }
  const double avg_p50 = std::accumulate(p50s.begin(), p50s.end(), 0.0) / p50s.size();
  const double avg_p99 = std::accumulate(p99s.begin(), p99s.end(), 0.0) / p99s.size();

  std::printf("%s,%d,%u,%.1f,%.3f,%.3f\n", mode.c_str(), num_threads, idle_ms, total_iops,
              avg_p50, avg_p99);

  for (int i = 0; i < num_threads; ++i) ::io_uring_queue_exit(&rings[static_cast<std::size_t>(i)]);
  for (int fd : fds) ::close(fd);
  return 0;
}
