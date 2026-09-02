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
option(RINGIO_ENABLE_TSAN "Build with ThreadSanitizer (-fsanitize=thread)" OFF)
option(RINGIO_ENABLE_ASAN "Build with AddressSanitizer (-fsanitize=address)" OFF)
option(RINGIO_BUILD_EXPERIMENTS
  "Build exploratory microarchitecture experiments under experiments/ -- \
throwaway investigation code, not part of the library or its CI-tracked \
results" OFF)

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

# ASan and TSan link incompatible runtimes into the same binary, so only one
# can be active at a time. Configure a separate build directory for each
# (e.g. `cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DRINGIO_ENABLE_TSAN=ON`)
# rather than toggling this option in place -- CMake doesn't clean up flags
# from a prior configure of the same build directory.
if(RINGIO_ENABLE_TSAN AND RINGIO_ENABLE_ASAN)
  message(FATAL_ERROR
    "ringio: RINGIO_ENABLE_TSAN and RINGIO_ENABLE_ASAN are mutually "
    "exclusive -- configure separate build directories for each.")
endif()
if(RINGIO_ENABLE_TSAN)
  target_compile_options(ringio INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(ringio INTERFACE -fsanitize=thread)
  message(STATUS "ringio: building with ThreadSanitizer")
elseif(RINGIO_ENABLE_ASAN)
  target_compile_options(ringio INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(ringio INTERFACE -fsanitize=address)
  message(STATUS "ringio: building with AddressSanitizer")
endif()

if(RINGIO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RINGIO_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(RINGIO_BUILD_EXPERIMENTS)
  if(NOT (LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY))
    message(FATAL_ERROR "ringio: RINGIO_BUILD_EXPERIMENTS needs liburing-dev")
  endif()
  add_subdirectory(experiments)
endif()
