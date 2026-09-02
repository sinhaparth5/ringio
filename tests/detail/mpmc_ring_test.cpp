#include "ringio/detail/mpmc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using ringio::detail::MpmcRing;

TEST(MpmcRing, PopOnEmptyRingFails) {
  MpmcRing<int, 4> ring;
  int out = 0;
  EXPECT_FALSE(ring.try_pop(out));
}

TEST(MpmcRing, PushThenPopPreservesOrderSingleThreaded) {
  MpmcRing<int, 4> ring;
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

TEST(MpmcRing, PushFailsOnceFull) {
  MpmcRing<int, 4> ring;
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.try_push(i));
  }
  EXPECT_FALSE(ring.try_push(4));
}

TEST(MpmcRing, WrapsAroundAfterDraining) {
  MpmcRing<int, 4> ring;
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

// Several producers and several consumers hammer a ring much smaller than
// the total item count. Every producer pushes a disjoint slice of
// [0, kProducers * kItemsPerProducer) so a value showing up twice, or not
// at all, is unambiguous.
TEST(MpmcRing, ConcurrentProducersAndConsumersDeliverEveryItemExactlyOnce) {
  constexpr int kCapacity = 16;
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kItemsPerProducer = 50000;
  constexpr int kTotal = kProducers * kItemsPerProducer;

  MpmcRing<int, kCapacity> ring;
  std::vector<std::atomic<int>> seen_count(kTotal);
  for (auto& c : seen_count) c.store(0, std::memory_order_relaxed);

  std::atomic<int> produced_done{0};
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      const int base = p * kItemsPerProducer;
      for (int i = 0; i < kItemsPerProducer; ++i) {
        while (!ring.try_push(base + i)) {
          std::this_thread::yield();
        }
      }
      produced_done.fetch_add(1, std::memory_order_release);
    });
  }

  std::atomic<int> consumed{0};
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      for (;;) {
        int out = 0;
        if (ring.try_pop(out)) {
          seen_count[out].fetch_add(1, std::memory_order_relaxed);
          consumed.fetch_add(1, std::memory_order_release);
          continue;
        }
        // Empty: stop once every producer is done and nothing is left.
        if (produced_done.load(std::memory_order_acquire) == kProducers &&
            consumed.load(std::memory_order_acquire) >= kTotal) {
          return;
        }
        std::this_thread::yield();
      }
    });
  }

  for (auto& t : producers) t.join();
  for (auto& t : consumers) t.join();

  EXPECT_EQ(consumed.load(), kTotal);
  for (int i = 0; i < kTotal; ++i) {
    EXPECT_EQ(seen_count[i].load(), 1) << "item " << i << " delivered "
                                        << seen_count[i].load() << " times";
  }
}

}  // namespace
