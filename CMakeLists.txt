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

# liburing is only needed by the code paths that call into it directly
# (buffer/file registration, submission), but we wire the detection up once
# so later phases don't need to touch this file. Headers that need it guard
# the include and any liburing-calling members behind RINGIO_HAVE_LIBURING.
find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)
if(LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
  target_include_directories(ringio INTERFACE ${LIBURING_INCLUDE_DIR})
  target_link_libraries(ringio INTERFACE ${LIBURING_LIBRARY})
  target_compile_definitions(ringio INTERFACE RINGIO_HAVE_LIBURING)
  message(STATUS "ringio: found liburing (${LIBURING_LIBRARY})")
else()
  message(WARNING
    "ringio: liburing not found. Buffer/file registration and submission "
    "need it; install with `sudo apt install liburing-dev` and re-run "
    "cmake. Everything else still builds without it.")
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
