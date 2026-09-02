# ringio Roadmap

Engineering roadmap for ringio, a header-only C++20 zero-allocation kernel-bypass storage engine.
Derived from `docs/ringio-roadmap.pdf`; see `docs/ringio-theory.pdf` for the design rationale
behind each item. Check items off as they land.

## Phase 1 — Environment & Memory Alignment

- [x] Repository directory structure: `include/ringio/`, `benchmarks/`, `tests/`
- [x] CMake build targeting C++20 (`-std=c++20`)
- [x] Optimization flags enabled (`-O3`, `-march=native`)
- [x] Link `liburing` (`-luring`) — CMake detects and links it when present; the dev package
      itself (`sudo apt install liburing-dev`) isn't installed in this sandbox, so this is wired
      up but not yet exercised end to end
- [x] 128-byte dual-cache-line-aligned padding wrapper (`alignas(128)`) to isolate atomic
      head/tail pointers from false sharing — `ringio::detail::CacheLinePadded<T>`
- [x] Google Test scaffolding
- [x] Google Benchmark scaffolding

## Phase 2 — Zero-Copy Page Buffer Pool

- [x] Page-aligned DMA buffer pool allocation (`posix_memalign`, 4096-byte boundaries matching
      NVMe sectors) — `ringio::BufferPool`
- [x] Memory page locking (`mlock()`) to prevent page-swapping stalls
- [x] Pre-registered buffer mapping (`io_uring_register_buffers()`) — `BufferPool::register_with`
- [x] Lock-free zero-allocation acquire/release (Treiber stack with tagged ABA-safe head), so
      checking a buffer in or out never allocates once the pool is built

## Phase 3 — Kernel-Bypass Submission Engine

- [x] Lock-free zero-allocation SPSC submission queue interfacing with `io_uring_sqe` —
      `ringio::detail::SpscRing<T, Capacity>`
- [x] Lock-free zero-allocation MPMC submission queue interfacing with `io_uring_sqe` —
      `ringio::detail::MpmcRing<T, Capacity>` (Vyukov bounded queue)
- [x] SQPOLL kernel thread initialization (`IORING_SETUP_SQPOLL`) — `ringio::SqpollEngine`
- [x] Fixed file descriptor registration (`io_uring_register_files()`) —
      `SqpollEngine::register_files`

## Phase 4 — Asynchronous Completion Engine

- [x] Non-blocking `io_uring_cqe` completion queue harvesting —
      `SqpollEngine::harvest_completions`
- [x] Single-pass batch submission — `SqpollEngine::drain_and_submit` (Phase 3), one
      `io_uring_submit` call per batch
- [x] Out-of-order request correlation via 64-bit opaque token IDs — `IoRequest::token` in,
      `IoCompletion::token` echoed back out; the kernel may complete requests in any order
- [x] Batched completion event dispatch (16–32 events per pass) to minimize atomic tail updates —
      `harvest_completions` walks the CQ ring with `io_uring_for_each_cqe` and calls
      `io_uring_cq_advance` once per batch instead of once per CQE
- [x] Zero-allocation completion event handling — harvested completions are trivially-copyable
      `IoCompletion`s pushed into a caller-owned `SpscRing`/`MpmcRing`

## Phase 5 — Microarchitectural Profiling Suite

- [x] Google Benchmark throughput harness: 4KB random read/write IOPS scaling across 1–32 threads —
      `BM_SqpollIops` in `benchmarks/iops_throughput_benchmark.cpp`
- [x] Tail latency tracking: p50, p95, p99, p99.9 — `ReportPercentiles` in the same file, applied to
      every backend benchmarked
- [x] Baseline comparison vs. POSIX `pread`/`pwrite` — `BM_PosixPreadPwriteIops`
- [x] Baseline comparison vs. `libaio` — `BM_LibaioIops` (built when `libaio-dev` is present;
      detected in `benchmarks/CMakeLists.txt`)
- [x] Baseline comparison vs. plain `io_uring` (no SQPOLL) — `BM_PlainIoUringIops`
- [x] ThreadSanitizer verification (`-fsanitize=thread`) — zero race conditions — `RINGIO_ENABLE_TSAN`
      CMake option; caught and fixed a real data race in `BufferPool`'s free-list on the first run
- [x] AddressSanitizer verification (`-fsanitize=address`) — no memory leaks — `RINGIO_ENABLE_ASAN`
      CMake option

### Follow-up: SQPOLL core-budget investigation

Re-running Phase 5's benchmarks on real NVMe (a GCE `n2-standard-4` with a local NVMe SSD, not the
original run's persistent-disk VM) showed `SqpollEngine` still trailing every baseline and still
collapsing past 2 threads on 4 vCPUs. The cause: each `SqpollEngine`'s SQPOLL kernel thread
busy-polls continuously, so it costs a full core by itself — a ring is really a two-core
commitment, not one, and nothing in the design accounted for that.

`experiments/sqpoll_core_budget.cpp` tested three fixes against raw liburing:
`IORING_SETUP_ATTACH_WQ` (sharing one poller across rings), `sq_thread_idle` tuning, and
core-pinning the app thread and poller separately. Sharing the poller was the one that worked —
independent rings collapsed from 157K IOPS (2 threads) to 71K (8 threads); threads sharing one
poller climbed to 248K at 8 and hadn't plateaued.

- [x] `SqpollEngine` gets a third constructor parameter, `attach_to`, defaulted to `nullptr` so
      every existing call site is unaffected — when set, the new engine shares `attach_to`'s
      kernel poller (`IORING_SETUP_ATTACH_WQ`) instead of spawning its own
- [x] `BM_SqpollSharedPollerIops` benchmarks it: one master engine plus N-1 attached followers,
      built before any thread starts (doesn't use `->Threads(N)`, since that ordering can't be
      guaranteed across per-thread setup — see the function's comment)

One caveat worth being direct about: `BM_SqpollSharedPollerIops`'s own numbers don't show the
raw harness's full win. Its worker threads call `harvest_completions()` in a busy-spin loop (the
same round-trip-latency methodology `BM_SqpollIops` uses), which is correct for what it measures
but means every application thread also burns a full core — on top of the one core the shared
poller now takes. That reintroduces a core-budget ceiling from the application side once thread
count exceeds roughly (vCPUs − 1), which is why this benchmark still turns over past 2 threads on
a 4-vCPU box even though the underlying kernel mechanism doesn't. The raw harness avoids this
because it blocks on `io_uring_wait_cqe` instead of spinning. A benchmark that queues deeper or
waits instead of spinning would likely show attach-mode's real ceiling; that's future work, not
done here.

## Phase 6 — Paper Writing & Publication

- [ ] Manuscript: methodology, kernel-bypass design, empirical evaluation
- [ ] Benchmark plots: IOPS throughput curves, tail latency percentiles, syscall count reduction
- [ ] Open-source reproducible artifact packaging
- [ ] Manuscript drafted for a target venue (USENIX ATC / EuroSys / IEEE TPDS)

## Target metrics (from `docs/ringio-theory.pdf`)

| Metric | Baseline (POSIX pread / libaio) | ringio target |
|---|---|---|
| 4KB random read throughput | ~150K–300K IOPS (syscall-bound) | > 600K IOPS |
| Tail latency (p99 / p99.9) | 50–100 µs+ | sub-microsecond to low-microsecond |
| Syscalls per 1M ops | 1,000,000 | near-zero (steady-state SQPOLL) |
| CPU cycle cost per op | high (context switches, TLB misses) | minimal |
