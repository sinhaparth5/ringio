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

The first version of this benchmark undersold the fix. Its worker threads called
`harvest_completions()` in a flat busy-spin loop, so every application thread burned a full core
on top of the one the shared poller now takes, which reintroduced a core-budget ceiling from the
application side and made throughput collapse past 2 threads even though the kernel mechanism
doesn't.

- [x] `WaitForCompletion` (in `benchmarks/iops_throughput_benchmark.cpp`) replaces the flat spin
      with a three-tier backoff — fast retries, then bounded pause-instruction spinning, then a
      scheduler yield — so a waiting thread stops denying the scheduler cycles it needs to run
      the poller. Both `BM_SqpollIops` and `BM_SqpollSharedPollerIops` use it.

With backoff, `BM_SqpollSharedPollerIops` beats the independent-ring `BM_SqpollIops` at every
thread count above 1, by a wide margin, on the same `n2-standard-4` box:

| threads | independent (`BM_SqpollIops`) | shared poller (`BM_SqpollSharedPollerIops`) |
|---|---|---|
| 1 | 194K | 207K |
| 2 | 205K | 417K |
| 4 | 1.8K | 163K |
| 8 | 1.6K | 112K |
| 16 | 1.1K | 63K |
| 32 | 1.2K | 31K |

Backoff doesn't create CPU capacity that isn't there, and the shared-poller numbers still peak at
2 threads and decline afterward: 4 vCPUs means 1 core for the poller and 3 for application
threads, so anything past that is already oversubscribed. What backoff fixes is the collapse
being disproportionate to that oversubscription — before it, 8 threads on the shared poller fell
to 46K IOPS; after it, the same run holds 112K. The independent-ring backend, with no shared
poller to protect, still falls off a cliff past 2 threads regardless of backoff. That's expected:
it still pays the full 2-cores-per-ring cost this investigation exists to fix.

### Follow-up: baseline comparison methodology

With the shared-poller fix and its backoff in place, ran `SqpollEngine` (both the independent-ring
and shared-poller variants) head-to-head against `BM_PosixPreadPwriteIops`, `BM_LibaioIops`, and
`BM_PlainIoUringIops` on the same `n2-standard-4` box, to check whether Phase 5's original finding
("`SqpollEngine` trailing every baseline") still held. It did, by a wide margin at every thread
count — but the comparison itself turned out to be invalid, for two reasons found in
`benchmarks/iops_throughput_benchmark.cpp` while checking why the baseline numbers looked too fast
to be real disk I/O:

- `MakeScratchFile()` never opens with `O_DIRECT`, and the working set is 64 MiB. On a box with far
  more RAM than that, every backend's I/O after the first pass is served from the page cache, not
  the NVMe device — POSIX `pread`/`pwrite` hit 698K-1.3M IOPS in this run, which is DRAM speed, not
  disk speed. The baselines never touched the drive this suite claims to measure.
- Every backend, `SqpollEngine` included, runs at queue depth 1: submit one request, block-wait for
  that exact completion, then submit the next (`BM_SqpollIops`'s own comment already says as much).
  SQPOLL's entire value proposition is amortizing cost across many in-flight requests submitted
  without a syscall each; at depth 1 there's nothing to amortize, and the design pays kernel-poller
  scheduling latency against a synchronous path that runs inline instead. Losing here is expected,
  not a regression — it's evidence the design isn't being exercised, not evidence against it.

So the paper's core hypothesis (kernel-bypass `io_uring`/SQPOLL beats syscall-based I/O) is neither
confirmed nor refuted by any run so far. Answering it needs a harness redesign, not another rerun:

- [x] `O_DIRECT | O_RDWR` on the scratch file in `MakeScratchFile()`, with every backend's I/O
      buffers aligned to the 4096-byte sector size (`posix_memalign`/`std::aligned_alloc`) —
      `O_DIRECT` rejects unaligned buffers with `EINVAL`. Working set sized well past this box's RAM
      (or otherwise confirmed to bypass the page cache) so the syscall baselines hit the actual
      device instead of DRAM.
- [x] A queue-depth parameter per backend, swept across QD 1/8/32/64/128: each worker keeps that
      many requests in flight, reaping completions in batches and immediately resubmitting to keep
      the pipeline full, instead of the current submit-then-block-wait-one loop. Applies to all four
      backends being compared, not just the SQPOLL ones — `BM_PlainIoUringIops` also currently waits
      one CQE at a time.
- [x] Rerun all five benchmarks across the QD sweep on the same real-NVMe VM once both land, and
      replace this section's finding with whatever that run actually shows.

### Follow-up: O_DIRECT and queue-depth results

Landed both fixes and reran on the same `n2-standard-4`-with-local-NVMe setup. `MakeScratchFile()`
now reopens with `O_DIRECT` (buffers moved to `posix_memalign`-aligned storage, since `O_DIRECT`
rejects unaligned ones with `EINVAL`), and `libaio`, plain `io_uring`, and both `SqpollEngine`
backends pipeline `queue_depth` requests in flight instead of one at a time, swept across QD
1/8/32/64/128 at 1 and 4 threads (bounded down from the full 1-32 thread sweep to keep the QD ×
thread cross product to a reasonable amount of VM time — see the benchmark file's header comment).
POSIX `pread`/`pwrite` kept its full thread sweep with no QD axis, since a blocking syscall has
nothing to pipeline.

The page-cache fix is confirmed by the numbers themselves: every backend now tops out around 200K
IOPS instead of the previous run's 698K-1.3M, which is a physically plausible ceiling for real NVMe
under virtualization rather than DRAM speed.

The core hypothesis is still not confirmed, though — at matched thread count and queue depth, plain
`io_uring` and `libaio` match or beat both `SqpollEngine` variants on raw IOPS. At 4 threads, plain
`io_uring` and `libaio` both plateau around 199-200K ops/s from QD 8 upward; `SqpollEngine`'s best
result at 4 threads is 135K (shared poller, QD 128). The independent-ring variant does worse still
(132K at QD 128), and it reproduces the exact core-budget collapse from the investigation above at
QD 1 (11.6K at 4 threads, down from 23.4K at 1 thread) — the same effect, now visible across the QD
axis as well, not just the thread-count axis.

Where the design's advantage does show up is round-trip cost: at QD 128, `SqpollIops` needs roughly
0.1-0.3 submit/reap calls per completed op (see the benchmark file's `calls_per_op` counter and its
comment on why this is a round-trip count, not a confirmed syscall count, for the io_uring-based
backends), against 1.3-4.3 for plain `io_uring` at the same setting. That's the batching mechanism
working as designed. But it comes at a tail-latency cost: at 4 threads/QD 128, `SqpollIops` p99 is
23.5ms, worse than plain `io_uring` (15.0ms) or `libaio` (14.3ms) at the identical setting — SQPOLL
is spending fewer syscalls but queuing completions longer before harvesting them.

So the finding this harness actually supports is narrower than "SQPOLL wins": fewer round trips per
op, not more throughput, and worse tail latency at depth on this hardware. Whether that's inherent
to the design or an artifact of this specific box's poller-vs-worker core split (still contended at
4 threads on a 4-vCPU machine) is open — a wider core count, or a poller-idle/backoff tuning pass,
would be needed to separate the two. Not scheduled yet.

### Follow-up: wider-core rerun (n2-standard-16)

Widened `kPipelinedThreadCounts` from `{1, 4}` to `{1, 2, 4, 8}` and reran on an `n2-standard-16`
with local NVMe, giving every configuration up to 8 worker threads plus the SQPOLL poller thread
room across 16 vCPUs — the 4-vCPU box's tightest case (4 workers + 1 poller) had exactly as many
threads as cores.

Core oversubscription is ruled out as the explanation. At every matched thread count and queue
depth, `SqpollEngine` (both variants) still doesn't beat plain `io_uring` or `libaio` on raw IOPS —
the gap from the 4-vCPU run persists on a box where the poller has a dedicated core to itself.
`libaio` is the standout instead, and its lead widens rather than narrows once threads are cheap:
at 1 thread/QD 32 it does 160.7K ops/s against 69-80K for the other three backends, a gap this
suite hadn't shown at 4 vCPUs, where thread count was the scarcer resource than depth.

At 4-8 threads, every backend converges to roughly 200-207K ops/s regardless of QD, matching the
same device ceiling the O_DIRECT fix surfaced — that plateau, not per-backend design, is what's
capping throughput once there's enough concurrency to reach it. `SqpollSharedPollerIops`'s tail
latency is worse here than on the 4-vCPU box, not better: p99 at 4 threads/QD 128 is 47.6ms against
23.5ms before, while `libaio`'s p99 at the same setting (15.1ms) is close to unchanged. More worker
threads sharing one poller queues completions longer before they're harvested; a wider core count
made that worse, not better.

The open question from the previous section is answered: the shortfall isn't an artifact of core
contention on a small box. It's either inherent to SQPOLL's completion-harvesting path as
implemented here, or something this suite hasn't isolated yet (poller idle/backoff tuning is still
untried). Full results: `benchmarks/iops_throughput_benchmark.cpp` on
`widen-pipelined-thread-sweep`, raw JSON not checked in.

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
