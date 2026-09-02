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

- [ ] Lock-free zero-allocation SPSC submission queue interfacing with `io_uring_sqe`
- [ ] Lock-free zero-allocation MPMC submission queue interfacing with `io_uring_sqe`
- [ ] SQPOLL kernel thread initialization (`IORING_SETUP_SQPOLL`)
- [ ] Fixed file descriptor registration (`io_uring_register_files()`)

## Phase 4 — Asynchronous Completion Engine

- [ ] Non-blocking `io_uring_cqe` completion queue harvesting
- [ ] Single-pass batch submission
- [ ] Out-of-order request correlation via 64-bit opaque token IDs
- [ ] Batched completion event dispatch (16–32 events per pass) to minimize atomic tail updates
- [ ] Zero-allocation completion event handling

## Phase 5 — Microarchitectural Profiling Suite

- [ ] Google Benchmark throughput harness: 4KB random read/write IOPS scaling across 1–32 threads
- [ ] Tail latency tracking: p50, p95, p99, p99.9
- [ ] Baseline comparison vs. POSIX `pread`/`pwrite`
- [ ] Baseline comparison vs. `libaio`
- [ ] Baseline comparison vs. plain `io_uring` (no SQPOLL)
- [ ] ThreadSanitizer verification (`-fsanitize=thread`) — zero race conditions
- [ ] AddressSanitizer verification (`-fsanitize=address`) — no memory leaks

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
