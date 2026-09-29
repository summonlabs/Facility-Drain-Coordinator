# Facility Drain Coordinator — DCCP physical fleet lifecycle.
# Copyright 2026 Summon Software Labs.
# Apache License 2.0. No telemetry transmission.

# Proves that the installed package is usable by an independent consumer.
#
# The script installs the already built project into a scratch prefix under the
# build tree, configures tests/package_consumer against that prefix with
# find_package (never against the build tree), builds it, and runs it. The first
# step that does not succeed stops the script with a FATAL_ERROR that names the
# step, the command, the exit code and the output.
#
# It removes nothing. The scratch prefix and the consumer build tree are left
# exactly as the failing step left them, so a failure can be inspected; the
# script is deterministic and independent of the current working directory, and
# every path it touches is derived from its four inputs:
#
#   cmake -DFDC_SOURCE_DIR=<source tree>
#         -DFDC_BINARY_DIR=<build tree holding the built project>
#         -DFDC_GENERATOR=<cmake generator, e.g. "Ninja">
#         -DFDC_CONFIG=<configuration, e.g. "Release"; empty for a single
#                       configuration generator with no build type>
#         -P tests/package_consumer/run_consumer_test.cmake
#
# FDC_CONFIG may be empty but must be defined; FDC_SOURCE_DIR, FDC_BINARY_DIR
# and FDC_GENERATOR must be non empty.

cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------

if(NOT DEFINED FDC_CONFIG)
  set(FDC_CONFIG "")
endif()

foreach(fdc_required IN ITEMS FDC_SOURCE_DIR FDC_BINARY_DIR FDC_GENERATOR)
  if(NOT DEFINED ${fdc_required} OR "${${fdc_required}}" STREQUAL "")
    message(FATAL_ERROR
      "run_consumer_test.cmake needs -D${fdc_required}=... . Run it the way the test suite does: "
      "cmake -DFDC_SOURCE_DIR=<source> -DFDC_BINARY_DIR=<build> -DFDC_GENERATOR=<generator> "
      "-DFDC_CONFIG=<config> -P tests/package_consumer/run_consumer_test.cmake")
  endif()
endforeach()

get_filename_component(FDC_SOURCE_DIR "${FDC_SOURCE_DIR}" ABSOLUTE)
get_filename_component(FDC_BINARY_DIR "${FDC_BINARY_DIR}" ABSOLUTE)

set(fdc_consumer_source_dir "${FDC_SOURCE_DIR}/tests/package_consumer")
set(fdc_test_root "${FDC_BINARY_DIR}/package-test")
set(fdc_prefix_dir "${fdc_test_root}/prefix")
set(fdc_consumer_build_dir "${fdc_test_root}/build")

if(NOT EXISTS "${fdc_consumer_source_dir}/CMakeLists.txt")
  message(FATAL_ERROR
    "the package consumer project is missing: ${fdc_consumer_source_dir}/CMakeLists.txt does not exist. "
    "FDC_SOURCE_DIR is '${FDC_SOURCE_DIR}'.")
endif()

if(NOT EXISTS "${FDC_BINARY_DIR}/CMakeCache.txt")
  message(FATAL_ERROR
    "the project has not been configured in '${FDC_BINARY_DIR}': CMakeCache.txt is missing there, so there "
    "is nothing to install. Build the project first, then run this script.")
endif()

# A multi configuration generator selects the configuration on the command
# line; a single configuration generator needs it at configure time instead.
set(fdc_multi_config FALSE)
if(FDC_GENERATOR MATCHES "Visual Studio|Xcode|Ninja Multi-Config")
  set(fdc_multi_config TRUE)
endif()

# The executable suffix of the host this script runs on.
set(fdc_executable_suffix "")
if(CMAKE_HOST_WIN32)
  set(fdc_executable_suffix ".exe")
endif()

# The path separator of the host, used when the run step has to make an
# installed shared library findable.
set(fdc_path_separator ":")
if(CMAKE_HOST_WIN32)
  set(fdc_path_separator ";")
endif()

# ---------------------------------------------------------------------------
# A stale consumer build tree is refused, never deleted
# ---------------------------------------------------------------------------
#
# Reconfiguring an existing tree works as long as the generator matches. When it
# does not, CMake refuses, and the reason is worth stating in full instead of
# letting the configure step report it indirectly.

if(EXISTS "${fdc_consumer_build_dir}/CMakeCache.txt")
  file(STRINGS "${fdc_consumer_build_dir}/CMakeCache.txt" fdc_cached_generator
       REGEX "^CMAKE_GENERATOR:INTERNAL=")
  if(NOT "${fdc_cached_generator}" STREQUAL "")
    string(REPLACE "CMAKE_GENERATOR:INTERNAL=" "" fdc_cached_generator "${fdc_cached_generator}")
    if(NOT "${fdc_cached_generator}" STREQUAL "${FDC_GENERATOR}")
      message(FATAL_ERROR
        "the package consumer build tree at '${fdc_consumer_build_dir}' was configured with generator "
        "'${fdc_cached_generator}', but this run uses '${FDC_GENERATOR}'. This script removes nothing, so "
        "that directory has to be deleted by hand before the test can run again.")
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Runs one step and stops with the whole picture when it fails.
function(fdc_run_step step_label)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE fdc_step_result
    OUTPUT_VARIABLE fdc_step_stdout
    ERROR_VARIABLE fdc_step_stderr)
  if(NOT fdc_step_result STREQUAL "0")
    message(FATAL_ERROR
      "package consumer: ${step_label} failed with exit code '${fdc_step_result}'.\n"
      "  command: ${ARGN}\n"
      "  stdout:\n${fdc_step_stdout}\n"
      "  stderr:\n${fdc_step_stderr}\n"
      "Nothing was removed: the failed step's tree is still there to inspect.")
  endif()
  set(fdc_step_stdout "${fdc_step_stdout}" PARENT_SCOPE)
  set(fdc_step_stderr "${fdc_step_stderr}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 1. Install the built project into the scratch prefix
# ---------------------------------------------------------------------------

message(STATUS "package consumer: installing '${FDC_BINARY_DIR}' into '${fdc_prefix_dir}'")

set(fdc_install_command "${CMAKE_COMMAND}" --install "${FDC_BINARY_DIR}" --prefix "${fdc_prefix_dir}")
if(NOT "${FDC_CONFIG}" STREQUAL "")
  list(APPEND fdc_install_command --config "${FDC_CONFIG}")
endif()
fdc_run_step("the install step" ${fdc_install_command})
if(NOT "${fdc_step_stdout}" STREQUAL "")
  message(STATUS "package consumer: install output:\n${fdc_step_stdout}")
endif()

# ---------------------------------------------------------------------------
# 2. Configure the out of tree consumer against the installed prefix
# ---------------------------------------------------------------------------

message(STATUS "package consumer: configuring '${fdc_consumer_source_dir}' into '${fdc_consumer_build_dir}'")

set(fdc_configure_command
    "${CMAKE_COMMAND}"
    -S "${fdc_consumer_source_dir}"
    -B "${fdc_consumer_build_dir}"
    -G "${FDC_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${fdc_prefix_dir}")
if(NOT "${FDC_CONFIG}" STREQUAL "" AND NOT fdc_multi_config)
  list(APPEND fdc_configure_command "-DCMAKE_BUILD_TYPE=${FDC_CONFIG}")
endif()
# A test runner does not necessarily inherit a compiler environment, so the
# compiler this project was built with is passed through when it is known. That
# keeps the step about the installed package rather than about PATH.
if(DEFINED FDC_CXX_COMPILER AND EXISTS "${FDC_CXX_COMPILER}")
  list(APPEND fdc_configure_command "-DCMAKE_CXX_COMPILER=${FDC_CXX_COMPILER}")
endif()
# The Windows resource compiler and manifest tool are not on PATH in a test
# runner, and an MSVC project cannot be generated without them. They are passed
# from the build that discovered them rather than guessed from a machine layout.
if(DEFINED FDC_RC_COMPILER AND EXISTS "${FDC_RC_COMPILER}")
  list(APPEND fdc_configure_command "-DCMAKE_RC_COMPILER=${FDC_RC_COMPILER}")
endif()
if(DEFINED FDC_MT AND EXISTS "${FDC_MT}")
  list(APPEND fdc_configure_command "-DCMAKE_MT=${FDC_MT}")
endif()
if(DEFINED FDC_SANITIZERS AND "${FDC_SANITIZERS}")
  list(APPEND fdc_configure_command "-DFDC_CONSUMER_SANITIZERS=ON")
endif()

# A test runner does not necessarily inherit the SDK library and include paths a
# linker needs (kernel32.lib and friends). They are supplied here from the build
# that discovered them, which makes this test independent of how ctest was
# launched, and they are never set when the caller already has them.
if(DEFINED FDC_LINK_DIRECTORIES AND NOT "${FDC_LINK_DIRECTORIES}" STREQUAL "")
  set(ENV{LIB} "${FDC_LINK_DIRECTORIES}")
endif()
if(DEFINED FDC_INCLUDE_DIRECTORIES AND NOT "${FDC_INCLUDE_DIRECTORIES}" STREQUAL "")
  set(ENV{INCLUDE} "${FDC_INCLUDE_DIRECTORIES}")
endif()
fdc_run_step("configuring the package consumer" ${fdc_configure_command})
if(NOT "${fdc_step_stdout}" STREQUAL "")
  message(STATUS "package consumer: configure output:\n${fdc_step_stdout}")
endif()

# ---------------------------------------------------------------------------
# 3. Build the consumer
# ---------------------------------------------------------------------------

message(STATUS "package consumer: building the consumer")

set(fdc_build_command "${CMAKE_COMMAND}" --build "${fdc_consumer_build_dir}")
if(NOT "${FDC_CONFIG}" STREQUAL "")
  list(APPEND fdc_build_command --config "${FDC_CONFIG}")
endif()
fdc_run_step("building the package consumer" ${fdc_build_command})
if(NOT "${fdc_step_stdout}" STREQUAL "")
  message(STATUS "package consumer: build output:\n${fdc_step_stdout}")
endif()

# ---------------------------------------------------------------------------
# 4. Locate and run the consumer executable
# ---------------------------------------------------------------------------
#
# A multi configuration generator puts the binary in a per configuration
# directory; a single configuration generator puts it in the build root. Both
# are tried, in that order, and a miss names every candidate.

set(fdc_executable_candidates)
if(NOT "${FDC_CONFIG}" STREQUAL "")
  list(APPEND fdc_executable_candidates
       "${fdc_consumer_build_dir}/${FDC_CONFIG}/fdc_consumer${fdc_executable_suffix}")
endif()
list(APPEND fdc_executable_candidates "${fdc_consumer_build_dir}/fdc_consumer${fdc_executable_suffix}")

set(fdc_consumer_executable "")
foreach(fdc_candidate IN LISTS fdc_executable_candidates)
  if(EXISTS "${fdc_candidate}")
    set(fdc_consumer_executable "${fdc_candidate}")
    break()
  endif()
endforeach()

if("${fdc_consumer_executable}" STREQUAL "")
  message(FATAL_ERROR
    "the package consumer executable was not found after a successful build. Tried:\n"
    "  ${fdc_executable_candidates}\n"
    "The build tree '${fdc_consumer_build_dir}' was left in place for inspection.")
endif()

message(STATUS "package consumer: running '${fdc_consumer_executable}'")

# An installed shared library lives in the prefix's bin directory on platforms
# without an rpath. Putting it first on PATH is what lets the installed package,
# rather than the build tree, satisfy the consumer at run time.
set(ENV{PATH} "${fdc_prefix_dir}/bin${fdc_path_separator}$ENV{PATH}")

fdc_run_step("running the package consumer" "${fdc_consumer_executable}")

message(STATUS "package consumer: ok\n${fdc_step_stdout}")
