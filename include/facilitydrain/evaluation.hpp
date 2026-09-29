// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_EVALUATION_HPP
#define FACILITYDRAIN_EVALUATION_HPP

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/plan.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Safe to remove evaluation
// ---------------------------------------------------------------------------
//
// The evaluation answers one question: for this physical scope, under this
// exact generation set, has every required domain proven that the obligations
// it owns are gone, with nothing unknown left behind? The answer is a verdict,
// not a state: it does not change anything, and it is recomputed from the
// durable facts every time it is asked.

enum class DomainVerdict : std::uint8_t {
  /// The domain proved the drain complete under the required generation set.
  kProvenComplete = 1,
  /// The domain produced something, but not enough to prove completion.
  kIncomplete = 2,
  /// The domain's answer cannot be established at all: unknown coverage, an
  /// absent count, or a count that names obligations nobody can identify.
  kUnknown = 3,
  /// The domain is not required by this plan.
  kNotRequired = 4,
  /// The owning system reported a failure.
  kFailed = 5,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(DomainVerdict verdict) noexcept;

/// One domain's contribution to the verdict, with the exact facts behind it.
struct DomainAssessment {
  OwnerDomain domain = OwnerDomain::kAsi;
  DomainVerdict verdict = DomainVerdict::kIncomplete;
  /// The single code that explains a non proven verdict. kOk when proven or not
  /// required. This is what the report and the CLI surface as the reason.
  ErrorCode blocking_code = ErrorCode::kOk;

  bool required = false;
  bool enumeration_present = false;
  bool enumeration_complete = false;
  bool enumeration_manifest_matches = false;
  bool enumeration_after_floor = false;
  bool completion_present = false;
  bool completion_compatible = false;
  bool completion_manifest_matches = false;
  bool completion_after_floor = false;
  bool residual_count_known = false;

  std::uint64_t residual_count = 0;
  std::uint32_t open_residuals = 0;
  std::uint32_t unknown_residuals = 0;
  std::uint32_t known_obligations = 0;
  std::uint32_t relinquished_obligations = 0;
  std::uint32_t mandatory_obligations = 0;

  EvidenceGeneration enumeration_generation{};
  EvidenceGeneration completion_generation{};
  GenerationSet evidence_generations{};
  std::string reason{};
};

enum class SafeToRemoveVerdict : std::uint8_t {
  kGranted = 1,
  kDenied = 2,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(SafeToRemoveVerdict verdict) noexcept;

struct SafeToRemoveEvaluation {
  PlanId plan{};
  Revision revision{};
  ControlEpoch epoch{};
  SafeToRemoveVerdict verdict = SafeToRemoveVerdict::kDenied;

  /// In canonical domain order, always all four entries.
  std::array<DomainAssessment, kOwnerDomainCount> domains{};

  /// Every distinct blocking code, in canonical domain order and then in
  /// evaluation order inside a domain. Never contains duplicates.
  std::vector<ErrorCode> blocking_codes{};
  /// The single code that explains the denial. Deterministic: the first
  /// blocking code in the canonical walk. kOk when granted.
  ErrorCode primary_blocking_code = ErrorCode::kOk;

  bool all_required_proven = false;
  bool enumeration_complete_for_required = false;
  bool fenced = false;
  ObservationSequence fence_floor{};

  std::uint32_t open_residuals = 0;
  std::uint32_t unknown_residuals = 0;
  std::uint32_t required_domain_count = 0;

  /// The generation set the verdict applies to.
  GenerationSet generations{};
  /// Digest over the exact evidence the verdict was computed from.
  ContentDigest evidence_digest{};
  /// Digest over the consumer manifests the verdict was computed against.
  ContentDigest manifest_digest{};
  CommitSequence evaluated_commit{};

  /// Deterministic, human readable explanation. Identical inputs produce
  /// identical text.
  std::string explanation{};

  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
};

/// A live grant of removal authority. It binds the plan revision, the control
/// epoch, the generation set and the evidence digest it was evaluated against,
/// so it stops being live the moment any of those moves.
struct DrainPlanSnapshot;

/// The pure evaluation over a snapshot of recorded facts.
///
/// This is the same function the coordinator uses: it reads the plan snapshot
/// and the current control epoch and returns the verdict, the per domain
/// assessment, the blocking codes and the explanation. It changes nothing, and
/// its result depends on nothing but its arguments, which is what makes the
/// decision rules directly testable.
[[nodiscard]] FACILITYDRAIN_API SafeToRemoveEvaluation evaluate_safe_to_remove(
    const DrainPlanSnapshot& plan, ControlEpoch current_epoch);

struct SafeToRemoveGrant {
  PlanId plan{};
  Revision revision{};
  ControlEpoch epoch{};
  GenerationSet generations{};
  ContentDigest evidence_digest{};
  ContentDigest manifest_digest{};
  ObservationSequence observation_floor{};
  CommitSequence granted_commit{};
  std::int64_t granted_at_milliseconds = 0;
  std::string granted_by{};
};

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_EVALUATION_HPP
