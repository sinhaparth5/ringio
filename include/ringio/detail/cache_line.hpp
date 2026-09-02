#pragma once

#include <concepts>
#include <cstddef>
#include <utility>

namespace ringio::detail {

// Most x86_64 parts fetch adjacent 64-byte lines together (the L2 spatial
// prefetcher), so two hot atomics 64 bytes apart still false-share under
// contention. Padding to 128 bytes (two lines) keeps each one isolated.
inline constexpr std::size_t kCacheLineSize = 128;

// Wraps `T`, padded and aligned to `kCacheLineSize` so no two instances ever
// land in the same cache-line pair. Meant for hot atomics — ring buffer
// head/tail indices — that different threads write concurrently.
//
// The struct's size is not set explicitly: `alignas` already guarantees the
// compiler rounds sizeof(CacheLinePadded<T>) up to a multiple of
// kCacheLineSize, which is exactly the padding wanted here.
template <typename T>
struct alignas(kCacheLineSize) CacheLinePadded {
  T value;

  CacheLinePadded() = default;

  // Forwards straight into T's constructor instead of taking `const T&`, so
  // types like std::atomic — constructible from a plain value but not
  // copyable — can still be initialized in place: `CacheLinePadded<std::
  // atomic<uint64_t>> counter(0)`.
  template <typename U>
    requires std::constructible_from<T, U>
  constexpr CacheLinePadded(U&& v) : value(std::forward<U>(v)) {}

  T& get() noexcept { return value; }
  const T& get() const noexcept { return value; }

  operator T&() noexcept { return value; }
  operator const T&() const noexcept { return value; }
};

static_assert(sizeof(CacheLinePadded<int>) == kCacheLineSize);
static_assert(alignof(CacheLinePadded<int>) == kCacheLineSize);

}  // namespace ringio::detail
