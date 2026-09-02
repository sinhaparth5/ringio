#pragma once

#include <cstdint>
#include <type_traits>

namespace ringio {

// The kernel-bypass I/O opcode a request represents.
enum class IoOp : std::uint8_t { kRead, kWrite };

// One zero-allocation submission request. Worker threads push these into a
// SpscRing<IoRequest, N> or MpmcRing<IoRequest, N>; the engine's drain loop
// pops them and translates each into a fixed-file, fixed-buffer io_uring
// SQE. Trivially copyable so it can live directly in a lock-free ring slot.
struct IoRequest {
  IoOp op = IoOp::kRead;

  // Index into the engine's registered file table (SqpollEngine::register_files),
  // not a raw file descriptor.
  std::int32_t fixed_fd_index = -1;

  // Index into a BufferPool, both for locating the buffer and as the
  // registered-buffer index io_uring needs for a *_fixed op.
  std::uint32_t buffer_index = 0;

  std::uint64_t offset = 0;
  std::uint32_t length = 0;

  // Opaque, echoed back verbatim on the matching CQE (io_uring_cqe::user_data)
  // so Phase 4's completion engine can correlate it back to this request.
  std::uint64_t token = 0;
};

static_assert(std::is_trivially_copyable_v<IoRequest>);

}  // namespace ringio
