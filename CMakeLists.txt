cmake_minimum_required(VERSION 3.16)
project(order_book_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENGINE_SOURCES
    src/order_book.cpp
    src/order_book_v2.cpp)

add_library(order_book STATIC ${ENGINE_SOURCES})
target_include_directories(order_book PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(tests
    tests/test_matching.cpp
    tests/test_invariants.cpp)
target_include_directories(tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(tests PRIVATE order_book)

# The benchmark recompiles engine sources so -O3 / NDEBUG cover the matching
# loop itself, not merely the benchmark-driver code.
add_executable(bench bench/bench_main.cpp ${ENGINE_SOURCES})
target_include_directories(bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/bench)
target_compile_definitions(bench PRIVATE NDEBUG)

if(MSVC)
    target_compile_options(order_book PRIVATE /W4)
    target_compile_options(tests PRIVATE /W4)
    target_compile_options(bench PRIVATE /O2)
    target_compile_options(bench PRIVATE /W4)
else()
    target_compile_options(order_book PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(bench PRIVATE -O3)
    target_compile_options(bench PRIVATE -Wall -Wextra -Wpedantic)
endif()
