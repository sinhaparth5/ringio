#include "ringio/detail/cache_line.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

using ringio::detail::CacheLinePadded;
using ringio::detail::kCacheLineSize;

TEST(CacheLinePadded, SizeMatchesCacheLine) {
  EXPECT_EQ(sizeof(CacheLinePadded<int>), kCacheLineSize);
}

TEST(CacheLinePadded, AlignmentMatchesCacheLine) {
  EXPECT_EQ(alignof(CacheLinePadded<int>), kCacheLineSize);
}

TEST(CacheLinePadded, AdjacentInstancesDoNotShareCacheLinePair) {
  CacheLinePadded<std::atomic<std::uint64_t>> slots[2];
  const auto* first = reinterpret_cast<const std::byte*>(&slots[0]);
  const auto* second = reinterpret_cast<const std::byte*>(&slots[1]);
  EXPECT_GE(second - first, static_cast<std::ptrdiff_t>(kCacheLineSize));
}

TEST(CacheLinePadded, ReadsAndWritesThroughToValue) {
  CacheLinePadded<int> padded(41);
  EXPECT_EQ(padded.get(), 41);

  padded.get() = 42;
  EXPECT_EQ(static_cast<int&>(padded), 42);
}

}  // namespace
