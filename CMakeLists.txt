cmake_minimum_required(VERSION 3.20)
project(ringio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RINGIO_BUILD_TESTS "Build the ringio test suite" ON)
option(RINGIO_BUILD_BENCHMARKS "Build the ringio benchmark suite" ON)
option(RINGIO_MARCH_NATIVE "Compile with -march=native in Release builds" ON)

# ringio itself is header-only.
add_library(ringio INTERFACE)
add_library(ringio::ringio ALIAS ringio)
target_include_directories(ringio INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(ringio INTERFACE cxx_std_20)

# liburing is only needed once Phase 3 starts issuing io_uring calls, but we
# wire the detection up now so later phases don't need to touch this file.
find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)
if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
  target_include_directories(ringio INTERFACE ${LIBURING_INCLUDE_DIR})
  target_link_libraries(ringio INTERFACE ${LIBURING_LIBRARY})
  message(STATUS "ringio: found liburing (${LIBURING_LIBRARY})")
else()
  message(WARNING
    "ringio: liburing not found. Code from Phase 3 onward needs it; install "
    "with `sudo apt install liburing-dev` and re-run cmake. Phase 1/2 "
    "targets are unaffected.")
endif()

if(RINGIO_MARCH_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" RINGIO_HAS_MARCH_NATIVE)
  if(RINGIO_HAS_MARCH_NATIVE)
    target_compile_options(ringio INTERFACE
      $<$<CONFIG:Release>:-O3 -march=native>
    )
  endif()
endif()

if(RINGIO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RINGIO_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
