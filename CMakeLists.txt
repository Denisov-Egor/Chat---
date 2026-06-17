cmake_minimum_required(VERSION 3.14)
project(chat_tests)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Только test_chat.cpp, который включает chat.cpp
add_executable(test_chat test_chat.cpp)

target_link_libraries(test_chat gtest_main pthread)
target_compile_definitions(test_chat PRIVATE TESTING)