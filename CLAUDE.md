# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This repository currently contains **no source code** — only `README.md`, `LICENSE`, and `docs/`
(the project's design documents). There is no build system, no `include/` tree, and no tests yet.
Do not assume any of the structure below exists on disk; treat it as the target architecture to
build toward, described in `docs/ringio-theory.pdf` (motivation/design) and
`docs/ringio-roadmap.pdf` (phased implementation plan). Read those two PDFs before starting
substantial work — they are the authoritative spec for this project, not this file.

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

## Planned repository layout (per roadmap, not yet created)

- `include/ringio/` — the header-only library itself.
- `benchmarks/` — Google Benchmark harness (IOPS scaling across 1–32 threads, p50/p95/p99/p99.9
  tail latency, comparisons against POSIX `pread`/`pwrite`, `libaio`, and plain `io_uring`
  without SQPOLL).
- `tests/` — Google Test suite.
- CMake build targeting `-std=c++20`, `-O3 -march=native`, linking `liburing` (`-luring`).
- Correctness passes intended via ThreadSanitizer (`-fsanitize=thread`) for race detection and
  AddressSanitizer (`-fsanitize=address`) for leaks.

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

- **Testing the application**: `gcloud` is already authenticated — use it to provision/access
  whatever cloud resources (e.g. an NVMe-backed VM) are needed to actually run and test the
  engine, rather than asking the user to do it manually.
- **Branches and PRs**: `gh` is already authenticated — use it directly to create branches and
  open pull requests rather than asking the user to do so.
- **Commit messages**: do not append a `Co-Authored-By: Claude` trailer.
- **PR descriptions and code comments**: do not append a "Generated with Claude Code" line or
  similar attribution.
- **Writing commit messages and comments**: run them through the
  `avoid-ai-writing:avoid-ai-writing` skill before finalizing, to strip AI-writing tells and keep
  the wording plain.
