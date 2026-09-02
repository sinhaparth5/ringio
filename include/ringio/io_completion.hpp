#pragma once

#include <cstdint>
#include <type_traits>

namespace ringio {

// One harvested completion, mirroring an io_uring_cqe: the token echoed
// back from the IoRequest that produced it, and the raw result (bytes
// transferred on success, -errno on failure). Trivially copyable so it can
// live directly in a lock-free ring slot, same as IoRequest.
//
// The token is the only correlation mechanism ringio provides — matching a
// completion back to whatever state a caller tracks for it (a request slot,
// a promise, a callback) is the caller's job. Completions arrive in
// whatever order the kernel finishes them in, not submission order.
struct IoCompletion {
  std::uint64_t token = 0;
  std::int32_t result = 0;
};

static_assert(std::is_trivially_copyable_v<IoCompletion>);

}  // namespace ringio
