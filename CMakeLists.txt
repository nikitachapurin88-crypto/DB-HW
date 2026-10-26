cmake_minimum_required(VERSION 3.10)
project(MiniDB CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(minidb_core table.cpp database.cpp cli.cpp)

add_executable(minidb main.cpp)
target_link_libraries(minidb minidb_core)

add_executable(minidb_tests tests.cpp)
target_link_libraries(minidb_tests minidb_core)

add_executable(minidb_bench bench.cpp)
target_link_libraries(minidb_bench minidb_core)

enable_testing()
add_test(NAME minidb_tests COMMAND minidb_tests)
