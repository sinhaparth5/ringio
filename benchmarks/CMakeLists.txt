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
)
target_link_libraries(ringio_benchmarks PRIVATE ringio::ringio benchmark::benchmark_main)
