// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/clock.hpp"

#include <chrono>
#include <cstdint>

namespace facilitydrain {

// Out of line so that the vtable and the deleting destructor live in exactly
// one translation unit, which is what keeps the shared library build well
// formed.
Clock::~Clock() = default;

std::int64_t SystemClock::now_milliseconds() const {
  const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
  return static_cast<std::int64_t>(milliseconds.count());
}

}  // namespace facilitydrain
