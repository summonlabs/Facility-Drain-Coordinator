// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using facilitydrain::ErrorCode;
using facilitydrain::Limits;
using facilitydrain::Status;

/// True when the diagnostic names the field that failed. The detail text is
/// diagnostic only, but it is deterministic, and naming the right field is what
/// makes a rejected configuration actionable.
[[nodiscard]] bool detail_names(const Status& status, std::string_view field) {
  return std::string_view{status.error().detail()}.find(field) != std::string_view::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// The default value
// ---------------------------------------------------------------------------

FDC_TEST(limits, default_value_is_valid) {
  const Limits limits;
  const Status status = limits.validate();
  FDC_CHECK(status.ok());
  FDC_CHECK_EQ(status.code(), ErrorCode::kOk);
  FDC_CHECK_EQ(status.to_text(), std::string{"ok"});

  // The defaults are the documented bounds, not merely a value that happens to
  // validate.
  FDC_CHECK_EQ(limits.max_plans, 4096U);
  FDC_CHECK_EQ(limits.max_targets_per_plan, 4096U);
  FDC_CHECK_EQ(limits.max_consumers_per_domain, 200000U);
  FDC_CHECK_EQ(limits.max_evidence_per_domain, 4096U);
  FDC_CHECK_EQ(limits.max_requests_per_plan, 8192U);
  FDC_CHECK_EQ(limits.max_residuals_per_plan, 200000U);
  FDC_CHECK_EQ(limits.max_history_per_plan, 4096U);
  FDC_CHECK_EQ(limits.max_text_bytes, 256U);
  FDC_CHECK_EQ(limits.max_annotation_bytes, 2048U);
  FDC_CHECK_EQ(limits.max_state_bytes, 268435456ULL);
  FDC_CHECK_EQ(limits.max_document_bytes, 67108864ULL);
  FDC_CHECK_EQ(limits.max_attempts_per_key, 64U);
}

FDC_TEST(limits, every_documented_bound_accepts_its_own_edges) {
  // The text and annotation bounds at the bottom of their ranges.
  Limits lowest_text;
  lowest_text.max_text_bytes = 8U;
  lowest_text.max_annotation_bytes = 8U;
  lowest_text.max_state_bytes = 4096ULL;
  lowest_text.max_document_bytes = 1ULL;
  lowest_text.max_attempts_per_key = 1U;
  FDC_CHECK(lowest_text.validate().ok());

  // The same bounds at the top of their ranges.
  Limits highest_text;
  highest_text.max_text_bytes = 4096U;
  highest_text.max_annotation_bytes = 1048576U;
  highest_text.max_state_bytes = 8589934592ULL;
  highest_text.max_document_bytes = 8589934592ULL;
  highest_text.max_attempts_per_key = 4096U;
  FDC_CHECK(highest_text.validate().ok());

  // A document exactly as large as a state payload is accepted.
  Limits equal_documents;
  equal_documents.max_state_bytes = 4096ULL;
  equal_documents.max_document_bytes = 4096ULL;
  FDC_CHECK(equal_documents.validate().ok());

  // An annotation exactly as small as a text field is accepted.
  Limits equal_text;
  equal_text.max_text_bytes = 1024U;
  equal_text.max_annotation_bytes = 1024U;
  FDC_CHECK(equal_text.validate().ok());

  // Every collection bound at its minimum of one.
  Limits single_entry;
  single_entry.max_plans = 1U;
  single_entry.max_targets_per_plan = 1U;
  single_entry.max_consumers_per_domain = 1U;
  single_entry.max_evidence_per_domain = 1U;
  single_entry.max_requests_per_plan = 1U;
  single_entry.max_residuals_per_plan = 1U;
  single_entry.max_history_per_plan = 1U;
  FDC_CHECK(single_entry.validate().ok());
}

// ---------------------------------------------------------------------------
// One test per documented boundary violation, in the documented order
// ---------------------------------------------------------------------------

FDC_TEST(limits, max_plans_zero) {
  Limits limits;
  limits.max_plans = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(), std::string{"field max_plans is 0, the bound is 1..4294967295"});
}

FDC_TEST(limits, max_targets_per_plan_zero) {
  Limits limits;
  limits.max_targets_per_plan = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(status, "max_targets_per_plan"));
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_targets_per_plan is 0, the bound is 1..4294967295"});
}

FDC_TEST(limits, max_consumers_per_domain_zero) {
  Limits limits;
  limits.max_consumers_per_domain = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_consumers_per_domain is 0, the bound is 1..4294967295"});
}

FDC_TEST(limits, max_evidence_per_domain_zero) {
  Limits limits;
  limits.max_evidence_per_domain = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(status, "max_evidence_per_domain"));
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_evidence_per_domain is 0, the bound is 1..4294967295"});
}

FDC_TEST(limits, max_requests_per_plan_zero) {
  Limits limits;
  limits.max_requests_per_plan = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_requests_per_plan is 0, the bound is 1..4294967295"});
}

FDC_TEST(limits, max_residuals_per_plan_zero) {
  Limits limits;
  limits.max_residuals_per_plan = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_residuals_per_plan is 0, the bound is 1..4294967295"});
}

FDC_TEST(limits, max_history_per_plan_zero) {
  Limits limits;
  limits.max_history_per_plan = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_history_per_plan is 0, the bound is 1..4294967295"});
}

FDC_TEST(limits, max_text_bytes_below_its_minimum) {
  Limits limits;
  limits.max_text_bytes = 7U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(), std::string{"field max_text_bytes is 7, the bound is 8..4096"});
}

FDC_TEST(limits, max_text_bytes_above_its_maximum) {
  Limits limits;
  limits.max_text_bytes = 4097U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(), std::string{"field max_text_bytes is 4097, the bound is 8..4096"});
}

FDC_TEST(limits, max_annotation_bytes_below_the_text_bound) {
  Limits limits;
  limits.max_text_bytes = 256U;
  limits.max_annotation_bytes = 255U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_annotation_bytes is 255, the bound is 256..1048576"});
}

FDC_TEST(limits, max_annotation_bytes_above_its_maximum) {
  Limits limits;
  limits.max_annotation_bytes = 1048577U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_annotation_bytes is 1048577, the bound is 256..1048576"});
}

FDC_TEST(limits, max_state_bytes_below_its_minimum) {
  Limits limits;
  limits.max_state_bytes = 4095ULL;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_state_bytes is 4095, the bound is 4096..8589934592"});
}

FDC_TEST(limits, max_state_bytes_above_its_maximum) {
  Limits limits;
  limits.max_state_bytes = 8589934593ULL;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_state_bytes is 8589934593, the bound is 4096..8589934592"});
}

FDC_TEST(limits, max_document_bytes_zero) {
  Limits limits;
  limits.max_document_bytes = 0ULL;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_document_bytes is 0, the bound is 1..268435456"});
}

FDC_TEST(limits, max_document_bytes_above_the_state_bound) {
  Limits limits;
  limits.max_state_bytes = 4096ULL;
  limits.max_document_bytes = 4097ULL;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_document_bytes is 4097, the bound is 1..4096"});
}

FDC_TEST(limits, max_attempts_per_key_zero) {
  Limits limits;
  limits.max_attempts_per_key = 0U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_attempts_per_key is 0, the bound is 1..4096"});
}

FDC_TEST(limits, max_attempts_per_key_above_its_maximum) {
  Limits limits;
  limits.max_attempts_per_key = 4097U;
  const Status status = limits.validate();
  FDC_CHECK_STATUS(status, ErrorCode::kLimitExceeded);
  FDC_CHECK_EQ(status.error().detail(),
               std::string{"field max_attempts_per_key is 4097, the bound is 1..4096"});
}

// ---------------------------------------------------------------------------
// Precedence
// ---------------------------------------------------------------------------

FDC_TEST(limits, the_first_violation_in_the_documented_order_wins) {
  // Two collection bounds at once: max_plans is checked before
  // max_history_per_plan.
  Limits limits;
  limits.max_plans = 0U;
  limits.max_history_per_plan = 0U;
  const Status collections = limits.validate();
  FDC_CHECK_STATUS(collections, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(collections, "max_plans"));
  FDC_CHECK(!detail_names(collections, "max_history_per_plan"));

  // max_requests_per_plan is checked before max_residuals_per_plan.
  limits = Limits{};
  limits.max_requests_per_plan = 0U;
  limits.max_residuals_per_plan = 0U;
  const Status requests = limits.validate();
  FDC_CHECK_STATUS(requests, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(requests, "max_requests_per_plan"));
  FDC_CHECK(!detail_names(requests, "max_residuals_per_plan"));

  // Every collection bound precedes max_text_bytes.
  limits = Limits{};
  limits.max_targets_per_plan = 0U;
  limits.max_text_bytes = 1U;
  const Status targets = limits.validate();
  FDC_CHECK_STATUS(targets, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(targets, "max_targets_per_plan"));
  FDC_CHECK(!detail_names(targets, "max_text_bytes"));

  // max_text_bytes precedes max_annotation_bytes.
  limits = Limits{};
  limits.max_text_bytes = 4U;
  limits.max_annotation_bytes = 0U;
  const Status text = limits.validate();
  FDC_CHECK_STATUS(text, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(text, "max_text_bytes"));
  FDC_CHECK(!detail_names(text, "max_annotation_bytes"));

  // max_annotation_bytes precedes max_state_bytes.
  limits = Limits{};
  limits.max_annotation_bytes = 1U;
  limits.max_state_bytes = 0ULL;
  const Status annotation = limits.validate();
  FDC_CHECK_STATUS(annotation, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(annotation, "max_annotation_bytes"));
  FDC_CHECK(!detail_names(annotation, "max_state_bytes"));

  // max_state_bytes precedes max_document_bytes.
  limits = Limits{};
  limits.max_state_bytes = 1ULL;
  limits.max_document_bytes = 0ULL;
  const Status state = limits.validate();
  FDC_CHECK_STATUS(state, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(state, "max_state_bytes"));
  FDC_CHECK(!detail_names(state, "max_document_bytes"));

  // max_document_bytes precedes max_attempts_per_key.
  limits = Limits{};
  limits.max_document_bytes = 0ULL;
  limits.max_attempts_per_key = 0U;
  const Status document = limits.validate();
  FDC_CHECK_STATUS(document, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(document, "max_document_bytes"));
  FDC_CHECK(!detail_names(document, "max_attempts_per_key"));

  // Everything wrong at once reports the very first check.
  limits = Limits{};
  limits.max_plans = 0U;
  limits.max_targets_per_plan = 0U;
  limits.max_consumers_per_domain = 0U;
  limits.max_evidence_per_domain = 0U;
  limits.max_requests_per_plan = 0U;
  limits.max_residuals_per_plan = 0U;
  limits.max_history_per_plan = 0U;
  limits.max_text_bytes = 0U;
  limits.max_annotation_bytes = 0U;
  limits.max_state_bytes = 0ULL;
  limits.max_document_bytes = 0ULL;
  limits.max_attempts_per_key = 0U;
  const Status everything = limits.validate();
  FDC_CHECK_STATUS(everything, ErrorCode::kLimitExceeded);
  FDC_CHECK(detail_names(everything, "max_plans"));
  FDC_CHECK(!detail_names(everything, "max_attempts_per_key"));
}
