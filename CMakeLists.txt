cmake_minimum_required(VERSION 3.21)

project(mars_allocator
  VERSION 0.1.0
  DESCRIPTION "Fault-tolerant memory allocator with integrity checking"
  LANGUAGES C)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(MARS_WERROR   "Treat compiler warnings as errors"        OFF)
option(MARS_STATS    "Compile in allocation counters"           ON)
option(MARS_BUILD_BENCH "Build the benchmark harness"           ON)
option(MARS_COVERAGE "Instrument for gcov/lcov coverage"        OFF)

# Integrity profile. Selects the block layout and how much validation is done.
set(MARS_PROFILE "hardened" CACHE STRING
    "Integrity profile: fast | hardened | paranoid")
set_property(CACHE MARS_PROFILE PROPERTY STRINGS fast hardened paranoid)

set(MARS_SANITIZE "none" CACHE STRING
    "Sanitizer: none | address | thread")
set_property(CACHE MARS_SANITIZE PROPERTY STRINGS none address thread)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Keep the shared library beside the executables so test binaries resolve it
# from the build tree on every platform (DLL search path on Windows, RPATH on
# ELF).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Shared compile settings
# ---------------------------------------------------------------------------

add_library(mars_flags INTERFACE)

target_compile_features(mars_flags INTERFACE c_std_11)

if(MSVC)
  target_compile_options(mars_flags INTERFACE /W4)
  if(MARS_WERROR)
    target_compile_options(mars_flags INTERFACE /WX)
  endif()
else()
  target_compile_options(mars_flags INTERFACE
    -Wall -Wextra
    -Wshadow
    -Wcast-align
    -Wpointer-arith
    -Wvla)
  if(MARS_WERROR)
    target_compile_options(mars_flags INTERFACE -Werror)
  endif()
endif()

# Stricter still. Kept separate from mars_flags so that any third-party or
# generated source added later can opt out without loosening the baseline.
add_library(mars_flags_strict INTERFACE)
if(NOT MSVC)
  target_compile_options(mars_flags_strict INTERFACE
    -Wstrict-prototypes
    -Wmissing-prototypes)
endif()

string(TOUPPER "${MARS_PROFILE}" _mars_profile_upper)
target_compile_definitions(mars_flags INTERFACE
  MARS_PROFILE_${_mars_profile_upper}=1)

# Sanitizers and coverage are applied to every target so that instrumentation
# is consistent across the library boundary.
if(NOT MARS_SANITIZE STREQUAL "none")
  if(MSVC)
    message(FATAL_ERROR "MARS_SANITIZE is not supported with MSVC")
  endif()
  if(MARS_SANITIZE STREQUAL "address")
    set(_mars_san_flags -fsanitize=address,undefined
                        -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
  elseif(MARS_SANITIZE STREQUAL "thread")
    set(_mars_san_flags -fsanitize=thread -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "Unknown MARS_SANITIZE value: ${MARS_SANITIZE}")
  endif()
  target_compile_options(mars_flags INTERFACE ${_mars_san_flags} -g)
  target_link_options(mars_flags INTERFACE ${_mars_san_flags})
endif()

if(MARS_COVERAGE)
  if(MSVC)
    message(FATAL_ERROR "MARS_COVERAGE is not supported with MSVC")
  endif()
  target_compile_options(mars_flags INTERFACE --coverage -O0 -g)
  target_link_options(mars_flags INTERFACE --coverage)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

# Shared on ELF platforms: the library is consumed via LD_PRELOAD there, so the
# .so is the artifact that matters. Static on Windows, which is a secondary
# platform used only to keep the core and tests portable -- and where producing
# an import library depends on a binutils feature that is not reliably present.
if(WIN32)
  set(MARS_LIB_TYPE STATIC)
else()
  set(MARS_LIB_TYPE SHARED)
endif()

set(MARS_SOURCES
  src/mm_core.c
  src/mm_arena.c
  src/mm_freelist.c
  src/mm_integrity.c
  src/mm_access.c
  src/mm_stats.c
  src/mm_crc32.c)

add_library(mars ${MARS_LIB_TYPE} ${MARS_SOURCES})

add_library(mars::mars ALIAS mars)

target_include_directories(mars
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(mars
  PUBLIC  mars_flags
  PRIVATE mars_flags_strict)

if(MARS_STATS)
  target_compile_definitions(mars PUBLIC MM_STATS=1)
endif()

set_target_properties(mars PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)

if(MARS_LIB_TYPE STREQUAL "SHARED")
  set_target_properties(mars PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

# ---------------------------------------------------------------------------
# The LD_PRELOAD shim
# ---------------------------------------------------------------------------

# Unix only, and deliberately so: LD_PRELOAD is the mechanism, and a growable
# arena -- which is what the shim needs and what mm_arena.c only provides over
# mmap -- is the other half of it.
#
# Built from the allocator sources rather than linked against libmars.so. A
# preload library has to be loadable by `LD_PRELOAD=./libmars_preload.so prog`
# with nothing else set, and linking to a second shared object would make that
# depend on LD_LIBRARY_PATH or on an RPATH baked in at build time. Compiling
# the sources twice costs a second and removes the whole question.
#
# **Not built under a sanitizer, and not under coverage.** ASan replaces malloc
# with its own, so a build in which both exist is a build where which one runs
# is a link-order accident -- the two would fight, and whichever won, the result
# would say nothing about either. TSan is the same. The core is still fully
# sanitized: every other target here is built, and the shim is the one thing
# left out. Coverage is excluded for a different reason: this target compiles
# src/ a second time, and a second set of counters over the same source files
# would be merged into the gate as though the shim's copy were the library's.
#
# The consequence is that the shim itself is not covered by the 90% line gate
# in .github/workflows/ci.yml, which the workflow says in as many words.
set(MARS_BUILD_SHIM OFF)
if(UNIX AND MARS_SANITIZE STREQUAL "none" AND NOT MARS_COVERAGE)
  set(MARS_BUILD_SHIM ON)
endif()

if(MARS_BUILD_SHIM)
  add_library(mars_preload SHARED shim/mm_shim.c ${MARS_SOURCES})
  target_include_directories(mars_preload
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(mars_preload
    PRIVATE mars_flags mars_flags_strict ${CMAKE_DL_LIBS})
  if(MARS_STATS)
    target_compile_definitions(mars_preload PRIVATE MM_STATS=1)
  endif()
  set_target_properties(mars_preload PROPERTIES
    OUTPUT_NAME mars_preload
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
endif()

include(CTest)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(BUILD_TESTING)
  add_library(mars_test STATIC tests/mars_test.c)
  target_include_directories(mars_test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(mars_test PUBLIC mars_flags PRIVATE mars_flags_strict)
  set_target_properties(mars_test PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

  # add_test() with a timeout: a corrupted-link test that regresses would
  # otherwise hang the run rather than fail it.
  function(mars_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE mars mars_test mars_flags_strict)
    # Tests reach into the internal header to corrupt blocks deliberately.
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(${name} PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  endfunction()

  mars_add_test(test_crc32       tests/test_crc32.c)
  mars_add_test(test_layout      tests/test_layout.c)
  mars_add_test(test_core        tests/test_core.c)
  mars_add_test(test_arena       tests/test_arena.c)
  mars_add_test(test_freelist    tests/test_freelist.c)
  mars_add_test(test_access      tests/test_access.c)
  mars_add_test(test_realloc     tests/test_realloc.c)
  mars_add_test(test_integrity   tests/test_integrity.c)
  mars_add_test(test_regressions tests/test_regressions.c)

  # Linked against the preload library rather than against libmars, so that
  # every allocation it makes -- its own and the test framework's -- goes
  # through the shim. Built only where the shim is; see MARS_BUILD_SHIM above
  # for why a sanitized build is not one of those places.
  if(MARS_BUILD_SHIM)
    add_executable(test_shim tests/test_shim.c)
    target_link_libraries(test_shim
      PRIVATE mars_preload mars_test mars_flags mars_flags_strict)
    target_include_directories(test_shim
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
              ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(test_shim PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME test_shim COMMAND test_shim)
    set_tests_properties(test_shim PROPERTIES TIMEOUT 120)
  endif()

  # The fuzzer brings its own main() and CLI, so it is registered directly
  # rather than through mars_add_test. CI runs a short bounded pass; longer
  # runs are launched by hand with --seconds or --ops.
  add_executable(fuzz_shadow tests/fuzz_shadow.c)
  target_link_libraries(fuzz_shadow PRIVATE mars mars_flags mars_flags_strict)
  target_include_directories(fuzz_shadow
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src)
  set_target_properties(fuzz_shadow PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME fuzz_shadow_smoke
           COMMAND fuzz_shadow --seed 20260809 --ops 20000)
  set_tests_properties(fuzz_shadow_smoke PROPERTIES TIMEOUT 300)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(MARS_BUILD_BENCH)
  add_executable(bench
    bench/bench_main.c
    bench/bench_timer.c
    bench/workloads.c)
  target_link_libraries(bench PRIVATE mars mars_flags mars_flags_strict)
  target_include_directories(bench
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  set_target_properties(bench PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

  # Prices the fresh-mapping shortcut in calloc. Deliberately linked against
  # nothing of ours: it has to run under LD_PRELOAD against whichever allocator
  # is being measured, glibc included, so it must not have this one linked in.
  if(UNIX)
    add_executable(calloc_probe bench/calloc_probe.c)
    set_target_properties(calloc_probe PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  endif()

  # The histogram decides every percentile that gets reported, so it is tested
  # on its own rather than trusted because the benchmark runs.
  if(BUILD_TESTING)
    add_executable(test_bench_hist tests/test_bench_hist.c bench/bench_timer.c)
    target_link_libraries(test_bench_hist PRIVATE mars_test mars_flags
                                                  mars_flags_strict)
    target_include_directories(test_bench_hist
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bench)
    set_target_properties(test_bench_hist PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_test(NAME test_bench_hist COMMAND test_bench_hist)
    set_tests_properties(test_bench_hist PROPERTIES TIMEOUT 60)
  endif()

  # A tiny run, purely to catch the harness breaking. Real measurements are
  # taken deliberately, not as a side effect of running the tests.
  if(BUILD_TESTING)
    add_test(NAME bench_smoke
             COMMAND bench --ops 2000 --reps 1 --arena 4194304 --quiet)
    set_tests_properties(bench_smoke PROPERTIES TIMEOUT 300)
  endif()
endif()

# ---------------------------------------------------------------------------
# Fault injection
# ---------------------------------------------------------------------------

# Forks a child per trial so that a crashing or hanging trial is recorded as a
# result rather than ending the run. That makes it Unix-only.
if(UNIX)
  add_executable(faultinject tools/faultinject/fi_main.c)
  target_link_libraries(faultinject PRIVATE mars mars_flags mars_flags_strict m)
  target_include_directories(faultinject
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  set_target_properties(faultinject PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

  if(BUILD_TESTING)
    add_test(NAME faultinject_smoke
             COMMAND faultinject --trials 25 --bits 1 --target alloc_hdr)
    set_tests_properties(faultinject_smoke PROPERTIES TIMEOUT 300)

    # The two trials that caught the arena promise being broken under `fast`,
    # replayed by seed. A sweep large enough to hit a 1-in-120,000 event again
    # by chance does not belong in the test suite; replaying the exact trials
    # that found it costs milliseconds and is what actually stops it coming
    # back. --fail-on-crash is what turns the outcome into an exit code.
    #
    # Registered for every profile, not only for `fast`. The promise is
    # unconditional, so a cell that only holds under the profiles carrying a
    # checksum is a cell that was never evidence for the promise at all.
    #
    # See docs/FAULT_MODEL.md for how a per-trial seed maps back to a cell of
    # the full run: these are trials 7341 and 9149.
    foreach(_case "2;7361282833" "4;9169288258")
      list(GET _case 0 _bits)
      list(GET _case 1 _seed)
      add_test(NAME faultinject_arena_promise_free_hdr_${_bits}bit
               COMMAND faultinject --trials 1 --bits ${_bits}
                       --target free_hdr --seed ${_seed} --fail-on-crash)
      set_tests_properties(faultinject_arena_promise_free_hdr_${_bits}bit
                           PROPERTIES TIMEOUT 60)
    endforeach()
  endif()
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "mars_allocator ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  profile    : ${MARS_PROFILE}")
message(STATUS "  sanitizer  : ${MARS_SANITIZE}")
message(STATUS "  coverage   : ${MARS_COVERAGE}")
message(STATUS "  warn=error : ${MARS_WERROR}")
message(STATUS "  counters   : ${MARS_STATS}")
