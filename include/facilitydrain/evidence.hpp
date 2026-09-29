// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_EVIDENCE_HPP
#define FACILITYDRAIN_EVIDENCE_HPP

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------
//
// Everything the coordinator knows about the world arrives as evidence stamped
// by the system that owns the effect. The coordinator never infers an effect
// from its own request, from an acknowledgement, or from the absence of a
// complaint. Evidence that is missing stays missing, evidence that is unknown
// stays unknown, and neither is ever converted into zero, healthy or safe.

/// How much of the physical scope an enumeration actually covered.
enum class CoverageState : std::uint8_t {
  kNotEnumerated = 1,
  /// The owner saw part of the scope. A partial enumeration is never proof of
  /// absence: zero consumers found under partial coverage is not zero consumers.
  kPartial = 2,
  /// The owner attests the enumeration covers the whole scope.
  kComplete = 3,
  kFailed = 4,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(CoverageState state) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<CoverageState> coverage_state_from_token(std::string_view token) noexcept;

/// One owner's statement about which consumers currently depend on the scope.
struct EnumerationEvidence {
  OwnerDomain domain = OwnerDomain::kAsi;
  CoverageState coverage = CoverageState::kNotEnumerated;
  /// Monotone per (plan, domain). A record that does not advance the previous
  /// generation is stale and is rejected rather than adopted.
  EvidenceGeneration generation{};
  ObservationSequence observed_at{};
  /// Digest of the consumer records the owner reported for this domain.
  ContentDigest manifest_digest{};
  /// Digest of the physical membership the owner enumerated against.
  ContentDigest scope_manifest_digest{};
  /// The generations the owner enumerated under.
  GenerationSet generations{};
  std::string source{};
  std::string annotation{};
  std::int64_t observed_at_milliseconds = 0;

  [[nodiscard]] FACILITYDRAIN_API Status validate(const Limits& limits) const;
  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
};

/// What the owning system reports about the drain itself.
enum class CompletionState : std::uint8_t {
  kUnknown = 1,
  /// The owner received the bounded request and has not started.
  kRequested = 2,
  /// The owner acknowledges the request. An acknowledgement is a received
  /// message, not an effect, and never completes a domain.
  kAcknowledged = 3,
  /// The owner is actively draining and has not finished.
  kDraining = 4,
  /// The owner drained everything it could and reports that unresolved
  /// obligations remain.
  kDrainedWithResiduals = 5,
  /// The owner reports the domain drained.
  kDrained = 6,
  /// The owner refused the request. A refusal is a decision, not a failure.
  kRefused = 7,
  kFailed = 8,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(CompletionState state) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<CompletionState> completion_state_from_token(
    std::string_view token) noexcept;
/// True only for the states in which the owner claims the drain itself is done.
[[nodiscard]] FACILITYDRAIN_API bool is_terminal_completion(CompletionState state) noexcept;

/// One owner's statement about the outcome of a drain over the whole scope.
struct CompletionEvidence {
  EvidenceId id{};
  OwnerDomain domain = OwnerDomain::kAsi;
  CompletionState state = CompletionState::kUnknown;
  /// Monotone per (plan, domain); a non advancing generation is stale.
  EvidenceGeneration generation{};
  ObservationSequence observed_at{};
  /// Digest of the owner's report payload. The payload itself is not retained:
  /// the coordinator keeps the digest, not a copy of another system's data.
  ContentDigest payload_digest{};
  /// The consumer manifest the owner acted on. It must equal the enumeration
  /// the plan adopted, otherwise the owner drained a different set.
  ContentDigest manifest_digest{};
  /// The physical membership the owner drained.
  ContentDigest scope_manifest_digest{};
  /// The generations the report was produced under.
  GenerationSet generations{};
  /// Whether the owner stated how many obligations remain. An absent count is
  /// unknown, which is not zero, and blocks a safe to remove verdict.
  bool residual_count_known = false;
  std::uint64_t residual_count = 0;
  std::string source{};
  std::string annotation{};
  std::int64_t observed_at_milliseconds = 0;

  [[nodiscard]] FACILITYDRAIN_API Status validate(const Limits& limits) const;
  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
};

/// Domain separated digest over an ordered set of enumeration records. Used by
/// the plan binding and by the durable state digest.
[[nodiscard]] FACILITYDRAIN_API ContentDigest enumeration_evidence_digest(
    std::span<const EnumerationEvidence> records);

/// Domain separated digest over an ordered set of completion records. This is
/// the digest a safe to remove grant binds to, so a grant names exactly the
/// evidence it was evaluated against.
[[nodiscard]] FACILITYDRAIN_API ContentDigest completion_evidence_digest(
    std::span<const CompletionEvidence> records);

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_EVIDENCE_HPP
