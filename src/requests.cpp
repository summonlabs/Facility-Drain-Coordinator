// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/requests.hpp"
#include "utf8.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace facilitydrain {

std::string_view to_token(RequestState state) noexcept {
  switch (state) {
    case RequestState::kStaged:
      return "staged";
    case RequestState::kIssued:
      return "issued";
    case RequestState::kAcknowledged:
      return "acknowledged";
    case RequestState::kCompleted:
      return "completed";
    case RequestState::kRefused:
      return "refused";
    case RequestState::kFailed:
      return "failed";
    case RequestState::kSuperseded:
      return "superseded";
    case RequestState::kCancelled:
      return "cancelled";
  }
  return std::string_view{};
}

std::optional<RequestState> request_state_from_token(std::string_view token) noexcept {
  if (token == "staged") {
    return RequestState::kStaged;
  }
  if (token == "issued") {
    return RequestState::kIssued;
  }
  if (token == "acknowledged") {
    return RequestState::kAcknowledged;
  }
  if (token == "completed") {
    return RequestState::kCompleted;
  }
  if (token == "refused") {
    return RequestState::kRefused;
  }
  if (token == "failed") {
    return RequestState::kFailed;
  }
  if (token == "superseded") {
    return RequestState::kSuperseded;
  }
  if (token == "cancelled") {
    return RequestState::kCancelled;
  }
  return std::nullopt;
}

bool is_request_acknowledged(RequestState state) noexcept {
  // Confirmed receipt. None of these is an effect, so none of them completes a
  // domain: a refusal and a failure are answers, not drains.
  switch (state) {
    case RequestState::kAcknowledged:
    case RequestState::kCompleted:
    case RequestState::kRefused:
    case RequestState::kFailed:
      return true;
    case RequestState::kStaged:
    case RequestState::kIssued:
    case RequestState::kSuperseded:
    case RequestState::kCancelled:
      return false;
  }
  return false;
}

bool is_request_outstanding(RequestState state) noexcept {
  // The record still expects something to happen to it. A restart must re-offer
  // exactly these records rather than forget them, and because the idempotency
  // key is unchanged the owner sees a replay rather than a second drain.
  return state == RequestState::kStaged || state == RequestState::kIssued;
}

bool is_request_settled(RequestState state) noexcept {
  // No further answer is expected. Note that acknowledged and issued are not
  // settled: something is still owed, even though a message was exchanged.
  switch (state) {
    case RequestState::kCompleted:
    case RequestState::kRefused:
    case RequestState::kFailed:
    case RequestState::kSuperseded:
    case RequestState::kCancelled:
      return true;
    case RequestState::kStaged:
    case RequestState::kIssued:
    case RequestState::kAcknowledged:
      return false;
  }
  return false;
}

ContentDigest request_idempotency_key(const DrainRequestKey& key) noexcept {
  // The plan revision is deliberately absent from this text. A revision is
  // bookkeeping inside the coordinator, and including it would turn one
  // physical drain into two the moment anything else about the plan was
  // revised. Only an explicit supersede advances the attempt, and the attempt
  // is part of the key.
  std::string canonical = "plan=";
  canonical += to_string(key.plan);
  canonical += "|domain=";
  canonical += to_token(key.domain);
  canonical += "|scope=";
  canonical += format_scope(key.scope);
  canonical += "|obligations=";
  canonical += key.obligation_digest.to_hex();
  canonical += "|policy=";
  canonical += to_string(key.policy_generation);
  canonical += "|attempt=";
  canonical += to_string(key.attempt);
  const std::array<ContentDigest, 1> digests{digest_text(canonical)};
  return combine_digests("request-key", digests);
}

DrainRequestId request_id_for(const DrainRequestKey& key) noexcept {
  const ContentDigest idempotency_key = request_idempotency_key(key);
  const std::span<const std::byte, ContentDigest::kSize> bytes = idempotency_key.bytes();
  std::uint64_t raw = 0;
  for (std::size_t index = 0; index < sizeof(raw); ++index) {
    // Little endian: the first byte of the key is the least significant byte of
    // the handle, so the same key produces the same handle on every machine.
    raw |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
           << (8U * static_cast<unsigned int>(index));
  }
  // A request identity of 0 means "no request", so a key whose leading bytes are
  // all zero is folded onto 1 rather than producing an unusable handle.
  if (raw == 0) {
    raw = 1;
  }
  return DrainRequestId{raw};
}

Status DrainRequest::validate(const Limits& limits) const {
  // Fixed order: identity, key material, plan, obligations, attempt, scope,
  // state, bound, then the text. Validation stops at the first violation, so
  // the same record always reports the same primary code.
  if (id.value() == 0) {
    return Status::failure(ErrorCode::kInvalidIdentity, "request id 0 is not an identity");
  }
  if (idempotency_key.is_zero()) {
    // Without a key the request cannot be recognised as a replay, and a lost
    // acknowledgement would become a second physical drain.
    return Status::failure(ErrorCode::kMissingRequiredField, "request idempotency key is not set");
  }
  if (key.plan.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "request key names no plan");
  }
  if (key.obligation_digest.is_zero()) {
    // The request is about exactly one obligation set, and an unset digest
    // would make it a request about nothing in particular.
    return Status::failure(ErrorCode::kMissingRequiredField, "request key carries no obligation digest");
  }
  if (key.attempt.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "request key attempt 0: the first attempt is 1");
  }
  if (key.scope.id == 0) {
    return Status::failure(ErrorCode::kInvalidScope, "request key names scope 0, which does not exist");
  }
  if (to_token(state).empty()) {
    std::string detail = "unknown request state ";
    detail += format_strong(static_cast<std::uint64_t>(state));
    return Status::failure(ErrorCode::kInvalidEnumValue, detail);
  }
  if (bound_operations == 0) {
    // A request is bounded or it is not issued: an unbounded request would let
    // the owner act on obligations the coordinator never enumerated.
    return Status::failure(ErrorCode::kMissingRequiredField, "an unbounded request is not issued");
  }
  if (target_system.empty()) {
    return Status::failure(ErrorCode::kMissingRequiredField, "request names no target system");
  }
  const Status target_status = detail::validate_text(target_system, "target_system", limits.max_text_bytes);
  if (!target_status.ok()) {
    return target_status;
  }
  return detail::validate_text(instruction, "instruction", limits.max_annotation_bytes);
}

std::string DrainRequest::to_canonical() const {
  std::string text = "id=";
  text += to_string(id);
  text += " domain=";
  text += to_token(key.domain);
  text += " scope=";
  text += format_scope(key.scope);
  text += " state=";
  text += to_token(state);
  text += " attempt=";
  text += to_string(key.attempt);
  text += " bound=";
  text += format_strong(bound_operations);
  text += " key=";
  text += idempotency_key.to_hex();
  return text;
}

bool DrainRequest::operator<(const DrainRequest& other) const noexcept {
  // Requests are ordered by what the request is about before how it is doing:
  // the domain, the scope, the obligation set, and only then the attempt. That
  // way every attempt at one drain sorts next to the others.
  const std::uint32_t domain_key = domain_index(key.domain);
  const std::uint32_t other_domain_key = domain_index(other.key.domain);
  if (domain_key != other_domain_key) {
    return domain_key < other_domain_key;
  }
  if (key.scope != other.key.scope) {
    return key.scope < other.key.scope;
  }
  if (key.obligation_digest != other.key.obligation_digest) {
    return key.obligation_digest < other.key.obligation_digest;
  }
  return key.attempt < other.key.attempt;
}

}  // namespace facilitydrain
