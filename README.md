<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
  <img src="assets/logo-light.svg" alt="ringio" height="64">
</picture>

Header-only C++20, zero-allocation asynchronous storage engine for Linux. `ringio` bridges a
lock-free MPMC ring buffer directly with `io_uring` submission/completion queues run in
`IORING_SETUP_SQPOLL` mode, aiming for kernel-bypass NVMe I/O: no per-operation syscalls, no
runtime allocation, no memcpy between user buffers and the kernel.

## What's here

- **Lock-free ingestion ring**: worker threads enqueue read/write requests into a 128-byte
  dual-cache-line-aligned lock-free MPMC queue.
- **SQPOLL submission**: `io_uring` runs with a dedicated kernel polling thread, so issuing I/O
  is a plain memory write as long as that thread hasn't gone idle.
- **Pre-registered fixed buffers**: page-aligned (`posix_memalign`), `mlock`ed, and registered
  once via `io_uring_register_buffers()`.
- **Fixed file descriptor registration**: registered once via `io_uring_register_files()`,
  bypassing per-op kernel file-table lookups.
- **Shared-poller mode**: an engine can attach to another's kernel poller
  (`IORING_SETUP_ATTACH_WQ`) instead of spawning its own, since a poller busy-spins continuously
  and costs a full core by itself.

See [`ROADMAP.md`](ROADMAP.md) for what's built and what's left, and
[`docs/ringio-theory.pdf`](docs/ringio-theory.pdf) / [`docs/ringio-roadmap.pdf`](docs/ringio-roadmap.pdf)
for the original design motivation and phased plan.

## Does SQPOLL actually win?

We tested it. **[Read the paper](docs/paper/ringio-paper.pdf)** for the full methodology and
results; the short version:

SQPOLL removes the per-operation syscall, but on real NVMe it did not beat `libaio` or plain
`io_uring` on raw IOPS at any thread count or queue depth we swept, on either a 4-vCPU or a
16-vCPU test machine, ruling out core contention as the reason. What it does deliver is roughly
an order-of-magnitude fewer kernel entries per completed operation at high queue depth, at the
cost of higher tail latency, a cost that grew rather than shrank on the wider machine. The paper
reports that result plainly, including a page-cache measurement defect in an earlier round that
produced DRAM-speed numbers mislabeled as disk I/O, and is explicit about what the data does and
does not establish about the underlying cause.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure   # run the test suite
./build/benchmarks/ringio_benchmarks         # run benchmarks
```

`liburing` is detected via `find_path`/`find_library`; when found, `RINGIO_HAVE_LIBURING` is
defined and the `SqpollEngine` path becomes available (`sudo apt install liburing-dev`). `libaio`
is detected separately for one baseline benchmark (`sudo apt install libaio-dev`). Both are
optional at configure time; everything else still builds without them.

IOPS/tail-latency benchmarks need real NVMe, not a network-attached persistent disk. See
[`CLAUDE.md`](CLAUDE.md) for the full development workflow, including the GCE Local SSD setup
used for every result in this repository.

## License

Apache License 2.0. See [`LICENSE`](LICENSE).
