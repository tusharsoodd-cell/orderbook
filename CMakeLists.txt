cmake_minimum_required(VERSION 3.16)
project(orderbook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

# the book + feed parser as a static lib
add_library(ob STATIC src/book.cpp src/feed.cpp)
target_include_directories(ob PUBLIC src)

# benchmark harness
add_executable(bench src/bench.cpp)
target_link_libraries(bench PRIVATE ob)

# fuzz driver (text op log -> trades + book dump)
add_executable(replay tools/replay.cpp)
target_link_libraries(replay PRIVATE ob)

# unit tests (no framework, just asserts)
add_executable(unit_tests tests/test_main.cpp)
target_link_libraries(unit_tests PRIVATE ob)

enable_testing()
add_test(NAME unit_tests COMMAND unit_tests)
