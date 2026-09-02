#if defined(RINGIO_HAVE_LIBURING)

#include "ringio/sqpoll_engine.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <system_error>

#include <unistd.h>

#include <gtest/gtest.h>

#include "ringio/detail/buffer_pool.hpp"
#include "ringio/detail/spsc_ring.hpp"
#include "ringio/io_request.hpp"

namespace {

using ringio::BufferPool;
using ringio::IoOp;
using ringio::IoRequest;
using ringio::SqpollEngine;
using ringio::detail::SpscRing;

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

}  // namespace

#endif  // RINGIO_HAVE_LIBURING
