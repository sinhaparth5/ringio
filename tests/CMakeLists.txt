include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.16.0
)
FetchContent_MakeAvailable(googletest)

add_executable(ringio_tests
  detail/cache_line_test.cpp
)
target_link_libraries(ringio_tests PRIVATE ringio::ringio GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(ringio_tests)
