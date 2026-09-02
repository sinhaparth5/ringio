include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.5
)
FetchContent_MakeAvailable(benchmark)

add_executable(ringio_benchmarks
  cache_line_benchmark.cpp
  buffer_pool_benchmark.cpp
  ring_benchmark.cpp
  completion_engine_benchmark.cpp
  iops_throughput_benchmark.cpp
)
target_link_libraries(ringio_benchmarks PRIVATE ringio::ringio benchmark::benchmark_main)

# libaio is only needed by iops_throughput_benchmark.cpp's baseline
# comparison, not by the library itself, so it's detected here rather than
# in the root CMakeLists.txt.
find_path(LIBAIO_INCLUDE_DIR libaio.h)
find_library(LIBAIO_LIBRARY aio)
if(LIBAIO_INCLUDE_DIR AND LIBAIO_LIBRARY)
  target_include_directories(ringio_benchmarks PRIVATE ${LIBAIO_INCLUDE_DIR})
  target_link_libraries(ringio_benchmarks PRIVATE ${LIBAIO_LIBRARY})
  target_compile_definitions(ringio_benchmarks PRIVATE RINGIO_HAVE_LIBAIO)
  message(STATUS "ringio: found libaio (${LIBAIO_LIBRARY}) -- libaio IOPS baseline enabled")
else()
  message(STATUS
    "ringio: libaio not found -- libaio IOPS baseline skipped (install "
    "libaio-dev to enable)")
endif()
