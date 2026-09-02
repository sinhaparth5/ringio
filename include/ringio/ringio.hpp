#pragma once

// Umbrella header for ringio. Currently pulls in only the primitives that
// exist so far; each phase adds its own header here as it lands (buffer
// pool, submission ring, completion engine, ...).

#include "ringio/detail/cache_line.hpp"
#include "ringio/detail/buffer_pool.hpp"
