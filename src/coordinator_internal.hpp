// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SRC_COORDINATOR_INTERNAL_HPP
#define FACILITYDRAIN_SRC_COORDINATOR_INTERNAL_HPP

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/snapshot.hpp"
#include "facilitydrain/version.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace facilitydrain {
namespace detail {

// ---------------------------------------------------------------------------
// The durable state
// ---------------------------------------------------------------------------
//
// The durable state is a list of recorded facts. Nothing in it is a cached
// conclusion: the lifecycle state, the required domains, the verdict and the
// evidence digests are all recomputed from these facts by the documented rules
// whenever they are asked for. That is why a restart cannot resurrect a
// conclusion that the facts no longer support.

/// One enumeration the owner reported, with the consumer records it carried.
struct EnumerationRecord {
  EnumerationEvidence evidence{};
  std::vector<ConsumerRecord> consumers{};
};

/// Everything recorded about one drain plan.
struct PlanRecord {
  DrainPlanSpec spec{};
  /// The manifest known when the plan was created, per domain.
  std::array<std::vector<ConsumerRecord>, kOwnerDomainCount> planning_consumers{};
  /// Recorded enumerations per domain, oldest first. A record is only appended
  /// when its evidence generation strictly advances the domain's newest, so the
  /// newest record is always the last element.
  std::array<std::vector<EnumerationRecord>, kOwnerDomainCount> enumerations{};
  /// Recorded completion reports per domain, oldest first, same monotonicity.
  std::array<std::vector<CompletionEvidence>, kOwnerDomainCount> completions{};
  ResidualLedger residuals{};
  std::vector<DrainRequest> requests{};
  std::optional<SafeToRemoveGrant> grant{};
  std::optional<FenceRecord> fence{};
  std::vector<PlanHistoryEntry> history{};
  bool cancelled = false;
  std::string cancellation_detail{};
  bool failed = false;
  std::string failure_detail{};
  ObservationSequence last_observation{};
  CommitSequence last_commit{};
  Revision revision{};
};

/// The whole durable state of one store.
struct CoordinatorState {
  std::uint32_t payload_version = kStateFormatVersion;
  Limits limits{};
  ControlEpoch control_epoch{};
  IncarnationId incarnation{};
  CommitSequence commit_sequence{};
  ObservationSequence observation_sequence{};
  std::int64_t created_at_milliseconds = 0;
  std::int64_t updated_at_milliseconds = 0;
  /// In canonical plan id order; the encoder relies on that order.
  std::vector<PlanRecord> plans{};
};

// ---------------------------------------------------------------------------
// Derived facts
// ---------------------------------------------------------------------------
//
// Every function below is pure: it reads recorded facts and returns a value.
// None of them mutates anything, which is what keeps a query from changing the
// answer it is asking about.

[[nodiscard]] const PlanRecord* find_plan(const CoordinatorState& state, PlanId id) noexcept;
[[nodiscard]] PlanRecord* find_plan(CoordinatorState& state, PlanId id) noexcept;

/// The newest enumeration recorded for a domain, or nullptr.
[[nodiscard]] const EnumerationRecord* newest_enumeration(const PlanRecord& plan, OwnerDomain domain) noexcept;
/// The newest completion report recorded for a domain, or nullptr.
[[nodiscard]] const CompletionEvidence* newest_completion(const PlanRecord& plan, OwnerDomain domain) noexcept;

/// The manifest of record for a domain: the consumers of the newest enumeration
/// whose digest the plan has bound, falling back to the planning time manifest
/// when no enumeration has been accepted. Never empty by accident: an accepted
/// empty enumeration is a real, proven empty manifest.
[[nodiscard]] const std::vector<ConsumerRecord>& manifest_of_record(const PlanRecord& plan, OwnerDomain domain);

/// True when the newest enumeration for the domain matches the plan's bound
/// manifest digest, and therefore counts as the plan's manifest of record.
[[nodiscard]] bool enumeration_is_accepted(const PlanRecord& plan, OwnerDomain domain) noexcept;

/// The domains that must be proven: the declared requirement widened by every
/// domain that owns a mandatory obligation in its manifest of record.
[[nodiscard]] DomainMask required_domains_for_plan(const PlanRecord& plan);

/// The lifecycle state derived from the recorded facts by the fixed rule
/// documented in plan.hpp. First match wins.
[[nodiscard]] DrainState derive_state(const PlanRecord& plan, ControlEpoch current_epoch) noexcept;

/// The same derivation over a snapshot that has already been built. The
/// snapshot builder calls this after filling every fact, which is what keeps
/// the state a conclusion about the facts rather than a field somebody set.
[[nodiscard]] DrainState derive_state_from_snapshot(const DrainPlanSnapshot& plan,
                                                    ControlEpoch current_epoch) noexcept;

/// True when the plan holds a residual that withholds removal authority: an
/// open entry in a required domain, or an open entry of an unknown kind in any
/// domain. An open entry in a domain the plan does not require is reported but
/// does not withhold authority the operator explicitly declared unnecessary.
[[nodiscard]] bool has_blocking_residuals(const DrainPlanSnapshot& plan) noexcept;

/// Whether an enumeration agrees with the plan's binding and fence floor.
/// kOk means it is accepted as the plan's manifest of record.
[[nodiscard]] ErrorCode enumeration_acceptance(const PlanRecord& plan,
                                               const EnumerationEvidence& evidence) noexcept;

/// Whether a completion report agrees with the plan's generation set, its
/// bound manifests and its fence floor. kOk means it is compatible. Note that
/// compatible is not the same as complete: this says the report is about the
/// right world, not that it proves the drain.
[[nodiscard]] ErrorCode completion_compatibility(const PlanRecord& plan,
                                                 const CompletionEvidence& evidence) noexcept;

/// True when a grant exists and still binds the current revision, the current
/// control epoch, and a commit at or after the last fence.
[[nodiscard]] bool grant_is_live(const PlanRecord& plan, ControlEpoch current_epoch) noexcept;

/// The digest of a plan's canonical encoding. Stable across processes.
[[nodiscard]] ContentDigest plan_digest(const PlanRecord& plan, ControlEpoch current_epoch);
/// The digest of the whole canonical state.
[[nodiscard]] ContentDigest coordinator_digest(const CoordinatorState& state);

/// The verdict over a built snapshot. The public pure entry point
/// facilitydrain::evaluate_safe_to_remove forwards to this, so an operator
/// asking for an explanation and the coordinator deciding whether to grant are
/// running exactly the same rules.
[[nodiscard]] SafeToRemoveEvaluation evaluate_safe_to_remove(const DrainPlanSnapshot& plan,
                                                             ControlEpoch current_epoch);

/// Builds the read only view of one plan, with every derived field filled in.
[[nodiscard]] DrainPlanSnapshot build_plan_snapshot(const PlanRecord& plan, ControlEpoch current_epoch);
[[nodiscard]] CoordinatorSnapshot build_snapshot(const CoordinatorState& state, bool durable,
                                                 const std::string& store_root);

/// The digest of the obligations a request for this domain would ask to drain:
/// the canonical manifest digest of the domain's manifest of record.
[[nodiscard]] ContentDigest obligation_digest_for(const PlanRecord& plan, OwnerDomain domain);

/// A deterministic, dependency free instruction text for a bounded request.
[[nodiscard]] std::string instruction_for(OwnerDomain domain, const DrainScope& scope, std::uint32_t bound,
                                          std::size_t obligation_count);

/// The canonical target system token for a domain: the owning system a request
/// is addressed to.
[[nodiscard]] std::string target_system_for(OwnerDomain domain);

/// Appends a history entry when the derived state changed. Returns true when an
/// entry was appended. Trims the history to the configured bound, oldest first.
bool record_history(PlanRecord& plan, DrainState previous, DrainState current, PlanOperation cause,
                    ObservationSequence observation, CommitSequence commit, std::int64_t clock_milliseconds,
                    const Limits& limits, std::string detail);

/// Moves the plan's fence forward. The floor only ever moves forward, so an
/// older fence can never re-open a newer answer.
void apply_fence(PlanRecord& plan, FenceReason reason, ObservationSequence floor, Revision revision,
                 ControlEpoch epoch, CommitSequence commit, std::int64_t clock_milliseconds,
                 std::string detail);

/// Fences a live grant, if there is one, and clears it. Returns the fence that
/// was recorded, or nullopt when nothing was live.
[[nodiscard]] std::optional<FenceRecord> fence_live_grant(PlanRecord& plan, FenceReason reason,
                                                          ObservationSequence floor, ControlEpoch epoch,
                                                          CommitSequence commit,
                                                          std::int64_t clock_milliseconds, std::string detail);

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_SRC_COORDINATOR_INTERNAL_HPP
