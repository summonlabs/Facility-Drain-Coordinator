// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_PLAN_HPP
#define FACILITYDRAIN_PLAN_HPP

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/scope.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Plan lifecycle
// ---------------------------------------------------------------------------
//
// The lifecycle state of a plan is derived from the recorded facts, never
// assigned by a caller. There is deliberately no set_state operation: a caller
// that could write "SafeToRemove" would be able to authorise a removal by
// assertion, which is exactly the failure this system exists to prevent.
//
// The derivation is a fixed first-match-wins sequence over the plan's recorded
// facts, in this order:
//
//   1. the plan is cancelled                      -> Cancelled
//   2. the plan is marked failed                  -> Failed
//   3. a live safe to remove grant exists         -> SafeToRemove
//   4. every required domain is proven complete
//      and no residual entry is open              -> Drained
//   5. any residual entry is open                 -> ResidualsPresent
//   6. any request is acknowledged or completed   -> Draining
//   7. any request is issued                      -> Requested
//   8. any enumeration evidence is recorded       -> Enumerating
//   9. otherwise                                  -> Proposed

enum class DrainState : std::uint8_t {
  kProposed = 1,
  kEnumerating = 2,
  kRequested = 3,
  kDraining = 4,
  kResidualsPresent = 5,
  kDrained = 6,
  kSafeToRemove = 7,
  kCancelled = 8,
  kFailed = 9,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(DrainState state) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<DrainState> drain_state_from_token(std::string_view token) noexcept;
/// Cancelled and Failed: recorded facts that no further operation may move the
/// plan out of, apart from a revision that creates the next attempt.
[[nodiscard]] FACILITYDRAIN_API bool is_terminal_drain_state(DrainState state) noexcept;
/// SafeToRemove is the only state that carries removal authority.
[[nodiscard]] FACILITYDRAIN_API bool is_authority_drain_state(DrainState state) noexcept;

/// The operations whose admissibility depends on the derived state.
enum class PlanOperation : std::uint8_t {
  kRevise = 1,
  kRecordEnumeration = 2,
  kIssueRequests = 3,
  kRecordAcknowledgement = 4,
  kIngestCompletion = 5,
  kRecordResidual = 6,
  kResolveResidual = 7,
  kEvaluate = 8,
  kGrant = 9,
  kFence = 10,
  kCancel = 11,
  kFail = 12,
  kSupersedeRequest = 13,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(PlanOperation operation) noexcept;

/// Whether the operation is admissible in this derived state. Evaluation and
/// reporting are always admissible; a cancelled or failed plan admits only a
/// revision, an evaluation that will deny, and its own terminal report.
[[nodiscard]] FACILITYDRAIN_API Status check_plan_operation(DrainState state, PlanOperation operation);

/// Why a previously live safe to remove answer stopped being live. A fenced
/// answer is never silently reused: the fence records a floor, and only
/// evidence observed strictly after that floor can support a new grant.
enum class FenceReason : std::uint8_t {
  kNone = 0,
  kRestart = 1,
  kControlEpochChanged = 2,
  kNewObligation = 3,
  kPlanRevised = 4,
  kEvidenceSuperseded = 5,
  kScopeManifestChanged = 6,
  kOperatorFence = 7,
  kPlanCancelled = 8,
  kPlanFailed = 9,
  kDependencyChange = 10,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(FenceReason reason) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<FenceReason> fence_reason_from_token(std::string_view token) noexcept;

struct FenceRecord {
  FenceReason reason = FenceReason::kNone;
  /// Evidence observed at or before this sequence cannot support a grant. The
  /// floor only ever moves forward.
  ObservationSequence floor{};
  /// The revision whose answer was fenced.
  Revision revision{};
  /// The control epoch under which the fence was recorded.
  ControlEpoch epoch{};
  /// The durable position of the fence.
  CommitSequence commit{};
  std::int64_t recorded_at_milliseconds = 0;
  std::string detail{};
};

/// Everything a plan was planned against. Every later mutation is checked
/// against these values, and evidence that does not agree with them is
/// rejected rather than reconciled.
struct DrainPlanBindings {
  /// The facility control epoch the plan was created under. Provenance only:
  /// it does not authorise anything after a restart.
  ControlEpoch facility_epoch{};
  /// The generation set the plan was planned against.
  GenerationSet generations{};
  PolicyId policy_id{};
  /// Digest of the policy document that decided which obligations are
  /// protected and which domains must be proven.
  ContentDigest policy_digest{};
  /// The consumer manifest digest bound at planning time for each domain. An
  /// empty entry means the digest was not known when the plan was created; the
  /// first complete enumeration from that domain binds it, and any later
  /// disagreement requires an explicit revision.
  std::array<std::optional<ContentDigest>, kOwnerDomainCount> domain_manifest_digests{};

  [[nodiscard]] const std::optional<ContentDigest>& manifest_digest(OwnerDomain domain) const noexcept {
    return domain_manifest_digests[domain_index(domain)];
  }
  void set_manifest_digest(OwnerDomain domain, ContentDigest digest) noexcept {
    domain_manifest_digests[domain_index(domain)] = digest;
  }
};

/// The immutable description of one drain attempt.
struct DrainPlanSpec {
  PlanId id{};
  /// The physical scope being drained. Always a member of the manifest.
  DrainScope scope{};
  /// The physical membership the plan covers.
  DrainTargetManifest targets{};
  /// What the plan was planned against.
  DrainPlanBindings bindings{};
  /// Starts at 1 and advances by exactly one per accepted revision.
  Revision revision{};
  /// The domains that must be proven complete. Must be non empty: a plan that
  /// requires nothing proven can never authorise anything.
  DomainMask declared_required_domains{};
  std::string label{};
  std::string requested_by{};
  std::int64_t created_at_milliseconds = 0;
};

/// One recorded change of derived state, for the plan's audit trail.
struct PlanHistoryEntry {
  DrainState from_state = DrainState::kProposed;
  DrainState to_state = DrainState::kProposed;
  PlanOperation cause = PlanOperation::kRevise;
  ObservationSequence observed_at{};
  CommitSequence commit{};
  std::int64_t recorded_at_milliseconds = 0;
  std::string detail{};
};

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_PLAN_HPP
