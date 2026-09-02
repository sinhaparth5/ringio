#if defined(RINGIO_HAVE_LIBURING)

#include "ringio/sqpoll_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "ringio/detail/buffer_pool.hpp"
#include "ringio/detail/spsc_ring.hpp"
#include "ringio/io_completion.hpp"
#include "ringio/io_request.hpp"

namespace {

using ringio::BufferPool;
using ringio::IoCompletion;
using ringio::IoOp;
using ringio::IoRequest;
using ringio::SqpollEngine;
using ringio::detail::SpscRing;

// Polls `pred` until it's true or `timeout` elapses, yielding between
// attempts. harvest_completions() and io_uring_cq_ready() are both
// syscall-free, so a fixed iteration count is not a reliable way to wait
// for the SQPOLL kernel thread: a tight spin loop can burn through tens of
// thousands of iterations in under a millisecond, well before that thread
// even gets its first scheduling slice, especially right after it's spun
// up. A wall-clock bound is what actually survives real hardware/VM
// scheduling jitter.
template <typename Predicate>
bool WaitUntil(Predicate pred, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (pred()) return true;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return pred();
}

// Some sandboxes (restrictive seccomp, RLIMIT_MEMLOCK too low, a kernel
// without CONFIG_IO_URING_SQPOLL) refuse to stand up an SQPOLL ring at all;
// skip those tests rather than fail so the suite still passes there. The
// real target environment for this code is a GCE VM with a normal kernel,
// where these tests are expected to run for real, not skip.
std::unique_ptr<SqpollEngine> TryCreateEngine(unsigned entries = 32) {
  try {
    return std::make_unique<SqpollEngine>(entries);
  } catch (const std::system_error&) {
    return nullptr;
  }
}

TEST(SqpollEngine, RejectsZeroEntries) {
  EXPECT_THROW(SqpollEngine(0), std::system_error);
}

TEST(SqpollEngine, DrainAndSubmitReturnsZeroWhenQueueEmpty) {
  auto engine = TryCreateEngine();
  if (!engine) GTEST_SKIP() << "SQPOLL io_uring unavailable in this environment";

  BufferPool pool(1, 4096);
  SpscRing<IoRequest, 8> queue;
  EXPECT_EQ(engine->drain_and_submit(queue, pool), 0u);
}

// End-to-end: register a real file and a real buffer pool, submit a fixed
// write followed by a fixed read through the SQPOLL ring, and check the
// bytes that come back match what was written.
TEST(SqpollEngine, WriteThenReadRoundTripsThroughFixedFileAndBuffer) {
  auto engine = TryCreateEngine();
  if (!engine) GTEST_SKIP() << "SQPOLL io_uring unavailable in this environment";

  char path[] = "/tmp/ringio_sqpoll_test_XXXXXX";
  const int fd = ::mkstemp(path);
  ASSERT_GE(fd, 0);
  ::unlink(path);  // the fd stays valid; nothing is left behind on disk

  ASSERT_EQ(engine->register_files(std::span<const int>(&fd, 1)), 0);

  BufferPool pool(2, 4096);
  ASSERT_EQ(engine->register_buffers(pool), 0);

  const std::uint32_t write_buf = pool.acquire();
  const std::uint32_t read_buf = pool.acquire();
  ASSERT_NE(write_buf, BufferPool::kInvalidIndex);
  ASSERT_NE(read_buf, BufferPool::kInvalidIndex);

  constexpr std::uint32_t kLen = 4096;
  std::memset(pool.data(write_buf), 0xAB, kLen);
  std::memset(pool.data(read_buf), 0, kLen);

  SpscRing<IoRequest, 8> queue;

  IoRequest write_req{};
  write_req.op = IoOp::kWrite;
  write_req.fixed_fd_index = 0;
  write_req.buffer_index = write_buf;
  write_req.offset = 0;
  write_req.length = kLen;
  write_req.token = 111;
  ASSERT_TRUE(queue.try_push(write_req));
  ASSERT_EQ(engine->drain_and_submit(queue, pool), 1u);

  ::io_uring_cqe* cqe = nullptr;
  ASSERT_EQ(::io_uring_wait_cqe(engine->native_handle(), &cqe), 0);
  EXPECT_EQ(cqe->user_data, 111u);
  EXPECT_EQ(cqe->res, static_cast<int>(kLen));
  ::io_uring_cqe_seen(engine->native_handle(), cqe);

  IoRequest read_req{};
  read_req.op = IoOp::kRead;
  read_req.fixed_fd_index = 0;
  read_req.buffer_index = read_buf;
  read_req.offset = 0;
  read_req.length = kLen;
  read_req.token = 222;
  ASSERT_TRUE(queue.try_push(read_req));
  ASSERT_EQ(engine->drain_and_submit(queue, pool), 1u);

  cqe = nullptr;
  ASSERT_EQ(::io_uring_wait_cqe(engine->native_handle(), &cqe), 0);
  EXPECT_EQ(cqe->user_data, 222u);
  EXPECT_EQ(cqe->res, static_cast<int>(kLen));
  ::io_uring_cqe_seen(engine->native_handle(), cqe);

  EXPECT_EQ(std::memcmp(pool.data(read_buf), pool.data(write_buf), kLen), 0);

  pool.release(write_buf);
  pool.release(read_buf);
}

TEST(SqpollEngine, HarvestCompletionsReturnsZeroWhenNothingPending) {
  auto engine = TryCreateEngine();
  if (!engine) GTEST_SKIP() << "SQPOLL io_uring unavailable in this environment";

  SpscRing<IoCompletion, 8> completions;
  EXPECT_EQ(engine->harvest_completions(completions), 0u);
}

// Same round trip as WriteThenReadRoundTripsThroughFixedFileAndBuffer, but
// driven entirely through the ringio API (drain_and_submit +
// harvest_completions) instead of raw liburing calls, to exercise the
// completion engine as callers actually use it.
TEST(SqpollEngine, WriteCompletionArrivesThroughHarvestCompletions) {
  auto engine = TryCreateEngine();
  if (!engine) GTEST_SKIP() << "SQPOLL io_uring unavailable in this environment";

  char path[] = "/tmp/ringio_sqpoll_harvest_test_XXXXXX";
  const int fd = ::mkstemp(path);
  ASSERT_GE(fd, 0);
  ::unlink(path);

  ASSERT_EQ(engine->register_files(std::span<const int>(&fd, 1)), 0);

  BufferPool pool(1, 4096);
  ASSERT_EQ(engine->register_buffers(pool), 0);

  const std::uint32_t buf = pool.acquire();
  ASSERT_NE(buf, BufferPool::kInvalidIndex);
  constexpr std::uint32_t kLen = 4096;
  std::memset(pool.data(buf), 0x42, kLen);

  SpscRing<IoRequest, 8> submit_queue;
  IoRequest req{};
  req.op = IoOp::kWrite;
  req.fixed_fd_index = 0;
  req.buffer_index = buf;
  req.offset = 0;
  req.length = kLen;
  req.token = 77;
  ASSERT_TRUE(submit_queue.try_push(req));
  ASSERT_EQ(engine->drain_and_submit(submit_queue, pool), 1u);

  SpscRing<IoCompletion, 8> completions;
  const bool delivered =
      WaitUntil([&] { return engine->harvest_completions(completions) > 0; });
  ASSERT_TRUE(delivered) << "completion never arrived within the timeout";
  IoCompletion completion{};
  ASSERT_TRUE(completions.try_pop(completion));
  EXPECT_EQ(completion.token, 77u);
  EXPECT_EQ(completion.result, static_cast<int>(kLen));

  pool.release(buf);
}

// max_batch caps how many CQEs a single harvest_completions() call walks,
// even when more are ready in the CQ ring.
TEST(SqpollEngine, HarvestCompletionsRespectsMaxBatch) {
  auto engine = TryCreateEngine();
  if (!engine) GTEST_SKIP() << "SQPOLL io_uring unavailable in this environment";

  char path[] = "/tmp/ringio_sqpoll_maxbatch_test_XXXXXX";
  const int fd = ::mkstemp(path);
  ASSERT_GE(fd, 0);
  ::unlink(path);
  ASSERT_EQ(engine->register_files(std::span<const int>(&fd, 1)), 0);

  constexpr std::uint32_t kLen = 4096;
  constexpr int kRequests = 4;
  BufferPool pool(kRequests, kLen);
  ASSERT_EQ(engine->register_buffers(pool), 0);
  SpscRing<IoRequest, 8> submit_queue;

  for (int i = 0; i < kRequests; ++i) {
    const std::uint32_t buf = pool.acquire();
    ASSERT_NE(buf, BufferPool::kInvalidIndex);
    std::memset(pool.data(buf), 0xCD, kLen);
    IoRequest req{};
    req.op = IoOp::kWrite;
    req.fixed_fd_index = 0;
    req.buffer_index = buf;
    req.offset = static_cast<std::uint64_t>(i) * kLen;
    req.length = kLen;
    req.token = static_cast<std::uint64_t>(i) + 1;
    ASSERT_TRUE(submit_queue.try_push(req));
  }
  ASSERT_EQ(engine->drain_and_submit(submit_queue, pool), static_cast<std::size_t>(kRequests));

  // Wait for all four writes to complete on the kernel side without
  // consuming them, so the max_batch check below sees a full CQ ring
  // rather than racing the SQPOLL thread.
  ::io_uring* ring = engine->native_handle();
  const bool ready = WaitUntil(
      [&] { return ::io_uring_cq_ready(ring) >= static_cast<unsigned>(kRequests); });
  ASSERT_TRUE(ready) << "writes never completed within the timeout";

  SpscRing<IoCompletion, 8> completions;
  EXPECT_EQ(engine->harvest_completions(completions, 2), 2u);
  EXPECT_EQ(engine->harvest_completions(completions, 2), 2u);

  std::vector<std::uint64_t> tokens;
  IoCompletion completion{};
  while (completions.try_pop(completion)) {
    EXPECT_EQ(completion.result, static_cast<int>(kLen));
    tokens.push_back(completion.token);
  }
  std::sort(tokens.begin(), tokens.end());
  EXPECT_EQ(tokens, (std::vector<std::uint64_t>{1, 2, 3, 4}));
}

}  // namespace

#endif  // RINGIO_HAVE_LIBURING
