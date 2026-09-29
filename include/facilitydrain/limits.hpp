// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_LIMITS_HPP
#define FACILITYDRAIN_LIMITS_HPP

#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"

#include <cstdint>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
//
// Every collection, string and durable payload has a hard bound. A limit is not
// a suggestion: exceeding one is a rejection with a limit error, never a
// silently truncated result. Limits are recorded in the durable state, so a
// store reopened with smaller limits than it was written with is rejected
// rather than reinterpreted.

struct Limits {
  /// Plans held by one coordinator.
  std::uint32_t max_plans = 4096;
  /// Physical targets declared inside one plan's scope manifest.
  std::uint32_t max_targets_per_plan = 4096;
  /// Consumer records (obligations) accepted for one plan and domain.
  std::uint32_t max_consumers_per_domain = 200000;
  /// Completion evidence records retained per plan and domain.
  std::uint32_t max_evidence_per_domain = 4096;
  /// Drain requests retained per plan.
  std::uint32_t max_requests_per_plan = 8192;
  /// Residual ledger entries retained per plan.
  std::uint32_t max_residuals_per_plan = 200000;
  /// Recorded state transitions retained per plan (the audit trail bound).
  std::uint32_t max_history_per_plan = 4096;
  /// Bytes of an identity, label or source string.
  std::uint32_t max_text_bytes = 256;
  /// Bytes of a free form annotation or explanation string.
  std::uint32_t max_annotation_bytes = 2048;
  /// Bytes of one canonical state payload written to, or read from, a store.
  std::uint64_t max_state_bytes = 268435456ULL;
  /// Bytes of one consumer or policy document ingested as a digest input.
  std::uint64_t max_document_bytes = 67108864ULL;
  /// Attempts allowed for one request key before the caller must supersede
  /// explicitly.
  std::uint32_t max_attempts_per_key = 64;

  /// Full validation, used when limits arrive from outside the process. The
  /// checks are ordered so that a limits value with several problems always
  /// reports the same primary code.
  [[nodiscard]] FACILITYDRAIN_API Status validate() const noexcept;
};

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_LIMITS_HPP
