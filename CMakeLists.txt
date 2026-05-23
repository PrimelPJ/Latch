cmake_minimum_required(VERSION 3.16)
project(latch VERSION 1.0.0 LANGUAGES CXX)

# ── Language ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)        # no GNU extensions — stay portable

# ── Build type defaults ──────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Compiler flags ───────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3 -march=native -DNDEBUG)
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-O0 -g -fsanitize=address,thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,thread)
endif()

# ── Interface (header-only) library ─────────────────────────────────────
add_library(latch INTERFACE)
target_include_directories(latch INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# ── Find pthreads ────────────────────────────────────────────────────────
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── Tests ────────────────────────────────────────────────────────────────
add_executable(run_tests tests/test_all.cpp)
target_link_libraries(run_tests PRIVATE latch Threads::Threads)

# ── Benchmarks ───────────────────────────────────────────────────────────
add_executable(bench benchmarks/bench.cpp)
target_link_libraries(bench PRIVATE latch Threads::Threads)

# ── CTest integration ────────────────────────────────────────────────────
enable_testing()
add_test(NAME unit_tests COMMAND run_tests)

# ── Install ──────────────────────────────────────────────────────────────
install(DIRECTORY include/latch DESTINATION include)
