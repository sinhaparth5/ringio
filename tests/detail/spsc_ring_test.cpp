#include "ringio/detail/spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

namespace {

using ringio::detail::SpscRing;

TEST(SpscRing, PopOnEmptyRingFails) {
  SpscRing<int, 4> ring;
  int out = 0;
  EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, PushThenPopPreservesOrder) {
  SpscRing<int, 4> ring;
  ASSERT_TRUE(ring.try_push(1));
  ASSERT_TRUE(ring.try_push(2));
  ASSERT_TRUE(ring.try_push(3));

  int out = 0;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 1);
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 2);
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 3);
  EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, PushFailsOnceFull) {
  SpscRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.try_push(i));
  }
  EXPECT_FALSE(ring.try_push(4));
}

TEST(SpscRing, WrapsAroundAfterDraining) {
  SpscRing<int, 4> ring;
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 4; ++i) {
      ASSERT_TRUE(ring.try_push(round * 4 + i));
    }
    EXPECT_FALSE(ring.try_push(-1));
    for (int i = 0; i < 4; ++i) {
      int out = 0;
      ASSERT_TRUE(ring.try_pop(out));
      EXPECT_EQ(out, round * 4 + i);
    }
  }
}

// One producer, one consumer, ring far smaller than the item count, so the
// pair spends most of its time contending on head/tail. Every value must
// arrive exactly once and in order.
TEST(SpscRing, ConcurrentProducerConsumerDeliversEveryItemInOrder) {
  constexpr int kCapacity = 8;
  constexpr int kCount = 200000;
  SpscRing<int, kCapacity> ring;

  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) {
      while (!ring.try_push(i)) {
        std::this_thread::yield();
      }
    }
  });

  int next_expected = 0;
  int received = 0;
  while (received < kCount) {
    int out = 0;
    if (ring.try_pop(out)) {
      EXPECT_EQ(out, next_expected);
      ++next_expected;
      ++received;
    }
  }
  producer.join();
  EXPECT_EQ(received, kCount);
}

}  // namespace
