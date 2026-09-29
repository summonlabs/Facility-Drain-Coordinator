# Facility Drain Coordinator - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the examples, the benchmarks and the tests each carry
# exactly the options they need, and nothing this project uses for itself can
# leak into the installed package.

cmake_minimum_required(VERSION 3.20)

project(FacilityDrainCoordinator
        VERSION 1.0.0
        DESCRIPTION "Physical fleet drain coordination for the Data Center Control Plane"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build facility_drain_coordinator as a shared library instead of a static one" OFF)

option(FDC_BUILD_TESTS "Build the Facility Drain Coordinator test suite" ON)
option(FDC_BUILD_TOOLS "Build the Facility Drain Coordinator command line tools" ON)
option(FDC_BUILD_EXAMPLES "Build the Facility Drain Coordinator example programs" ON)
option(FDC_BUILD_BENCHMARKS "Build the Facility Drain Coordinator benchmarks" ON)
option(FDC_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(FDC_SANITIZERS "Build with AddressSanitizer, and UndefinedBehaviorSanitizer on GCC and Clang" OFF)
option(FDC_ANALYZE "Build with the MSVC static analyzer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXCompilerFlag)
include(CTest)

set(FDC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/FacilityDrainCoordinator"
    CACHE STRING "Install destination of the Facility Drain Coordinator CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it is part of the installed package.
function(fdc_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(FDC_WARNINGS_AS_ERRORS AND NOT FDC_ANALYZE)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/RTC1>)
    target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=2>)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Release>:/O2>
      $<$<CONFIG:Release>:/Gy>
      $<$<CONFIG:Release>:/Gw>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(FDC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(MSVC AND FDC_ANALYZE)
    target_compile_options(${target} PRIVATE /analyze /analyze:WX-)
  endif()
  if(FDC_SANITIZERS)
    target_compile_options(${target} PRIVATE ${FDC_SANITIZER_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${FDC_SANITIZER_LINK_OPTIONS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers and static analysis
# ---------------------------------------------------------------------------

if(FDC_SANITIZERS)
  if(MSVC)
    check_cxx_compiler_flag("/fsanitize=address" FDC_COMPILER_HAS_ASAN)
    if(NOT FDC_COMPILER_HAS_ASAN)
      message(FATAL_ERROR
        "FDC_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: a program built with /fsanitize=address does not compile and "
        "link here. Either the toolset predates AddressSanitizer support (Visual Studio 2019 16.9) or "
        "the optional C++ AddressSanitizer component is not installed. Reconfigure with "
        "-DFDC_SANITIZERS=OFF or install that component.")
    endif()
    set(FDC_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi)
    set(FDC_SANITIZER_LINK_OPTIONS /fsanitize=address)
    message(STATUS "FacilityDrainCoordinator: AddressSanitizer enabled (MSVC)")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    check_cxx_compiler_flag("-fsanitize=address,undefined" FDC_COMPILER_HAS_ASAN_UBSAN)
    if(NOT FDC_COMPILER_HAS_ASAN_UBSAN)
      message(FATAL_ERROR
        "FDC_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: the sanitizer runtime is unavailable. Reconfigure with "
        "-DFDC_SANITIZERS=OFF.")
    endif()
    set(FDC_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(FDC_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
    message(STATUS "FacilityDrainCoordinator: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  else()
    message(FATAL_ERROR
      "FDC_SANITIZERS=ON is supported with MSVC (/fsanitize=address) and with GCC or Clang "
      "(-fsanitize=address,undefined); the configured compiler is ${CMAKE_CXX_COMPILER_ID} "
      "${CMAKE_CXX_COMPILER_VERSION}. Reconfigure with -DFDC_SANITIZERS=OFF.")
  endif()
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(FDC_PUBLIC_HEADERS
  include/facilitydrain/clock.hpp
  include/facilitydrain/consumer.hpp
  include/facilitydrain/coordinator.hpp
  include/facilitydrain/digest.hpp
  include/facilitydrain/errors.hpp
  include/facilitydrain/evaluation.hpp
  include/facilitydrain/evidence.hpp
  include/facilitydrain/export.hpp
  include/facilitydrain/facility_drain_coordinator.hpp
  include/facilitydrain/generations.hpp
  include/facilitydrain/identity.hpp
  include/facilitydrain/limits.hpp
  include/facilitydrain/persistence.hpp
  include/facilitydrain/plan.hpp
  include/facilitydrain/report.hpp
  include/facilitydrain/requests.hpp
  include/facilitydrain/residual.hpp
  include/facilitydrain/scope.hpp
  include/facilitydrain/snapshot.hpp
  include/facilitydrain/version.hpp)

set(FDC_SOURCES
  src/byte_codec.cpp
  src/clock.cpp
  src/consumer.cpp
  src/coordinator.cpp
  src/coordinator_mutations.cpp
  src/crc32.cpp
  src/digest.cpp
  src/errors.cpp
  src/evaluation.cpp
  src/evidence.cpp
  src/file_lock.cpp
  src/file_ops.cpp
  src/generations.cpp
  src/identity.cpp
  src/limits.cpp
  src/plan.cpp
  src/plan_facts.cpp
  src/report.cpp
  src/requests.cpp
  src/residual.cpp
  src/scope.cpp
  src/sha256.cpp
  src/state_codec.cpp
  src/store.cpp
  src/utf8.cpp
  src/version.cpp)

add_library(facility_drain_coordinator ${FDC_SOURCES})
add_library(FacilityDrainCoordinator::facility_drain_coordinator ALIAS facility_drain_coordinator)

target_include_directories(facility_drain_coordinator
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(facility_drain_coordinator PUBLIC cxx_std_20)
set_target_properties(facility_drain_coordinator PROPERTIES
  OUTPUT_NAME facility_drain_coordinator
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  target_compile_definitions(facility_drain_coordinator PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

fdc_configure_target(facility_drain_coordinator)

# ---------------------------------------------------------------------------
# Tools, examples and benchmarks
# ---------------------------------------------------------------------------

function(fdc_add_console_target target)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE FacilityDrainCoordinator::facility_drain_coordinator)
  fdc_configure_target(${target})
endfunction()

set(FDC_TOOL_TARGETS)
if(FDC_BUILD_TOOLS)
  fdc_add_console_target(fdc tools/fdc_cli.cpp)
  list(APPEND FDC_TOOL_TARGETS fdc)
endif()

set(FDC_EXAMPLE_SOURCES
  examples/01_create_plan.cpp
  examples/02_enumerate_and_issue.cpp
  examples/03_ingest_evidence.cpp
  examples/04_residual_ledger.cpp
  examples/05_safe_to_remove.cpp
  examples/06_cancel_and_fence.cpp
  examples/07_restart_and_replay.cpp
  examples/08_explain_why_not.cpp)

if(FDC_BUILD_EXAMPLES)
  foreach(source IN LISTS FDC_EXAMPLE_SOURCES)
    string(SUBSTRING "${source}" 9 2 FDC_EXAMPLE_NUMBER)
    set(FDC_EXAMPLE_TARGET "fdc_example_${FDC_EXAMPLE_NUMBER}")
    fdc_add_console_target(${FDC_EXAMPLE_TARGET} ${source})
  endforeach()
endif()

if(FDC_BUILD_BENCHMARKS)
  fdc_add_console_target(fdc_benchmark benchmarks/benchmark_drain.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

set(FDC_TEST_SOURCES
  tests/test_adversarial.cpp
  tests/test_authority.cpp
  tests/test_codec.cpp
  tests/test_concurrency.cpp
  tests/test_consumer.cpp
  tests/test_corruption.cpp
  tests/test_crash.cpp
  tests/test_digest.cpp
  tests/test_enumeration.cpp
  tests/test_errors.cpp
  tests/test_evaluation.cpp
  tests/test_evidence.cpp
  tests/test_fencing.cpp
  tests/test_ids.cpp
  tests/test_limits.cpp
  tests/test_lifecycle.cpp
  tests/test_multiprocess.cpp
  tests/test_persistence.cpp
  tests/test_plan.cpp
  tests/test_property.cpp
  tests/test_recovery.cpp
  tests/test_replay.cpp
  tests/test_report.cpp
  tests/test_requests.cpp
  tests/test_residuals.cpp
  tests/test_scope.cpp
  tests/test_version.cpp)

if(FDC_BUILD_TESTS)
  add_executable(fdc_tests
    ${FDC_TEST_SOURCES}
    tests/support/test_harness.cpp
    tests/support/test_main.cpp
    tests/support/test_process.cpp)
  target_link_libraries(fdc_tests PRIVATE FacilityDrainCoordinator::facility_drain_coordinator)
  target_include_directories(fdc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  fdc_configure_target(fdc_tests)

  # The test binary re-executes itself to obtain real independent operating
  # system processes for the shared-writer, restart, crash and replay tests.
  add_test(NAME fdc.tests COMMAND fdc_tests)

  if(FDC_BUILD_EXAMPLES)
    foreach(source IN LISTS FDC_EXAMPLE_SOURCES)
      string(SUBSTRING "${source}" 9 2 FDC_EXAMPLE_NUMBER)
      add_test(NAME fdc.example.${FDC_EXAMPLE_NUMBER} COMMAND fdc_example_${FDC_EXAMPLE_NUMBER})
    endforeach()
  endif()

  if(FDC_BUILD_TOOLS)
    add_test(NAME fdc.cli.selfcheck COMMAND fdc --self-check)
    add_test(NAME fdc.cli.scenario COMMAND fdc scenario --root "${CMAKE_CURRENT_BINARY_DIR}/cli-scenario")
  endif()

  if(FDC_BUILD_BENCHMARKS)
    add_test(NAME fdc.benchmark.smoke COMMAND fdc_benchmark --quick)
  endif()

  # The installed artifact is exercised by an independent out of tree consumer
  # that knows nothing about this build tree: it installs to a scratch prefix,
  # configures the consumer with find_package, builds it and runs it.
  #
  # The toolchain values are passed through because a test runner does not
  # necessarily inherit a developer environment, and the SDK library and include
  # paths are passed as escaped lists so a bare ctest still links.
  string(REPLACE ";" "\\;" FDC_TEST_LIB_DIRS "${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}")
  string(REPLACE ";" "\\;" FDC_TEST_INCLUDE_DIRS "${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}")
  set(FDC_CONSUMER_TEST_ENVIRONMENT "")
  if(DEFINED ENV{LIB})
    # The caller already has one; the test must not narrow it.
  else()
    list(APPEND FDC_CONSUMER_TEST_ENVIRONMENT "LIB=${FDC_TEST_LIB_DIRS}")
  endif()
  if(DEFINED ENV{INCLUDE})
  else()
    list(APPEND FDC_CONSUMER_TEST_ENVIRONMENT "INCLUDE=${FDC_TEST_INCLUDE_DIRS}")
  endif()

  add_test(NAME fdc.package.consumer
           COMMAND ${CMAKE_COMMAND}
                   -DFDC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                   -DFDC_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                   -DFDC_GENERATOR=${CMAKE_GENERATOR}
                   -DFDC_CONFIG=$<CONFIG>
                   -DFDC_SANITIZERS=${FDC_SANITIZERS}
                   -DFDC_LINK_DIRECTORIES=${FDC_TEST_LIB_DIRS}
                   -DFDC_INCLUDE_DIRECTORIES=${FDC_TEST_INCLUDE_DIRS}
                   "-DFDC_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                   "-DFDC_RC_COMPILER=${CMAKE_RC_COMPILER}"
                   "-DFDC_MT=${CMAKE_MT}"
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/package_consumer/run_consumer_test.cmake)
  if(FDC_CONSUMER_TEST_ENVIRONMENT)
    set_tests_properties(fdc.package.consumer PROPERTIES ENVIRONMENT "${FDC_CONSUMER_TEST_ENVIRONMENT}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS facility_drain_coordinator
  EXPORT FacilityDrainCoordinatorTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES ${FDC_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/facilitydrain)

if(FDC_BUILD_TOOLS)
  install(TARGETS ${FDC_TOOL_TARGETS} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT FacilityDrainCoordinatorTargets
  FILE FacilityDrainCoordinatorTargets.cmake
  NAMESPACE FacilityDrainCoordinator::
  DESTINATION ${FDC_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FacilityDrainCoordinatorConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDrainCoordinatorConfig.cmake
  INSTALL_DESTINATION ${FDC_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDrainCoordinatorConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDrainCoordinatorConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FacilityDrainCoordinatorConfigVersion.cmake
  DESTINATION ${FDC_INSTALL_CMAKEDIR})

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/FacilityDrainCoordinator)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "FacilityDrainCoordinator ${PROJECT_VERSION}")
message(STATUS "  compiler            : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  warnings as errors  : ${FDC_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${FDC_SANITIZERS}")
message(STATUS "  tests/tools/examples: ${FDC_BUILD_TESTS}/${FDC_BUILD_TOOLS}/${FDC_BUILD_EXAMPLES}")