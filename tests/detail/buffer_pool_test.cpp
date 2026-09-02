#include "ringio/detail/buffer_pool.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#if defined(RINGIO_HAVE_LIBURING)
#include <liburing.h>
#endif

namespace {

using ringio::BufferPool;

TEST(BufferPool, ReportsSizeAndCount) {
  BufferPool pool(4, 4096);
  EXPECT_EQ(pool.buffer_size(), 4096u);
  EXPECT_EQ(pool.buffer_count(), 4u);
}

TEST(BufferPool, RejectsZeroBufferCount) {
  EXPECT_THROW(BufferPool(0, 4096), std::invalid_argument);
}

TEST(BufferPool, RejectsBufferSizeNotAPageMultiple) {
  EXPECT_THROW(BufferPool(4, 100), std::invalid_argument);
}

TEST(BufferPool, RejectsZeroBufferSize) {
  EXPECT_THROW(BufferPool(4, 0), std::invalid_argument);
}

TEST(BufferPool, BuffersArePageAligned) {
  const auto page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  BufferPool pool(4, 4096);
  for (std::uint32_t i = 0; i < pool.buffer_count(); ++i) {
    auto addr = reinterpret_cast<std::uintptr_t>(pool.data(i));
    EXPECT_EQ(addr % page_size, 0u) << "buffer " << i << " is not page-aligned";
  }
}

TEST(BufferPool, IovecsMatchBufferLayout) {
  BufferPool pool(3, 4096);
  const auto& iovecs = pool.iovecs();
  ASSERT_EQ(iovecs.size(), 3u);
  for (std::uint32_t i = 0; i < 3; ++i) {
    EXPECT_EQ(iovecs[i].iov_base, pool.data(i));
    EXPECT_EQ(iovecs[i].iov_len, pool.buffer_size());
  }
}

TEST(BufferPool, AcquireHandsOutDistinctIndicesUntilExhausted) {
  BufferPool pool(4, 4096);
  std::unordered_set<std::uint32_t> seen;
  for (int i = 0; i < 4; ++i) {
    const std::uint32_t index = pool.acquire();
    ASSERT_NE(index, BufferPool::kInvalidIndex);
    EXPECT_TRUE(seen.insert(index).second) << "index " << index << " handed out twice";
  }
  EXPECT_EQ(pool.acquire(), BufferPool::kInvalidIndex);
}

TEST(BufferPool, ReleaseMakesIndexAcquirableAgain) {
  BufferPool pool(1, 4096);
  const std::uint32_t index = pool.acquire();
  ASSERT_NE(index, BufferPool::kInvalidIndex);
  EXPECT_EQ(pool.acquire(), BufferPool::kInvalidIndex);

  pool.release(index);
  EXPECT_EQ(pool.acquire(), index);
}

TEST(BufferPool, DataIsWritableAndPerBufferDistinct) {
  BufferPool pool(2, 4096);
  const std::uint32_t a = pool.acquire();
  const std::uint32_t b = pool.acquire();
  ASSERT_NE(a, BufferPool::kInvalidIndex);
  ASSERT_NE(b, BufferPool::kInvalidIndex);

  pool.data(a)[0] = std::byte{0xAB};
  pool.data(b)[0] = std::byte{0xCD};
  EXPECT_EQ(pool.data(a)[0], std::byte{0xAB});
  EXPECT_EQ(pool.data(b)[0], std::byte{0xCD});
}

// Stress test: many threads hammer acquire/release concurrently. If the
// free-list CAS loop ever leaks a double-issue, two threads will end up
// writing distinguishable markers into the same buffer at once and this
// will catch it via the owner-slot check below.
TEST(BufferPool, ConcurrentAcquireReleaseNeverDoubleIssues) {
  constexpr std::uint32_t kBuffers = 8;
  constexpr int kThreads = 8;
  constexpr int kItersPerThread = 20000;

  BufferPool pool(kBuffers, 4096);
  std::vector<std::atomic<int>> owner(kBuffers);
  for (auto& slot : owner) slot.store(-1, std::memory_order_relaxed);

  std::atomic<bool> saw_double_issue{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kItersPerThread; ++i) {
        std::uint32_t index;
        do {
          index = pool.acquire();
        } while (index == BufferPool::kInvalidIndex);

        int expected = -1;
        if (!owner[index].compare_exchange_strong(expected, t)) {
          saw_double_issue.store(true, std::memory_order_relaxed);
        } else {
          owner[index].store(-1, std::memory_order_relaxed);
        }
        pool.release(index);
      }
    });
  }
  for (auto& thread : threads) thread.join();

  EXPECT_FALSE(saw_double_issue.load());
}

#if defined(RINGIO_HAVE_LIBURING)
TEST(BufferPool, RegistersWithIoUring) {
  ::io_uring ring;
  const int init_ret = ::io_uring_queue_init(8, &ring, 0);
  if (init_ret < 0) {
    GTEST_SKIP() << "io_uring_queue_init unavailable in this environment: "
                 << init_ret;
  }

  BufferPool pool(4, 4096);
  const int ret = pool.register_with(&ring);
  EXPECT_EQ(ret, 0);

  ::io_uring_queue_exit(&ring);
}
#endif

}  // namespace
