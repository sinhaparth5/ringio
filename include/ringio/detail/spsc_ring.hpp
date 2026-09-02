#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

#include "ringio/detail/cache_line.hpp"

namespace ringio::detail {

// Bounded, zero-allocation, wait-free single-producer/single-consumer ring
// buffer. `Capacity` must be a power of two so index wraparound is a mask
// instead of a modulo. `T` must be trivially copyable: slots are assigned
// over, not constructed/destroyed, so the ring never allocates or runs a
// destructor after startup.
//
// Only one thread may call try_push, and only one (possibly different)
// thread may call try_pop; calling either from more than one thread is
// undefined behavior. Use MpmcRing when producers or consumers aren't
// single-threaded.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "SpscRing requires a trivially copyable T");

 public:
  SpscRing() = default;

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  // Producer-only. Returns false without blocking if the ring is full.
  bool try_push(const T& value) noexcept {
    const std::size_t tail = tail_.get().load(std::memory_order_relaxed);
    const std::size_t head = head_.get().load(std::memory_order_acquire);
    if (tail - head >= Capacity) {
      return false;
    }
    buffer_[tail & kMask] = value;
    tail_.get().store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer-only. Returns false without blocking if the ring is empty.
  bool try_pop(T& out) noexcept {
    const std::size_t head = head_.get().load(std::memory_order_relaxed);
    const std::size_t tail = tail_.get().load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    out = buffer_[head & kMask];
    head_.get().store(head + 1, std::memory_order_release);
    return true;
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  CacheLinePadded<std::atomic<std::size_t>> head_{0};
  CacheLinePadded<std::atomic<std::size_t>> tail_{0};
  std::array<T, Capacity> buffer_{};
};

}  // namespace ringio::detail
