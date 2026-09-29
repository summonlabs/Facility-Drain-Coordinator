// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_CLOCK_HPP
#define FACILITYDRAIN_CLOCK_HPP

#include "facilitydrain/export.hpp"

#include <cstdint>
#include <memory>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
//
// Wall clock time is diagnostic. It is never authority, never an ordering key
// for evidence, and never a fencing device: evidence is ordered by the
// observation sequence and the evidence generation the producer stamped on it.
// The clock exists so records can carry a human readable when, and so the
// canonical report can be reproduced from a fixed clock in tests.

class FACILITYDRAIN_API Clock {
 public:
  Clock() = default;
  virtual ~Clock();
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  /// Milliseconds since the Unix epoch. Monotonicity is not required and is not
  /// relied upon.
  [[nodiscard]] virtual std::int64_t now_milliseconds() const = 0;
};

/// Reads the host clock.
class FACILITYDRAIN_API SystemClock final : public Clock {
 public:
  [[nodiscard]] std::int64_t now_milliseconds() const override;
};

/// Returns one fixed value for its whole lifetime. Used by tests and by the
/// reproducible CLI scenarios, so that a run is byte identical everywhere.
class FACILITYDRAIN_API FixedClock final : public Clock {
 public:
  explicit FixedClock(std::int64_t milliseconds) noexcept : now_(milliseconds) {}

  [[nodiscard]] std::int64_t now_milliseconds() const override { return now_; }
  void set(std::int64_t milliseconds) noexcept { now_ = milliseconds; }

 private:
  std::int64_t now_ = 0;
};

using ClockPtr = std::shared_ptr<const Clock>;

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_CLOCK_HPP
