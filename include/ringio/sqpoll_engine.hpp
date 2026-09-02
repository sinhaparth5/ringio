#pragma once

// This header only defines SqpollEngine when liburing was found at
// configure time (see CMakeLists.txt) — everything in it talks directly to
// io_uring, unlike BufferPool, SpscRing, and MpmcRing which build with or
// without liburing present.
#if defined(RINGIO_HAVE_LIBURING)

#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>

#include <liburing.h>

// io_uring_register_ring_fd() was added in liburing 2.2; older liburing
// (Ubuntu 22.04 ships 2.1) doesn't declare it at all, so this can't be a
// runtime fallback -- it has to be compiled out. liburing before 2.2 also
// doesn't ship io_uring/io_uring_version.h, so its absence is itself the
// signal to skip the call.
#if __has_include(<liburing/io_uring_version.h>)
#include <liburing/io_uring_version.h>
#if defined(IO_URING_VERSION_MAJOR) && \
    (IO_URING_VERSION_MAJOR > 2 || (IO_URING_VERSION_MAJOR == 2 && IO_URING_VERSION_MINOR >= 2))
#define RINGIO_HAVE_REGISTER_RING_FD 1
#endif
#endif

#include "ringio/detail/buffer_pool.hpp"
#include "ringio/io_completion.hpp"
#include "ringio/io_request.hpp"

namespace ringio {

// Owns one SQPOLL-mode io_uring instance: a dedicated kernel thread polls
// the submission queue, so draining requests into it costs a plain memory
// write, not a syscall, as long as that thread hasn't gone idle (see
// sq_thread_idle_ms below).
//
// SqpollEngine itself holds no request queue — drain_and_submit() takes
// whichever SpscRing<IoRequest, N> or MpmcRing<IoRequest, N> the caller is
// feeding, so a single engine can be fed by either, depending on whether
// the workload has one producer thread or several.
class SqpollEngine {
 public:
  // `entries` sizes the underlying SQ/CQ rings (must be a power of two).
  // `sq_thread_idle_ms` is how long the kernel polling thread spins with no
  // work before sleeping; drain_and_submit() after that point costs one
  // wake-up syscall instead of zero, same as vanilla SQPOLL.
  //
  // Throws std::system_error if the kernel refuses to create the ring —
  // e.g. RLIMIT_MEMLOCK too low for the requested ring size, or SQPOLL
  // unsupported/disallowed in this environment.
  explicit SqpollEngine(unsigned entries, unsigned sq_thread_idle_ms = 1000) {
    ::io_uring_params params{};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = sq_thread_idle_ms;
    if (const int ret = ::io_uring_queue_init_params(entries, &ring_, &params); ret < 0) {
      throw std::system_error(-ret, std::generic_category(),
                               "SqpollEngine: io_uring_queue_init_params failed");
    }
    // Registers this ring's own fd (io_uring_register_ring_fd) so liburing's
    // internal io_uring_enter calls address it through an index instead of
    // the process fd table, skipping the fget/fput refcount churn that table
    // lookup costs. When the call is available (see the version check above)
    // it's still only best-effort: on a kernel too old to support it, it
    // returns a negative errno, nothing is set, and every call falls back to
    // the ordinary fd path.
#if defined(RINGIO_HAVE_REGISTER_RING_FD)
    ::io_uring_register_ring_fd(&ring_);
#endif
  }

  ~SqpollEngine() { ::io_uring_queue_exit(&ring_); }

  SqpollEngine(const SqpollEngine&) = delete;
  SqpollEngine& operator=(const SqpollEngine&) = delete;

  // Registers `fds` as this engine's fixed file table
  // (io_uring_register_files). IoRequest::fixed_fd_index addresses files by
  // index into this table, not by raw fd. Returns 0 on success, -errno
  // otherwise.
  int register_files(std::span<const int> fds) {
    return ::io_uring_register_files(&ring_, fds.data(), static_cast<unsigned>(fds.size()));
  }

  // Registers `pool`'s buffers as fixed DMA buffers
  // (io_uring_register_buffers), so *_fixed ops against them skip
  // per-request page pinning. Returns 0 on success, -errno otherwise.
  int register_buffers(const BufferPool& pool) { return pool.register_with(&ring_); }

  // Pops up to `max_batch` requests from `queue` (either ring type — any
  // type exposing `bool try_pop(IoRequest&)`), translates each into a
  // fixed-file, fixed-buffer SQE against `pool`, and submits the batch in
  // one io_uring_submit call. Stops early if the underlying SQ ring fills
  // up before `queue` or `max_batch` is exhausted. Returns the number of
  // requests submitted.
  template <typename Queue>
  std::size_t drain_and_submit(Queue& queue, const BufferPool& pool,
                                std::size_t max_batch = 32) {
    std::size_t submitted = 0;
    IoRequest req;
    while (submitted < max_batch && queue.try_pop(req)) {
      ::io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
      if (sqe == nullptr) {
        break;  // SQ ring is full; caller can retry once completions land.
      }
      // io_uring_prep_*_fixed takes void* regardless of direction (it
      // doesn't touch the buffer on a write), so this is safe even though
      // `pool` is const here.
      void* buf = const_cast<std::byte*>(pool.data(req.buffer_index));
      const int buf_index = static_cast<int>(req.buffer_index);
      if (req.op == IoOp::kRead) {
        ::io_uring_prep_read_fixed(sqe, req.fixed_fd_index, buf, req.length, req.offset,
                                    buf_index);
      } else {
        ::io_uring_prep_write_fixed(sqe, req.fixed_fd_index, buf, req.length, req.offset,
                                     buf_index);
      }
      sqe->flags |= IOSQE_FIXED_FILE;
      // io_uring_sqe_set_data64() isn't available in every liburing we
      // target (e.g. Ubuntu 22.04 ships 2.1); the pointer-based setter has
      // been there since 0.1 and round-trips a full 64-bit value on the
      // LP64 platforms this project runs on.
      ::io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<std::uintptr_t>(req.token)));
      ++submitted;
    }
    if (submitted > 0) {
      ::io_uring_submit(&ring_);
    }
    return submitted;
  }

  // Non-blockingly harvests completed CQEs and pushes each into
  // `out_queue` (either ring type — any type exposing
  // `bool try_push(const IoCompletion&)`) as the request's token and result
  // code. Never waits: if nothing has completed yet, returns 0 immediately.
  //
  // Walks the CQ ring in place with io_uring_for_each_cqe (a read-only
  // pointer walk, no kernel state touched yet) instead of calling
  // io_uring_cqe_seen per entry, then advances the CQ tail once via
  // io_uring_cq_advance for the whole batch. That turns what would be one
  // atomic store per completion into one per harvest_completions() call, in
  // line with the 16-32-per-pass batching this is meant for.
  //
  // A CQE is counted "seen" (and its ring slot freed for the kernel to
  // reuse) as soon as it's walked, whether or not out_queue had room for
  // it -- there's no way to unsee a CQE once past. Size out_queue for at
  // least max_batch so a full queue doesn't silently drop completions.
  template <typename CompletionQueue>
  std::size_t harvest_completions(CompletionQueue& out_queue, std::size_t max_batch = 32) {
    std::size_t walked = 0;
    std::size_t delivered = 0;
    unsigned head = 0;
    ::io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&ring_, head, cqe) {
      if (walked >= max_batch) {
        break;
      }
      IoCompletion completion{};
      completion.token = static_cast<std::uint64_t>(
          reinterpret_cast<std::uintptr_t>(::io_uring_cqe_get_data(cqe)));
      completion.result = cqe->res;
      if (out_queue.try_push(completion)) {
        ++delivered;
      }
      ++walked;
    }
    if (walked > 0) {
      ::io_uring_cq_advance(&ring_, static_cast<unsigned>(walked));
    }
    return delivered;
  }

  ::io_uring* native_handle() noexcept { return &ring_; }

 private:
  ::io_uring ring_{};
};

}  // namespace ringio

#endif  // RINGIO_HAVE_LIBURING
