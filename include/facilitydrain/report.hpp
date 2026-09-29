// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_REPORT_HPP
#define FACILITYDRAIN_REPORT_HPP

#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/snapshot.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Canonical rendering
// ---------------------------------------------------------------------------
//
// The text and JSON renderings are canonical: field order is fixed, records are
// emitted in their canonical order, no wall clock timestamp of the rendering
// itself is included, and there are no floating point values. Two runs over the
// same durable state produce byte identical output on every machine.

struct ReportOptions {
  bool include_provenance = true;
  bool include_consumers = true;
  bool include_residuals = true;
  bool include_requests = true;
  bool include_history = true;
  bool include_evaluations = true;
  /// Indentation for JSON. Text output ignores it.
  bool pretty = true;
  /// Additional bound on rendered plans. Zero means the snapshot bound applies.
  std::uint32_t max_plans = 0;
  /// Additional bound on rendered residual entries per plan. Zero means all.
  std::uint32_t max_residuals = 0;
  /// Additional bound on rendered requests per plan. Zero means all.
  std::uint32_t max_requests = 0;
};

/// The first line of every text report is
/// "format facility-drain-coordinator/1".
[[nodiscard]] FACILITYDRAIN_API Result<std::string> export_plan_text(const DrainPlanSnapshot& plan,
                                                                    const ReportOptions& options = {});
[[nodiscard]] FACILITYDRAIN_API Result<std::string> export_plan_json(const DrainPlanSnapshot& plan,
                                                                    const ReportOptions& options = {});
[[nodiscard]] FACILITYDRAIN_API Result<std::string> export_coordinator_text(const CoordinatorSnapshot& snapshot,
                                                                           const ReportOptions& options = {});
[[nodiscard]] FACILITYDRAIN_API Result<std::string> export_coordinator_json(const CoordinatorSnapshot& snapshot,
                                                                           const ReportOptions& options = {});

/// Escapes one UTF-8 string as a JSON string body, without the quotes.
[[nodiscard]] FACILITYDRAIN_API std::string json_escape(std::string_view text);

/// The deterministic human readable explanation of a verdict, one line per
/// domain in canonical order, then the blocking summary. This is what the
/// explanation command prints.
[[nodiscard]] FACILITYDRAIN_API std::string format_evaluation(const SafeToRemoveEvaluation& evaluation);

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_REPORT_HPP
