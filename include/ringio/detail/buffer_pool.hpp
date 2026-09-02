#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(RINGIO_HAVE_LIBURING)
#include <liburing.h>
#endif

#include "ringio/detail/cache_line.hpp"

namespace ringio {

// A fixed-size pool of page-aligned, page-locked buffers sized for
// zero-copy NVMe DMA. Every buffer is allocated once at construction
// (`posix_memalign`, on 4096-byte sector boundaries by default) and locked
// into RAM (`mlock`) so the kernel never swaps one out from under an
// in-flight DMA transfer. Handing a buffer out and taking it back
// (`acquire`/`release`) never allocates: both are index push/pop on a
// lock-free free list, safe to call from multiple threads concurrently.
class BufferPool {
 public:
  static constexpr std::uint32_t kInvalidIndex =
      std::numeric_limits<std::uint32_t>::max();

  // Allocates `buffer_count` buffers of `buffer_size` bytes each.
  // `buffer_size` must be a nonzero multiple of the system page size
  // (4096 bytes on all supported targets, matching the NVMe sector size).
  // Throws std::invalid_argument on bad arguments, std::system_error if
  // allocation or page-locking fails.
  explicit BufferPool(std::size_t buffer_count, std::size_t buffer_size = 4096);
  ~BufferPool();

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  BufferPool(BufferPool&&) = delete;
  BufferPool& operator=(BufferPool&&) = delete;

  // Pops a free buffer index off the pool, or kInvalidIndex if every buffer
  // is currently checked out. Lock-free, thread-safe.
  std::uint32_t acquire() noexcept;

  // Returns a buffer previously handed out by acquire(). Lock-free,
  // thread-safe. Releasing an index that isn't currently checked out, or
  // releasing the same index twice, is undefined behavior.
  void release(std::uint32_t index) noexcept;

  std::byte* data(std::uint32_t index) noexcept;
  const std::byte* data(std::uint32_t index) const noexcept;

  std::size_t buffer_size() const noexcept { return buffer_size_; }
  std::size_t buffer_count() const noexcept { return buffer_count_; }

  // iovecs()[i] describes buffer i as a {base, len} pair, ready to hand to
  // io_uring_register_buffers (or plain readv/writev).
  const std::vector<::iovec>& iovecs() const noexcept { return iovecs_; }

#if defined(RINGIO_HAVE_LIBURING)
  // Registers every buffer in the pool with `ring` as fixed, pre-mapped
  // buffers, so I/O against them skips per-request page pinning. Returns 0
  // on success, -errno on failure (see io_uring_register_buffers(3)).
  int register_with(::io_uring* ring) const;
#endif

 private:
  std::byte* base_ = nullptr;
  std::size_t buffer_size_ = 0;
  std::size_t buffer_count_ = 0;
  bool locked_ = false;
  std::vector<::iovec> iovecs_;

  // Treiber stack of free buffer indices, packed as {tag:32, index:32} so a
  // pop/push/pop race on another thread between our load and CAS can't slip
  // an ABA past us — the tag changes on every push and pop.
  detail::CacheLinePadded<std::atomic<std::uint64_t>> free_head_;

  // free_next_[i] is only ever written by whichever thread currently owns
  // buffer i (the thread about to publish it via release(), before anyone
  // else can observe it), and only ever read after acquire() has loaded a
  // head value naming i -- so the tag already rules out any read seeing a
  // *wrong* value. But acquire() has to read free_next_[index] before it
  // knows whether that head value is still current: another thread can
  // acquire i and release it again (a fresh write to free_next_[i], safe
  // under the ABA tag) while the first read is in flight. That's a genuine
  // concurrent read/write on the same element with no ordering between them
  // -- undefined behavior on a plain std::uint32_t even though the CAS
  // below is guaranteed to reject the stale read. Atomics with relaxed
  // ordering make that access well-defined without changing the algorithm;
  // the head CAS is still what establishes the actual ordering.
  std::vector<std::atomic<std::uint32_t>> free_next_;

  static constexpr std::uint64_t Pack(std::uint32_t index, std::uint32_t tag) noexcept {
    return (static_cast<std::uint64_t>(tag) << 32) | index;
  }
  static constexpr std::uint32_t UnpackIndex(std::uint64_t packed) noexcept {
    return static_cast<std::uint32_t>(packed);
  }
  static constexpr std::uint32_t UnpackTag(std::uint64_t packed) noexcept {
    return static_cast<std::uint32_t>(packed >> 32);
  }
};

inline BufferPool::BufferPool(std::size_t buffer_count, std::size_t buffer_size)
    : buffer_size_(buffer_size),
      buffer_count_(buffer_count),
      free_head_(Pack(kInvalidIndex, 0)),
      free_next_(buffer_count) {
  if (buffer_count_ == 0) {
    throw std::invalid_argument("BufferPool: buffer_count must be > 0");
  }

  const auto page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  if (buffer_size_ == 0 || buffer_size_ % page_size != 0) {
    throw std::invalid_argument(
        "BufferPool: buffer_size must be a nonzero multiple of the page "
        "size (" +
        std::to_string(page_size) + ")");
  }

  const std::size_t total_size = buffer_size_ * buffer_count_;
  void* base = nullptr;
  if (int err = ::posix_memalign(&base, page_size, total_size); err != 0) {
    throw std::system_error(err, std::generic_category(),
                             "BufferPool: posix_memalign failed");
  }
  base_ = static_cast<std::byte*>(base);

  if (::mlock(base_, total_size) != 0) {
    const int err = errno;
    ::free(base_);
    base_ = nullptr;
    throw std::system_error(err, std::generic_category(),
                             "BufferPool: mlock failed");
  }
  locked_ = true;

  iovecs_.reserve(buffer_count_);
  for (std::size_t i = 0; i < buffer_count_; ++i) {
    std::byte* buf = base_ + i * buffer_size_;
    iovecs_.push_back(::iovec{buf, buffer_size_});
    // Thread the free list last-to-first so acquire() hands out index 0
    // first; each entry's "next" is the index below it.
    free_next_[i].store((i == 0) ? kInvalidIndex : static_cast<std::uint32_t>(i - 1),
                         std::memory_order_relaxed);
  }
  free_head_.get().store(Pack(static_cast<std::uint32_t>(buffer_count_ - 1), 0),
                          std::memory_order_relaxed);
}

inline BufferPool::~BufferPool() {
  if (base_ != nullptr) {
    if (locked_) {
      ::munlock(base_, buffer_size_ * buffer_count_);
    }
    ::free(base_);
  }
}

inline std::uint32_t BufferPool::acquire() noexcept {
  std::uint64_t head = free_head_.get().load(std::memory_order_acquire);
  for (;;) {
    const std::uint32_t index = UnpackIndex(head);
    if (index == kInvalidIndex) {
      return kInvalidIndex;
    }
    const std::uint32_t next = free_next_[index].load(std::memory_order_relaxed);
    const std::uint64_t new_head = Pack(next, UnpackTag(head) + 1);
    if (free_head_.get().compare_exchange_weak(head, new_head,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
      return index;
    }
  }
}

inline void BufferPool::release(std::uint32_t index) noexcept {
  std::uint64_t head = free_head_.get().load(std::memory_order_relaxed);
  std::uint64_t new_head;
  do {
    free_next_[index].store(UnpackIndex(head), std::memory_order_relaxed);
    new_head = Pack(index, UnpackTag(head) + 1);
  } while (!free_head_.get().compare_exchange_weak(head, new_head,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed));
}

inline std::byte* BufferPool::data(std::uint32_t index) noexcept {
  return base_ + static_cast<std::size_t>(index) * buffer_size_;
}

inline const std::byte* BufferPool::data(std::uint32_t index) const noexcept {
  return base_ + static_cast<std::size_t>(index) * buffer_size_;
}

#if defined(RINGIO_HAVE_LIBURING)
inline int BufferPool::register_with(::io_uring* ring) const {
  return ::io_uring_register_buffers(ring, iovecs_.data(),
                                      static_cast<unsigned>(iovecs_.size()));
}
#endif

}  // namespace ringio
