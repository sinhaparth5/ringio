# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Phases 1–3 (repo/build scaffolding, zero-copy page buffer pool, kernel-bypass submission engine)
are done — see `ROADMAP.md` for what's checked off. Phases 4–6 are not started. Treat
`docs/ringio-theory.pdf` (motivation/design) and `docs/ringio-roadmap.pdf` (phased implementation
plan) as the authoritative spec for anything not yet built; this file only tracks what already
exists.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # run the test suite
./build/benchmarks/ringio_benchmarks         # run benchmarks
```

GoogleTest and Google Benchmark are pulled in automatically via `FetchContent` (network access
required on first configure). `liburing` is detected via `find_path`/`find_library`; when found,
CMake defines `RINGIO_HAVE_LIBURING` and headers that call into it (e.g.
`BufferPool::register_with`) become available. If it's missing, configure prints a warning and
everything else still builds fine, but those liburing-calling members are compiled out. Install
it with `sudo apt install liburing-dev`.

To run a single test: `ctest --test-dir build -R <TestSuite.TestName>`, or run the test binary
directly with GoogleTest's own filter: `./build/tests/ringio_tests --gtest_filter=<pattern>`.

`RINGIO_BUILD_TESTS`, `RINGIO_BUILD_BENCHMARKS`, and `RINGIO_MARCH_NATIVE` are CMake options
(all default `ON`) if you need to disable one, e.g. when `-march=native` isn't safe for the
target machine.

## What ringio is

ringio is a **header-only C++20, zero-allocation** asynchronous storage engine for Linux. It
bridges a user-space lock-free MPMC ring buffer directly with `io_uring` kernel submission/
completion queues run in `IORING_SETUP_SQPOLL` mode, aiming for kernel-bypass NVMe I/O: no
per-operation syscalls, no runtime allocation, no memcpy between user buffers and the kernel.

The design's core bet: on modern NVMe hardware the bottleneck is no longer flash latency, it's
OS overhead (syscall mode-switches, thread context switches, dynamic allocation/copying, TLB
misses). ringio's architecture attacks each of these directly:

- **Lock-free ingestion ring**: worker threads enqueue read/write requests into a 128-byte
  dual-cache-line-aligned lock-free MPMC queue (`alignas(128)` padding to keep atomic head/tail
  pointers on separate cache lines and avoid false sharing).
- **SQPOLL submission**: `io_uring` is configured with `IORING_SETUP_SQPOLL`; a dedicated kernel
  thread polls the submission queue, so issuing I/O is a plain memory write with zero syscalls.
- **Pre-registered fixed buffers**: buffer pools are page-aligned (`posix_memalign`, 4096-byte
  boundaries matching NVMe sectors), locked with `mlock()`, and registered once via
  `io_uring_register_buffers()` — eliminating per-request page pinning/copying.
- **Fixed file descriptor registration**: file descriptors are registered once at init via
  `io_uring_register_files()`, bypassing per-op kernel file-table lookups.
- **Completion correlation**: completions are harvested from the CQ non-blockingly and matched
  back to requests via 64-bit opaque token IDs, processed in batches (16–32 events) to minimize
  atomic tail updates.

## Repository layout

- `include/ringio/` — the header-only library. `ringio.hpp` is the umbrella header; each phase
  adds its own header under here:
  - `detail/cache_line.hpp` — the 128-byte cache-line padding wrapper `CacheLinePadded<T>` used
    to isolate hot atomics from false sharing.
  - `detail/buffer_pool.hpp` — `BufferPool`, a fixed set of page-aligned, `mlock`ed buffers
    handed out via a lock-free (Treiber-stack) `acquire`/`release`, with
    `register_with(io_uring*)` to pin them as fixed DMA buffers once liburing is present.
  - `detail/spsc_ring.hpp` / `detail/mpmc_ring.hpp` — bounded, zero-allocation lock-free ring
    buffers (`SpscRing<T, Capacity>`, and `MpmcRing<T, Capacity>` using Vyukov's bounded queue)
    that worker threads push `IoRequest`s into.
  - `io_request.hpp` — `IoRequest`, the trivially-copyable request struct the rings carry
    (opcode, fixed-file index, buffer index, offset, length, correlation token).
  - `sqpoll_engine.hpp` — `SqpollEngine` (liburing-only): owns an `IORING_SETUP_SQPOLL` ring,
    registers fixed files/buffers, and `drain_and_submit()`s a batch of `IoRequest`s from either
    ring type into real `io_uring_sqe`s.
- `benchmarks/` — Google Benchmark harness. Eventually: IOPS scaling across 1–32 threads,
  p50/p95/p99/p99.9 tail latency, comparisons against POSIX `pread`/`pwrite`, `libaio`, and plain
  `io_uring` without SQPOLL. Currently exercises `CacheLinePadded`, `BufferPool`, `SpscRing`, and
  `MpmcRing` (uncontended and contended across thread counts).
- `tests/` — Google Test suite, mirroring the `include/ringio/` layout (e.g.
  `tests/detail/cache_line_test.cpp` tests `include/ringio/detail/cache_line.hpp`).
- Root `CMakeLists.txt` defines an `INTERFACE` target `ringio::ringio`; per-directory
  `CMakeLists.txt` files under `tests/` and `benchmarks/` pull in their respective dependencies.
- Correctness passes intended via ThreadSanitizer (`-fsanitize=thread`) for race detection and
  AddressSanitizer (`-fsanitize=address`) for leaks — not wired into the build yet (Phase 5).

## Engineering sequencing

The roadmap phases build strictly bottom-up — each depends on the one before it:

1. Repo/CMake scaffolding + cache-line alignment primitives.
2. Zero-copy page-aligned DMA buffer pool (`posix_memalign`, `mlock`, `io_uring_register_buffers`).
3. Lock-free submission ring + SQPOLL kernel thread + `io_uring_register_files`.
4. Asynchronous completion engine (CQE harvesting, token-based request correlation, batched
   dispatch).
5. Benchmarking/profiling suite and sanitizer verification.
6. Paper/artifact writeup for a systems venue (USENIX ATC / EuroSys / IEEE TPDS).

When implementing, follow this order — later phases (e.g. the completion engine) assume the
buffer-pool and submission-ring invariants from earlier phases are already in place.

## Development workflow

- **Testing the application**: `gcloud` is already authenticated (project
  `project-6e56124d-9e93-4934-ba8`) — build and run the test suite on a GCE VM, not on the local
  sandbox. The VM gives a real, isolated Linux kernel (needed for `io_uring`/SQPOLL behavior that
  a sandboxed/virtualized kernel may not support or may behave differently under) and real
  multi-core hardware for the contended benchmarks. A local build is still fine as a quick
  compile-only sanity check before provisioning a VM, to catch syntax/type errors for free — just
  don't run `ctest` or the benchmarks locally and call that the test result.
  - Provision a short-lived instance (e.g. `e2-standard-4`, spot/preemptible to keep cost down),
    SSH in, install build deps, clone the branch being tested, build, run `ctest` and the
    benchmarks, and capture the output.
  - **Delete the VM immediately after**, success or failure — it bills by the hour and there's no
    reason to leave it running. Don't leave instances around between sessions; if `gcloud compute
    instances list` ever shows one left over, delete it before doing anything else.
- **Branches and PRs**: `gh` is already authenticated — use it directly to create branches and
  open pull requests rather than asking the user to do so.
- **Commit messages**: do not append a `Co-Authored-By: Claude` trailer.
- **PR descriptions and code comments**: do not append a "Generated with Claude Code" line or
  similar attribution.
- **Writing commit messages and comments**: run them through the
  `avoid-ai-writing:avoid-ai-writing` skill before finalizing, to strip AI-writing tells and keep
  the wording plain.
