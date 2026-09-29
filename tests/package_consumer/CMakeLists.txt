# Facility Drain Coordinator — DCCP physical fleet lifecycle.
# Copyright 2026 Summon Software Labs.
# Apache License 2.0. No telemetry transmission.

# A downstream consumer of the installed Facility Drain Coordinator package.
#
# This project lives outside the library's source tree on purpose: it is
# configured only against an installed prefix with find_package, never against
# the build tree, so it proves that the package exports everything a consumer
# needs. Configure it with CMAKE_PREFIX_PATH pointing at the install prefix.

cmake_minimum_required(VERSION 3.20)

project(FacilityDrainCoordinatorConsumer
        VERSION 1.0.0
        DESCRIPTION "Independent consumer of the Facility Drain Coordinator package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FacilityDrainCoordinator 1.0 REQUIRED CONFIG)

add_executable(fdc_consumer main.cpp)
target_link_libraries(fdc_consumer PRIVATE FacilityDrainCoordinator::facility_drain_coordinator)

if(MSVC)
  target_compile_options(fdc_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8 /EHsc)
  target_compile_definitions(fdc_consumer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(fdc_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# A library built with a sanitizer must be consumed by a program built with the
# same sanitizer: the instrumentation calls into the runtime, and the runtime is
# linked by the flag. Passing it here keeps the installed artifact test meaningful
# in a sanitizer build instead of excluding it from one.
if(FDC_CONSUMER_SANITIZERS)
  if(MSVC)
    target_compile_options(fdc_consumer PRIVATE /fsanitize=address)
    target_link_options(fdc_consumer PRIVATE /fsanitize=address)
  else()
    target_compile_options(fdc_consumer PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(fdc_consumer PRIVATE -fsanitize=address,undefined)
  endif()
endif()

message(STATUS "consumer: FacilityDrainCoordinator ${FacilityDrainCoordinator_VERSION} found")
