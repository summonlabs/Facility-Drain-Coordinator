// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_ERRORS_HPP
#define FACILITYDRAIN_ERRORS_HPP

#include "facilitydrain/export.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Error model
// ---------------------------------------------------------------------------
//
// Every externally meaningful failure has exactly one code, and the code a
// given invalid request resolves to is deterministic: identical invalid input
// produces the identical primary code regardless of map ordering, thread
// scheduling, or unrelated state. Operations therefore validate in a fixed,
// documented precedence and stop at the first violation.
//
// Numeric ranges are part of the contract, because the classification helpers
// below are what callers use to decide whether a rejection is their fault, a
// limit, an authority problem, a lifecycle problem, or an environment problem.

enum class ErrorCode : std::uint32_t {
  kOk = 0,

  // Shape and validation: the request itself is not well formed. 1..99.
  kInvalidArgument = 1,
  kMissingRequiredField = 2,
  kConflictingField = 3,
  kFieldTooLong = 4,
  kInvalidText = 5,
  kInvalidIdentity = 6,
  kInvalidScope = 7,
  kInvalidDigest = 8,
  kInvalidEnumValue = 9,
  kReservedNotZero = 10,
  kDuplicateIdentifier = 11,
  kUnsupportedVersion = 12,
  kMalformedRecord = 13,
  kInvalidGenerationOrder = 14,

  // Bounds. Nothing unbounded is ever accepted. 100..199.
  kLimitExceeded = 100,
  kPayloadTooLarge = 101,
  kTooManyEntries = 102,
  kCounterExhausted = 103,

  // Authority, generations and evidence. 200..299.
  kPlanNotFound = 200,
  kRevisionConflict = 201,
  kEpochMismatch = 202,
  kStaleGeneration = 203,
  kGenerationIncompatible = 204,
  kConsumerDigestMismatch = 205,
  kScopeManifestMismatch = 206,
  kStaleEvidence = 207,
  kAuthorityFenced = 208,
  kStaleAuthority = 209,
  kUnknownResidualCount = 210,
  kIncompleteEnumeration = 211,
  kEvidenceIncomplete = 212,
  kEvidenceMismatch = 213,
  kResidualsPresent = 214,
  kUnknownObligation = 215,
  kDomainFailed = 216,
  kAcknowledgementIsNotEffect = 217,
  kNotSafeToRemove = 218,
  kObligationNotDeclared = 219,
  kProtectedObligation = 220,

  // Lifecycle and state. 300..399.
  kInvalidStateTransition = 300,
  kPlanTerminal = 301,
  kPlanCancelled = 302,
  kPlanFailed = 303,
  kRequestNotFound = 304,
  kRequestSuperseded = 305,
  kDuplicateRequest = 306,
  kNotDrained = 307,
  kAlreadyIssued = 308,

  // Durability and environment. 400..499.
  kStoreNotFound = 400,
  kStoreLocked = 401,
  kStoreCorrupt = 402,
  kStoreChecksumMismatch = 403,
  kStoreTruncated = 404,
  kStoreTrailingBytes = 405,
  kStoreIoError = 406,
  kStorePathInvalid = 407,
  kStoreExists = 408,
  kStoreVersionUnsupported = 409,
  kStoreRecoveryFailed = 410,
  kCommitFailed = 411,
  kReadOnlyStore = 412,
  kMissingGenerationFile = 413,

  // Internal invariant breach. 900..999.
  kInternal = 900,
};

/// The stable lowercase token for a code. This is the text form used by the
/// CLI, by the canonical report format, and by every test diagnostic.
[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(ErrorCode code) noexcept;

/// The inverse of to_token. Unknown text is an empty optional, never a default.
[[nodiscard]] FACILITYDRAIN_API std::optional<ErrorCode> error_code_from_token(std::string_view token) noexcept;

/// 1..99: the request is malformed, not the world.
[[nodiscard]] FACILITYDRAIN_API bool is_validation_error(ErrorCode code) noexcept;
/// 100..199: a configured bound was exceeded.
[[nodiscard]] FACILITYDRAIN_API bool is_limit_error(ErrorCode code) noexcept;
/// 200..299: identity, generation, epoch, authority or evidence disagreement.
[[nodiscard]] FACILITYDRAIN_API bool is_authority_error(ErrorCode code) noexcept;
/// 300..399: the plan or request is not in a state where this is meaningful.
[[nodiscard]] FACILITYDRAIN_API bool is_lifecycle_error(ErrorCode code) noexcept;
/// 400..499: durability, locking or filesystem.
[[nodiscard]] FACILITYDRAIN_API bool is_store_error(ErrorCode code) noexcept;

/// An error value: a code plus bounded human readable detail.
///
/// The detail text is diagnostic only. It never carries authority, is never
/// parsed by this library, and is not part of any canonical encoding.
class FACILITYDRAIN_API Error {
 public:
  Error() = default;
  // One text constructor only. A second overload taking std::string would make
  // Error{code, "literal"} ambiguous, and an ambiguous constructor in the error
  // path is a defect in the error path.
  Error(ErrorCode code, std::string_view detail);

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::kOk; }

  /// "token: detail", or "ok" when the code is kOk.
  [[nodiscard]] std::string to_text() const;

  [[nodiscard]] static Error make(ErrorCode code, std::string_view detail);

 private:
  ErrorCode code_ = ErrorCode::kOk;
  std::string detail_{};
};

/// The result of an operation that produces no value.
class FACILITYDRAIN_API Status {
 public:
  Status() = default;
  Status(Error error) noexcept : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] static Status success() noexcept { return Status{}; }
  [[nodiscard]] static Status failure(Error error) noexcept { return Status{std::move(error)}; }
  [[nodiscard]] static Status failure(ErrorCode code, std::string_view detail);

  [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] std::string to_text() const { return error_.to_text(); }

 private:
  Error error_{};
};

/// The result of an operation that produces a value.
///
/// A default constructed result holds an internal error and is therefore
/// failure shaped: there is no path by which a missing answer silently becomes
/// a value, a zero, or a healthy verdict.
template <class T>
class Result {
 public:
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)
  Result(T value) : value_(std::move(value)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] Error& error() & { return error_; }
  [[nodiscard]] const Error& error() const& { return error_; }

  [[nodiscard]] T& operator*() & { return *value_; }
  [[nodiscard]] const T& operator*() const& { return *value_; }
  [[nodiscard]] T* operator->() { return &*value_; }
  [[nodiscard]] const T* operator->() const { return &*value_; }

 private:
  std::optional<T> value_{};
  Error error_{};
};

/// Builds a failure result without repeating the template argument.
template <class T>
[[nodiscard]] Result<T> make_error(ErrorCode code, std::string_view detail) {
  return Result<T>{Error{code, detail}};
}

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_ERRORS_HPP
