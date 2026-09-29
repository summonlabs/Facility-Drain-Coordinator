// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SNAPSHOT_HPP
#define FACILITYDRAIN_SNAPSHOT_HPP

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
//
// A snapshot is a complete, self consistent copy of what the coordinator knew
// at one commit. It is taken under the coordinator's lock and rendered without
// it, so a report can never observe a half applied mutation. Every field of a
// snapshot is either a recorded fact or a value derived from recorded facts by
// a documented rule; there is no field a caller can set.

/// The newest enumeration recorded for one domain, and whether the plan's
/// binding currently accepts it.
struct DomainEnumerationSnapshot {
  bool present = false;
  EnumerationEvidence evidence{};
  /// True when the record matches the plan's bound manifest digest for this
  /// domain, and therefore counts as the plan's manifest of record.
  bool accepted = false;
  /// Why it is not accepted, when it is not. kOk when accepted.
  ErrorCode rejection = ErrorCode::kOk;
  /// The consumer records the enumeration carried.
  std::vector<ConsumerRecord> consumers{};
};

/// The newest completion report recorded for one domain.
struct DomainCompletionSnapshot {
  bool present = false;
  CompletionEvidence evidence{};
  /// True when the report agrees with the plan's generation set and manifest
  /// binding, and was observed after the plan's fence floor.
  bool compatible = false;
  ErrorCode rejection = ErrorCode::kOk;
};

struct DrainPlanSnapshot {
  DrainPlanSpec spec{};
  /// Derived from the recorded facts by the fixed rule documented in plan.hpp.
  DrainState state = DrainState::kProposed;

  std::array<DomainEnumerationSnapshot, kOwnerDomainCount> enumerations{};
  std::array<DomainCompletionSnapshot, kOwnerDomainCount> completions{};
  /// The consumer manifest of record: the consumers of the accepted enumeration
  /// per domain, falling back to the planning time manifest.
  std::vector<ConsumerRecord> consumers{};
  /// The domains that must be proven, which is the declared requirement widened
  /// by every domain that owns a mandatory obligation.
  DomainMask required_domains{};

  ResidualLedger residuals{};
  std::vector<DrainRequest> requests{};
  std::optional<SafeToRemoveGrant> grant{};
  /// True when a grant exists and still binds the current revision, the current
  /// control epoch, and a commit past the last fence.
  bool grant_live = false;
  std::optional<FenceRecord> fence{};
  std::vector<PlanHistoryEntry> history{};

  bool cancelled = false;
  std::string cancellation_detail{};
  bool failed = false;
  std::string failure_detail{};

  ObservationSequence last_observation{};
  CommitSequence last_commit{};
  Revision revision{};
  /// Digest over this plan's canonical encoding as stored.
  ContentDigest plan_digest{};

  [[nodiscard]] FACILITYDRAIN_API const ResidualLedger& residual_ledger() const noexcept { return residuals; }
};

struct CoordinatorSnapshot {
  std::uint32_t report_format_version = 0;
  ControlEpoch control_epoch{};
  IncarnationId incarnation{};
  CommitSequence commit_sequence{};
  ObservationSequence observation_sequence{};
  Limits limits{};
  bool durable = false;
  /// Empty for an ephemeral coordinator.
  std::string store_root{};
  /// In canonical plan id order.
  std::vector<DrainPlanSnapshot> plans{};
  /// Digest over the whole canonical state as stored.
  ContentDigest state_digest{};

  [[nodiscard]] FACILITYDRAIN_API const DrainPlanSnapshot* find_plan(PlanId id) const noexcept;
};

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_SNAPSHOT_HPP
