#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ringio/detail/cache_line.hpp"

namespace ringio::detail {

// Bounded, zero-allocation, lock-free multi-producer/multi-consumer ring
// buffer (Dmitry Vyukov's bounded MPMC queue). `Capacity` must be a power
// of two. `T` must be trivially copyable: slots are assigned over, not
// constructed/destroyed, so the ring never allocates after startup.
//
// Any number of threads may call try_push and try_pop concurrently. Each
// slot carries its own sequence number, so producers and consumers claim
// slots via a CAS on the shared enqueue/dequeue cursor rather than a
// single global lock; the cursors are the only cache-contended state.
template <typename T, std::size_t Capacity>
class MpmcRing {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "MpmcRing requires a trivially copyable T");

 public:
  MpmcRing() noexcept {
    for (std::size_t i = 0; i < Capacity; ++i) {
      slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MpmcRing(const MpmcRing&) = delete;
  MpmcRing& operator=(const MpmcRing&) = delete;

  // Returns false without blocking if the ring is full.
  bool try_push(const T& value) noexcept {
    std::size_t pos = enqueue_pos_.get().load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = slots_[pos & kMask];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
      if (diff == 0) {
        if (enqueue_pos_.get().compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          slot.value = value;
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // full
      } else {
        pos = enqueue_pos_.get().load(std::memory_order_relaxed);
      }
    }
  }

  // Returns false without blocking if the ring is empty.
  bool try_pop(T& out) noexcept {
    std::size_t pos = dequeue_pos_.get().load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = slots_[pos & kMask];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
      if (diff == 0) {
        if (dequeue_pos_.get().compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          out = slot.value;
          slot.sequence.store(pos + Capacity, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;  // empty
      } else {
        pos = dequeue_pos_.get().load(std::memory_order_relaxed);
      }
    }
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  struct Slot {
    std::atomic<std::size_t> sequence{0};
    T value{};
  };

  std::array<Slot, Capacity> slots_;
  CacheLinePadded<std::atomic<std::size_t>> enqueue_pos_{0};
  CacheLinePadded<std::atomic<std::size_t>> dequeue_pos_{0};
};

}  // namespace ringio::detail
