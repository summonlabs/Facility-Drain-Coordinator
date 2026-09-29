// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/limits.hpp"

#include "facilitydrain/identity.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace facilitydrain {
namespace {

// "field max_text_bytes is 4, the bound is 8..4096"
[[nodiscard]] Status bound_failure(std::string_view field, std::uint64_t value, std::uint64_t low,
                                   std::uint64_t high) {
  std::string detail{"field "};
  detail.append(field);
  detail.append(" is ");
  detail.append(format_strong(value));
  detail.append(", the bound is ");
  detail.append(format_strong(low));
  detail.append("..");
  detail.append(format_strong(high));
  return Status::failure(ErrorCode::kLimitExceeded, detail);
}

}  // namespace

Status Limits::validate() const noexcept {
  // The order below is the contract. A limits value with several problems
  // always reports the same first one, so the same malformed configuration
  // produces the same code and the same detail text everywhere.
  constexpr std::uint64_t kUint32Maximum =
      static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());

  // 1..7: a zero collection bound would forbid the collection entirely, which
  // is never a useful configuration and is far more likely a missing field.
  if (max_plans == 0U) {
    return bound_failure("max_plans", max_plans, 1U, kUint32Maximum);
  }
  if (max_targets_per_plan == 0U) {
    return bound_failure("max_targets_per_plan", max_targets_per_plan, 1U, kUint32Maximum);
  }
  if (max_consumers_per_domain == 0U) {
    return bound_failure("max_consumers_per_domain", max_consumers_per_domain, 1U, kUint32Maximum);
  }
  if (max_evidence_per_domain == 0U) {
    return bound_failure("max_evidence_per_domain", max_evidence_per_domain, 1U, kUint32Maximum);
  }
  if (max_requests_per_plan == 0U) {
    return bound_failure("max_requests_per_plan", max_requests_per_plan, 1U, kUint32Maximum);
  }
  if (max_residuals_per_plan == 0U) {
    return bound_failure("max_residuals_per_plan", max_residuals_per_plan, 1U, kUint32Maximum);
  }
  if (max_history_per_plan == 0U) {
    return bound_failure("max_history_per_plan", max_history_per_plan, 1U, kUint32Maximum);
  }

  // 8: text must at least hold a canonical identity and must stay small enough
  // that a record full of labels cannot become a payload of its own.
  if (max_text_bytes < 8U || max_text_bytes > 4096U) {
    return bound_failure("max_text_bytes", max_text_bytes, 8U, 4096U);
  }

  // 9: an annotation is free form prose, so it may be larger than a label but
  // never smaller than one, and never unbounded.
  if (max_annotation_bytes < max_text_bytes || max_annotation_bytes > 1048576U) {
    return bound_failure("max_annotation_bytes", max_annotation_bytes, max_text_bytes, 1048576U);
  }

  // 10: a durable state payload has to be large enough to hold a header, and is
  // capped at 8 GiB so that a size computation cannot be used to exhaust
  // memory on a machine that would otherwise accept the write.
  if (max_state_bytes < 4096ULL || max_state_bytes > 8589934592ULL) {
    return bound_failure("max_state_bytes", max_state_bytes, 4096ULL, 8589934592ULL);
  }

  // 11: a document is ingested only to be hashed, so it must be non-empty and
  // must be able to travel inside a state payload unchanged.
  if (max_document_bytes == 0ULL || max_document_bytes > max_state_bytes) {
    return bound_failure("max_document_bytes", max_document_bytes, 1ULL, max_state_bytes);
  }

  // 12: retrying is bounded, and a bound above 4096 is indistinguishable from
  // an unbounded retry loop.
  if (max_attempts_per_key == 0U || max_attempts_per_key > 4096U) {
    return bound_failure("max_attempts_per_key", max_attempts_per_key, 1U, 4096U);
  }

  return Status::success();
}

}  // namespace facilitydrain
