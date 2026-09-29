// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/evidence.hpp"
#include "utf8.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {

std::string_view to_token(CoverageState state) noexcept {
  switch (state) {
    case CoverageState::kNotEnumerated:
      return "not-enumerated";
    case CoverageState::kPartial:
      return "partial";
    case CoverageState::kComplete:
      return "complete";
    case CoverageState::kFailed:
      return "failed";
  }
  return std::string_view{};
}

std::optional<CoverageState> coverage_state_from_token(std::string_view token) noexcept {
  if (token == "not-enumerated") {
    return CoverageState::kNotEnumerated;
  }
  if (token == "partial") {
    return CoverageState::kPartial;
  }
  if (token == "complete") {
    return CoverageState::kComplete;
  }
  if (token == "failed") {
    return CoverageState::kFailed;
  }
  return std::nullopt;
}

std::string_view to_token(CompletionState state) noexcept {
  switch (state) {
    case CompletionState::kUnknown:
      return "unknown";
    case CompletionState::kRequested:
      return "requested";
    case CompletionState::kAcknowledged:
      return "acknowledged";
    case CompletionState::kDraining:
      return "draining";
    case CompletionState::kDrainedWithResiduals:
      return "drained-with-residuals";
    case CompletionState::kDrained:
      return "drained";
    case CompletionState::kRefused:
      return "refused";
    case CompletionState::kFailed:
      return "failed";
  }
  return std::string_view{};
}

std::optional<CompletionState> completion_state_from_token(std::string_view token) noexcept {
  if (token == "unknown") {
    return CompletionState::kUnknown;
  }
  if (token == "requested") {
    return CompletionState::kRequested;
  }
  if (token == "acknowledged") {
    return CompletionState::kAcknowledged;
  }
  if (token == "draining") {
    return CompletionState::kDraining;
  }
  if (token == "drained-with-residuals") {
    return CompletionState::kDrainedWithResiduals;
  }
  if (token == "drained") {
    return CompletionState::kDrained;
  }
  if (token == "refused") {
    return CompletionState::kRefused;
  }
  if (token == "failed") {
    return CompletionState::kFailed;
  }
  return std::nullopt;
}

bool is_terminal_completion(CompletionState state) noexcept {
  // Only these two states claim the drain itself is finished. An
  // acknowledgement is a received message, and a refusal is a decision: neither
  // is an effect, and neither completes a domain.
  return state == CompletionState::kDrained || state == CompletionState::kDrainedWithResiduals;
}

Status EnumerationEvidence::validate(const Limits& limits) const {
  // Fixed order: generation, coverage, the digests, the generation set, the
  // observation sequence, then the text. Validation stops at the first
  // violation, so the same record always reports the same primary code.
  if (generation.value() == 0) {
    // A default generation means never observed, and an observation that never
    // happened cannot be evidence for anything.
    return Status::failure(ErrorCode::kMissingRequiredField, "enumeration generation 0 means never observed");
  }
  if (coverage == CoverageState::kNotEnumerated) {
    // "Not enumerated" is the absence of the observation this record claims to
    // be, so recording it as evidence is meaningless. A domain that could not
    // enumerate reports failed or partial instead, which are statements.
    return Status::failure(ErrorCode::kInvalidEnumValue,
                           "coverage 'not-enumerated' is not evidence; report failed or partial instead");
  }
  if (to_token(coverage).empty()) {
    std::string detail = "unknown enumeration coverage ";
    detail += format_strong(static_cast<std::uint64_t>(coverage));
    return Status::failure(ErrorCode::kInvalidEnumValue, detail);
  }
  if (manifest_digest.is_zero()) {
    // A zero digest is never produced by hashing: it means no manifest was
    // recorded, so there is nothing to compare a later enumeration against.
    return Status::failure(ErrorCode::kMissingRequiredField, "enumeration manifest digest is not set");
  }
  if (scope_manifest_digest.is_zero()) {
    return Status::failure(ErrorCode::kMissingRequiredField, "enumeration scope manifest digest is not set");
  }
  if (!generations.is_complete()) {
    // A default generation anywhere in the set means the enumeration was not
    // taken against a fully known world, so it cannot be reconciled with a plan
    // binding that names real generations.
    return Status::failure(ErrorCode::kMissingRequiredField, "enumeration generation set is incomplete");
  }
  if (observed_at.value() == 0) {
    // The observation sequence is the ordering key evidence is selected by, so
    // an unset one cannot be placed in that order.
    return Status::failure(ErrorCode::kMissingRequiredField, "enumeration observation sequence is 0");
  }
  const Status source_status = detail::validate_text(source, "source", limits.max_text_bytes);
  if (!source_status.ok()) {
    return source_status;
  }
  return detail::validate_text(annotation, "annotation", limits.max_annotation_bytes);
}

std::string EnumerationEvidence::to_canonical() const {
  std::string text = "domain=";
  text += to_token(domain);
  text += " gen=";
  text += to_string(generation);
  text += " coverage=";
  text += to_token(coverage);
  text += " obs=";
  text += to_string(observed_at);
  text += " manifest=";
  text += manifest_digest.to_hex();
  text += " scope=";
  text += scope_manifest_digest.to_hex();
  text += " source=";
  text += source;
  return text;
}

Status CompletionEvidence::validate(const Limits& limits) const {
  // Fixed order: identity, state, generation, the digests, the generation set,
  // the observation sequence, the residual count agreement, then the text.
  if (id.value() == 0) {
    return Status::failure(ErrorCode::kInvalidIdentity, "completion evidence id 0 is not an identity");
  }
  if (state == CompletionState::kUnknown || to_token(state).empty()) {
    // "Unknown" is the absence of a report, not a report, and an out of range
    // state names nothing at all. Both are refused as enum values.
    return Status::failure(ErrorCode::kInvalidEnumValue, "completion state names no reported outcome");
  }
  if (generation.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "completion generation 0 means never observed");
  }
  if (payload_digest.is_zero()) {
    return Status::failure(ErrorCode::kMissingRequiredField, "completion payload digest is not set");
  }
  if (manifest_digest.is_zero()) {
    return Status::failure(ErrorCode::kMissingRequiredField, "completion manifest digest is not set");
  }
  if (scope_manifest_digest.is_zero()) {
    return Status::failure(ErrorCode::kMissingRequiredField, "completion scope manifest digest is not set");
  }
  if (!generations.is_complete()) {
    return Status::failure(ErrorCode::kMissingRequiredField, "completion generation set is incomplete");
  }
  if (observed_at.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "completion observation sequence is 0");
  }
  if (!residual_count_known && residual_count != 0) {
    // A count that is present but declared unknown is two contradictory
    // statements about the same number.
    return Status::failure(ErrorCode::kConflictingField,
                           "completion reports an unknown residual count together with a count");
  }
  if (state == CompletionState::kDrainedWithResiduals && (!residual_count_known || residual_count == 0)) {
    // Claiming residuals while stating none, or refusing to state how many,
    // contradicts the claim: "drained with residuals" must name a positive
    // count, otherwise it is indistinguishable from a clean drain.
    return Status::failure(ErrorCode::kConflictingField,
                           "state 'drained-with-residuals' requires a known, non zero residual count");
  }
  const Status source_status = detail::validate_text(source, "source", limits.max_text_bytes);
  if (!source_status.ok()) {
    return source_status;
  }
  return detail::validate_text(annotation, "annotation", limits.max_annotation_bytes);
}

std::string CompletionEvidence::to_canonical() const {
  std::string text = "id=";
  text += to_string(id);
  text += " domain=";
  text += to_token(domain);
  text += " gen=";
  text += to_string(generation);
  text += " state=";
  text += to_token(state);
  text += " obs=";
  text += to_string(observed_at);
  text += " payload=";
  text += payload_digest.to_hex();
  text += " manifest=";
  text += manifest_digest.to_hex();
  text += " residuals=";
  // An unknown count is rendered as the word, never as zero: "nobody said" and
  // "none remain" are different claims and must not share a spelling.
  if (residual_count_known) {
    text += format_strong(residual_count);
  } else {
    text += "unknown";
  }
  return text;
}

ContentDigest enumeration_evidence_digest(std::span<const EnumerationEvidence> records) {
  // Hashed in the order given: this digest names an exact sequence of evidence,
  // and the caller is responsible for selecting and ordering it canonically.
  std::vector<ContentDigest> digests;
  digests.reserve(records.size());
  for (const EnumerationEvidence& record : records) {
    digests.push_back(digest_text(record.to_canonical()));
  }
  return combine_digests("enumeration-evidence", digests);
}

ContentDigest completion_evidence_digest(std::span<const CompletionEvidence> records) {
  std::vector<ContentDigest> digests;
  digests.reserve(records.size());
  for (const CompletionEvidence& record : records) {
    digests.push_back(digest_text(record.to_canonical()));
  }
  return combine_digests("completion-evidence", digests);
}

}  // namespace facilitydrain
