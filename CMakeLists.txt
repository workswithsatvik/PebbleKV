cmake_minimum_required(VERSION 3.20)
project(PebbleKV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(PEBBLEKV_ENABLE_TSAN "Build with ThreadSanitizer (GCC/Clang)" OFF)

find_package(Threads REQUIRED)

add_library(pebblekv src/bloom_filter.cpp src/pebble_kv.cpp)
target_include_directories(pebblekv PUBLIC include)
target_link_libraries(pebblekv PUBLIC Threads::Threads)

if(MSVC)
  target_compile_options(pebblekv PUBLIC /W4)
else()
  target_compile_options(pebblekv PUBLIC -Wall -Wextra -Wpedantic)
endif()

if(PEBBLEKV_ENABLE_TSAN)
  target_compile_options(pebblekv PUBLIC -fsanitize=thread -g)
  target_link_options(pebblekv PUBLIC -fsanitize=thread)
endif()

add_executable(pebblekv_benchmark src/benchmark.cpp)
target_link_libraries(pebblekv_benchmark PRIVATE pebblekv)

add_executable(pebblekv_tests tests/test_pebble_kv.cpp)
target_link_libraries(pebblekv_tests PRIVATE pebblekv)

enable_testing()
add_test(NAME pebblekv_tests COMMAND pebblekv_tests)
