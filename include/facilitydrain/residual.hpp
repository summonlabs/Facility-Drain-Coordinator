// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_RESIDUAL_HPP
#define FACILITYDRAIN_RESIDUAL_HPP

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/identity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Residual ledger
// ---------------------------------------------------------------------------
//
// The residual ledger is the exact list of what is not resolved, including the
// things nobody could measure. It is the reason a drain cannot be declared
// complete on the strength of an empty list: an empty ledger that exists only
// because enumeration was partial, or because a count was unknown, is recorded
// as an unknown entry rather than as nothing.

enum class ResidualKind : std::uint8_t {
  /// The owner still reports the obligation as present.
  kObligationActive = 1,
  /// The owner cannot determine whether the obligation is present.
  kObligationUnknown = 2,
  /// The owner declined to relinquish the obligation.
  kOwnerRefused = 3,
  /// Enumeration coverage for this domain was partial or failed.
  kEnumerationIncomplete = 4,
  /// A required domain has produced no completion evidence at all.
  kEvidenceMissing = 5,
  /// The owner reports drained but did not state how many obligations remain.
  kResidualCountUnknown = 6,
  /// A bounded request was issued but has no acknowledgement.
  kRequestUnacknowledged = 7,
  /// The owning system reported a failure for this domain.
  kDomainFailed = 8,
  /// The selected evidence predates the plan's fence floor, so it cannot
  /// support a new verdict.
  kEvidenceStale = 9,
  /// A protected obligation may not be relinquished under the current policy.
  kProtectedObligation = 10,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(ResidualKind kind) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<ResidualKind> residual_kind_from_token(std::string_view token) noexcept;
/// True for kinds that mean the answer is unknown rather than known bad. These
/// are counted separately and always block a safe to remove verdict.
[[nodiscard]] FACILITYDRAIN_API bool is_unknown_residual_kind(ResidualKind kind) noexcept;

enum class ResidualState : std::uint8_t {
  /// Unresolved. Blocks the domain it belongs to.
  kOpen = 1,
  /// Resolved with evidence: the owning system confirmed the obligation is gone.
  kRelinquished = 2,
  /// No longer applicable because the plan was revised or the scope changed.
  /// Recorded for the audit trail but never counted as proof of completion.
  kSuperseded = 3,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(ResidualState state) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<ResidualState> residual_state_from_token(std::string_view token) noexcept;

struct ResidualEntry {
  /// The obligation this entry is about. Zero means the entry is about the
  /// domain's coverage or evidence as a whole rather than one obligation.
  ObligationId obligation{};
  OwnerDomain domain = OwnerDomain::kAsi;
  ResidualKind kind = ResidualKind::kObligationActive;
  ResidualState state = ResidualState::kOpen;
  ObligationGeneration generation{};
  /// The evidence generation that resolved this entry. Zero while open.
  EvidenceGeneration resolution_evidence_generation{};
  ObservationSequence recorded_at{};
  ObservationSequence resolved_at{};
  ContentDigest detail_digest{};
  std::string detail{};

  /// Canonical identity of the entry inside a ledger.
  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
  [[nodiscard]] bool operator<(const ResidualEntry& other) const noexcept;
  [[nodiscard]] bool operator==(const ResidualEntry& other) const noexcept;
};

/// The residual ledger of one plan, in canonical order.
struct ResidualLedger {
  std::vector<ResidualEntry> entries{};

  [[nodiscard]] FACILITYDRAIN_API std::uint32_t open_count() const noexcept;
  [[nodiscard]] FACILITYDRAIN_API std::uint32_t open_count(OwnerDomain domain) const noexcept;
  [[nodiscard]] FACILITYDRAIN_API std::uint32_t unknown_count() const noexcept;
  [[nodiscard]] FACILITYDRAIN_API std::uint32_t unknown_count(OwnerDomain domain) const noexcept;
  /// Distinct non zero obligations recorded for a domain, in any state.
  [[nodiscard]] FACILITYDRAIN_API std::uint32_t known_obligation_count(OwnerDomain domain) const noexcept;
  /// Obligations for a domain that reached the relinquished state.
  [[nodiscard]] FACILITYDRAIN_API std::uint32_t relinquished_obligation_count(OwnerDomain domain) const noexcept;
  [[nodiscard]] FACILITYDRAIN_API bool has_open(OwnerDomain domain) const noexcept;
  [[nodiscard]] FACILITYDRAIN_API bool empty() const noexcept { return entries.empty(); }
};

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_RESIDUAL_HPP
