cmake_minimum_required(VERSION 3.14)
project(allocators LANGUAGES CXX)

include(CheckCXXSourceCompiles)

# --- Feature option: tracked memory ---
# allocators.cc used Xapiand's project-wide "config.h" for XAPIAND_TRACKED_MEM:
# when defined, the library overrides the global operator new/delete to account
# every byte and the tracked_allocator keeps per-thread / total counters; when
# not, total_allocated()/local_allocated() are stubs returning 0 and no global
# new/delete is overridden. Standalone, that is a build choice exposed as a CMake
# option (default OFF, matching Xapiand where tracking is opt-in). The option
# #cmakedefine's XAPIAND_TRACKED_MEM into the generated allocators_config.h; the
# macro name is kept verbatim so the source stays a faithful copy of upstream.
option(ALLOCATORS_TRACKED_MEM "Override global new/delete and track allocated bytes" OFF)
set(XAPIAND_TRACKED_MEM ${ALLOCATORS_TRACKED_MEM})

# --- Feature detection: thread_local ---
# allocators.cc used config.h (and an in-source compiler probe) for
# HAS_THREAD_LOCAL, which selects the thread_local counter over a mutex-guarded
# global map. Standalone, the library detects it itself: we compile a tiny
# program that uses thread_local. C++20 always has it, so this is defined on any
# supported toolchain; the probe stays for parity with how `times` guarded
# HAVE_CLOCK_GETTIME, and as a real check on exotic toolchains. The in-source
# fallback in allocators.cc still runs if this header isn't present.
check_cxx_source_compiles(
  "thread_local int x = 0; int main() { return x; }"
  HAS_THREAD_LOCAL
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/allocators_config.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/allocators_config.h"
  @ONLY
)

# --- Library target ---
# allocators is not header-only: allocators.cc carries the malloc/free wrappers,
# the byte accounting, and (when ALLOCATORS_TRACKED_MEM is on) the global
# new/delete overrides, so it must be compiled and linked. The repo root holds
# allocators.h; the build dir holds the generated allocators_config.h, so both
# go on the include path.
add_library(allocators allocators.cc)
target_include_directories(allocators PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_features(allocators PUBLIC cxx_std_20)

# Alias so consumers can link `allocators::allocators` whether they add this via
# add_subdirectory or FetchContent.
add_library(allocators::allocators ALIAS allocators)

# Only build the test when this repo is the top-level project, so consumers
# (e.g. via FetchContent) don't build our tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(allocators_test test/test.cc)
  target_link_libraries(allocators_test PRIVATE allocators)
  add_test(NAME allocators COMMAND allocators_test)

  # The tracked mode (global new/delete override) as a second copy of the library,
  # so CI tests both modes whatever ALLOCATORS_TRACKED_MEM is.
  add_library(allocators_tracked STATIC allocators.cc)
  target_include_directories(allocators_tracked PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
  target_compile_features(allocators_tracked PUBLIC cxx_std_20)
  target_compile_definitions(allocators_tracked PUBLIC XAPIAND_TRACKED_MEM=1)
  add_executable(allocators_tracked_test test/test.cc)
  target_link_libraries(allocators_tracked_test PRIVATE allocators_tracked)
  target_compile_definitions(allocators_tracked_test PRIVATE ALLOCATORS_EXPECT_TRACKED=1)
  add_test(NAME allocators_tracked COMMAND allocators_tracked_test)
  if(NOT ALLOCATORS_TRACKED_MEM)
    target_compile_definitions(allocators_test PRIVATE ALLOCATORS_EXPECT_TRACKED=0)
  endif()

  add_executable(allocators_demo examples/demo.cc)
  target_link_libraries(allocators_demo PRIVATE allocators_tracked)
endif()
