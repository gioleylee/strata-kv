cmake_minimum_required(VERSION 3.16)
project(strata LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(strata src/db.cc)
target_include_directories(strata PUBLIC include)
if(MSVC)
  target_compile_options(strata PRIVATE /W4)
else()
  target_compile_options(strata PRIVATE -Wall -Wextra)
endif()

add_executable(strata_tests tests/test_main.cc)
target_link_libraries(strata_tests PRIVATE strata)

add_executable(strata_bench bench/bench.cc)
target_link_libraries(strata_bench PRIVATE strata)

enable_testing()
add_test(NAME strata_tests COMMAND strata_tests)
