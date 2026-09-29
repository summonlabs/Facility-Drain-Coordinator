// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/errors.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace facilitydrain {
namespace {

struct TokenEntry {
  ErrorCode code;
  std::string_view token;
};

// The single source of truth for both directions of the code <-> token
// mapping. Generation is deliberate rather than clever: the tokens are the
// enumerator name with the leading 'k' removed, lower cased, and a '-' between
// words, written out by hand exactly once so that a reader can check a
// spelling without running code. Because to_token() and
// error_code_from_token() both consult this table, they cannot drift apart.
constexpr auto kErrorTokens = std::array{
    TokenEntry{ErrorCode::kOk, "ok"},
    TokenEntry{ErrorCode::kInvalidArgument, "invalid-argument"},
    TokenEntry{ErrorCode::kMissingRequiredField, "missing-required-field"},
    TokenEntry{ErrorCode::kConflictingField, "conflicting-field"},
    TokenEntry{ErrorCode::kFieldTooLong, "field-too-long"},
    TokenEntry{ErrorCode::kInvalidText, "invalid-text"},
    TokenEntry{ErrorCode::kInvalidIdentity, "invalid-identity"},
    TokenEntry{ErrorCode::kInvalidScope, "invalid-scope"},
    TokenEntry{ErrorCode::kInvalidDigest, "invalid-digest"},
    TokenEntry{ErrorCode::kInvalidEnumValue, "invalid-enum-value"},
    TokenEntry{ErrorCode::kReservedNotZero, "reserved-not-zero"},
    TokenEntry{ErrorCode::kDuplicateIdentifier, "duplicate-identifier"},
    TokenEntry{ErrorCode::kUnsupportedVersion, "unsupported-version"},
    TokenEntry{ErrorCode::kMalformedRecord, "malformed-record"},
    TokenEntry{ErrorCode::kInvalidGenerationOrder, "invalid-generation-order"},
    TokenEntry{ErrorCode::kLimitExceeded, "limit-exceeded"},
    TokenEntry{ErrorCode::kPayloadTooLarge, "payload-too-large"},
    TokenEntry{ErrorCode::kTooManyEntries, "too-many-entries"},
    TokenEntry{ErrorCode::kCounterExhausted, "counter-exhausted"},
    TokenEntry{ErrorCode::kPlanNotFound, "plan-not-found"},
    TokenEntry{ErrorCode::kRevisionConflict, "revision-conflict"},
    TokenEntry{ErrorCode::kEpochMismatch, "epoch-mismatch"},
    TokenEntry{ErrorCode::kStaleGeneration, "stale-generation"},
    TokenEntry{ErrorCode::kGenerationIncompatible, "generation-incompatible"},
    TokenEntry{ErrorCode::kConsumerDigestMismatch, "consumer-digest-mismatch"},
    TokenEntry{ErrorCode::kScopeManifestMismatch, "scope-manifest-mismatch"},
    TokenEntry{ErrorCode::kStaleEvidence, "stale-evidence"},
    TokenEntry{ErrorCode::kAuthorityFenced, "authority-fenced"},
    TokenEntry{ErrorCode::kStaleAuthority, "stale-authority"},
    TokenEntry{ErrorCode::kUnknownResidualCount, "unknown-residual-count"},
    TokenEntry{ErrorCode::kIncompleteEnumeration, "incomplete-enumeration"},
    TokenEntry{ErrorCode::kEvidenceIncomplete, "evidence-incomplete"},
    TokenEntry{ErrorCode::kEvidenceMismatch, "evidence-mismatch"},
    TokenEntry{ErrorCode::kResidualsPresent, "residuals-present"},
    TokenEntry{ErrorCode::kUnknownObligation, "unknown-obligation"},
    TokenEntry{ErrorCode::kDomainFailed, "domain-failed"},
    TokenEntry{ErrorCode::kAcknowledgementIsNotEffect, "acknowledgement-is-not-effect"},
    TokenEntry{ErrorCode::kNotSafeToRemove, "not-safe-to-remove"},
    TokenEntry{ErrorCode::kObligationNotDeclared, "obligation-not-declared"},
    TokenEntry{ErrorCode::kProtectedObligation, "protected-obligation"},
    TokenEntry{ErrorCode::kInvalidStateTransition, "invalid-state-transition"},
    TokenEntry{ErrorCode::kPlanTerminal, "plan-terminal"},
    TokenEntry{ErrorCode::kPlanCancelled, "plan-cancelled"},
    TokenEntry{ErrorCode::kPlanFailed, "plan-failed"},
    TokenEntry{ErrorCode::kRequestNotFound, "request-not-found"},
    TokenEntry{ErrorCode::kRequestSuperseded, "request-superseded"},
    TokenEntry{ErrorCode::kDuplicateRequest, "duplicate-request"},
    TokenEntry{ErrorCode::kNotDrained, "not-drained"},
    TokenEntry{ErrorCode::kAlreadyIssued, "already-issued"},
    TokenEntry{ErrorCode::kStoreNotFound, "store-not-found"},
    TokenEntry{ErrorCode::kStoreLocked, "store-locked"},
    TokenEntry{ErrorCode::kStoreCorrupt, "store-corrupt"},
    TokenEntry{ErrorCode::kStoreChecksumMismatch, "store-checksum-mismatch"},
    TokenEntry{ErrorCode::kStoreTruncated, "store-truncated"},
    TokenEntry{ErrorCode::kStoreTrailingBytes, "store-trailing-bytes"},
    TokenEntry{ErrorCode::kStoreIoError, "store-io-error"},
    TokenEntry{ErrorCode::kStorePathInvalid, "store-path-invalid"},
    TokenEntry{ErrorCode::kStoreExists, "store-exists"},
    TokenEntry{ErrorCode::kStoreVersionUnsupported, "store-version-unsupported"},
    TokenEntry{ErrorCode::kStoreRecoveryFailed, "store-recovery-failed"},
    TokenEntry{ErrorCode::kCommitFailed, "commit-failed"},
    TokenEntry{ErrorCode::kReadOnlyStore, "read-only-store"},
    TokenEntry{ErrorCode::kMissingGenerationFile, "missing-generation-file"},
    TokenEntry{ErrorCode::kInternal, "internal"},
};

// Not a table entry on purpose: "unknown" must not round-trip through
// error_code_from_token() into a real code, because a value that is not in the
// enumeration has no canonical code identity at all.
constexpr std::string_view kUnknownToken = "unknown";

[[nodiscard]] constexpr std::uint32_t numeric(ErrorCode code) noexcept {
  return static_cast<std::uint32_t>(code);
}

[[nodiscard]] constexpr bool in_range(ErrorCode code, std::uint32_t low,
                                      std::uint32_t high) noexcept {
  return numeric(code) >= low && numeric(code) <= high;
}

}  // namespace

std::string_view to_token(ErrorCode code) noexcept {
  for (const TokenEntry& entry : kErrorTokens) {
    if (entry.code == code) {
      return entry.token;
    }
  }
  return kUnknownToken;
}

std::optional<ErrorCode> error_code_from_token(std::string_view token) noexcept {
  // Exact, case sensitive comparison: a token that differs in any byte,
  // including by case or trailing whitespace, is not a code.
  for (const TokenEntry& entry : kErrorTokens) {
    if (entry.token == token) {
      return entry.code;
    }
  }
  return std::nullopt;
}

bool is_validation_error(ErrorCode code) noexcept { return in_range(code, 1U, 99U); }

bool is_limit_error(ErrorCode code) noexcept { return in_range(code, 100U, 199U); }

bool is_authority_error(ErrorCode code) noexcept { return in_range(code, 200U, 299U); }

bool is_lifecycle_error(ErrorCode code) noexcept { return in_range(code, 300U, 399U); }

bool is_store_error(ErrorCode code) noexcept { return in_range(code, 400U, 499U); }

Error::Error(ErrorCode code, std::string_view detail) : code_(code), detail_(detail) {}

std::string Error::to_text() const {
  // kOk is the absence of a failure, so it renders as the one token "ok" no
  // matter what diagnostic text happens to be attached to it.
  if (code_ == ErrorCode::kOk) {
    return std::string{"ok"};
  }
  const std::string_view token = to_token(code_);
  if (detail_.empty()) {
    return std::string{token};
  }
  std::string text;
  text.reserve(token.size() + 2U + detail_.size());
  text.append(token);
  text.append(": ");
  text.append(detail_);
  return text;
}

Error Error::make(ErrorCode code, std::string_view detail) { return Error{code, detail}; }

Status Status::failure(ErrorCode code, std::string_view detail) {
  return Status{Error{code, detail}};
}

}  // namespace facilitydrain
