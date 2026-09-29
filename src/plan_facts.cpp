// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "coordinator_internal.hpp"

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace facilitydrain {
namespace detail {
namespace {

std::string hex_or_none(const std::optional<ContentDigest>& digest) {
  return digest.has_value() ? digest->to_hex() : std::string{"unbound"};
}

std::string hex_or_unset(const ContentDigest& digest) {
  return digest.is_zero() ? std::string{"unset"} : digest.to_hex();
}

}  // namespace

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

const PlanRecord* find_plan(const CoordinatorState& state, PlanId id) noexcept {
  const auto it = std::lower_bound(state.plans.begin(), state.plans.end(), id,
                                   [](const PlanRecord& plan, PlanId wanted) { return plan.spec.id < wanted; });
  if (it == state.plans.end() || it->spec.id != id) {
    return nullptr;
  }
  return &*it;
}

PlanRecord* find_plan(CoordinatorState& state, PlanId id) noexcept {
  const auto it = std::lower_bound(state.plans.begin(), state.plans.end(), id,
                                   [](const PlanRecord& plan, PlanId wanted) { return plan.spec.id < wanted; });
  if (it == state.plans.end() || it->spec.id != id) {
    return nullptr;
  }
  return &*it;
}

const EnumerationRecord* newest_enumeration(const PlanRecord& plan, OwnerDomain domain) noexcept {
  const auto& records = plan.enumerations[domain_index(domain)];
  if (records.empty()) {
    return nullptr;
  }
  return &records.back();
}

const CompletionEvidence* newest_completion(const PlanRecord& plan, OwnerDomain domain) noexcept {
  const auto& records = plan.completions[domain_index(domain)];
  if (records.empty()) {
    return nullptr;
  }
  return &records.back();
}

const std::vector<ConsumerRecord>& manifest_of_record(const PlanRecord& plan, OwnerDomain domain) {
  const EnumerationRecord* enumeration = newest_enumeration(plan, domain);
  if (enumeration != nullptr) {
    const auto& bound = plan.spec.bindings.manifest_digest(domain);
    if (bound.has_value() && bound.value() == enumeration->evidence.manifest_digest) {
      return enumeration->consumers;
    }
  }
  return plan.planning_consumers[domain_index(domain)];
}

bool enumeration_is_accepted(const PlanRecord& plan, OwnerDomain domain) noexcept {
  const EnumerationRecord* enumeration = newest_enumeration(plan, domain);
  if (enumeration == nullptr) {
    return false;
  }
  return enumeration_acceptance(plan, enumeration->evidence) == ErrorCode::kOk;
}

ErrorCode enumeration_acceptance(const PlanRecord& plan, const EnumerationEvidence& evidence) noexcept {
  const ObservationSequence floor = plan.fence.has_value() ? plan.fence->floor : ObservationSequence{};
  if (evidence.observed_at.value() != 0 && evidence.observed_at <= floor) {
    return ErrorCode::kStaleEvidence;
  }
  if (evidence.scope_manifest_digest != plan.spec.targets.digest()) {
    return ErrorCode::kScopeManifestMismatch;
  }
  const auto& bound = plan.spec.bindings.manifest_digest(evidence.domain);
  if (!bound.has_value() || bound.value() != evidence.manifest_digest) {
    return ErrorCode::kConsumerDigestMismatch;
  }
  return ErrorCode::kOk;
}

ErrorCode completion_compatibility(const PlanRecord& plan, const CompletionEvidence& evidence) noexcept {
  if (evidence.generations != plan.spec.bindings.generations) {
    return ErrorCode::kGenerationIncompatible;
  }
  if (evidence.scope_manifest_digest != plan.spec.targets.digest()) {
    return ErrorCode::kScopeManifestMismatch;
  }
  const auto& bound = plan.spec.bindings.manifest_digest(evidence.domain);
  if (!bound.has_value() || bound.value() != evidence.manifest_digest) {
    return ErrorCode::kEvidenceMismatch;
  }
  const ObservationSequence floor = plan.fence.has_value() ? plan.fence->floor : ObservationSequence{};
  if (evidence.observed_at.value() != 0 && evidence.observed_at <= floor) {
    return ErrorCode::kStaleEvidence;
  }
  return ErrorCode::kOk;
}

DomainMask required_domains_for_plan(const PlanRecord& plan) {
  DomainMask required = plan.spec.declared_required_domains;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    for (const auto& consumer : manifest_of_record(plan, domain)) {
      if (consumer.strength == ObligationStrength::kMandatory) {
        required = required.with(domain);
        break;
      }
    }
  }
  return required;
}

// ---------------------------------------------------------------------------
// Live authority
// ---------------------------------------------------------------------------

bool grant_is_live(const PlanRecord& plan, ControlEpoch current_epoch) noexcept {
  if (!plan.grant.has_value()) {
    return false;
  }
  const SafeToRemoveGrant& grant = plan.grant.value();
  // A grant is authority at one epoch and one revision. A restart moves the
  // epoch and a revision moves the plan, and either one means the answer must
  // be asked again rather than inherited.
  if (grant.epoch != current_epoch) {
    return false;
  }
  if (grant.revision != plan.spec.revision) {
    return false;
  }
  if (plan.fence.has_value() && plan.fence->commit > grant.granted_commit) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Canonical digests
// ---------------------------------------------------------------------------

std::string plan_canonical_text(const PlanRecord& plan) {
  std::string text;
  auto line = [&text](std::string_view value) {
    text.append(value);
    text.push_back('\n');
  };
  line("plan " + to_string(plan.spec.id));
  line("scope " + format_scope(plan.spec.scope));
  line("targets " + plan.spec.targets.to_canonical());
  line("targets-digest " + plan.spec.targets.digest().to_hex());
  line("revision " + to_string(plan.spec.revision));
  line("required " + plan.spec.declared_required_domains.to_canonical());
  line("generations " + plan.spec.bindings.generations.to_canonical());
  line("facility-epoch " + to_string(plan.spec.bindings.facility_epoch));
  line("policy-id " + to_string(plan.spec.bindings.policy_id));
  line("policy-digest " + hex_or_unset(plan.spec.bindings.policy_digest));
  line("label " + plan.spec.label);
  line("requested-by " + plan.spec.requested_by);
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    line("bound " + std::string{to_token(domain)} + " " +
         hex_or_none(plan.spec.bindings.manifest_digest(domain)));
    for (const auto& consumer : plan.planning_consumers[index]) {
      line("planning-consumer " + consumer.to_canonical());
    }
    for (const auto& enumeration : plan.enumerations[index]) {
      line("enumeration " + enumeration.evidence.to_canonical());
      for (const auto& consumer : enumeration.consumers) {
        line("enumerated-consumer " + consumer.to_canonical());
      }
    }
    for (const auto& completion : plan.completions[index]) {
      line("completion " + completion.to_canonical());
    }
  }
  for (const auto& residual : plan.residuals.entries) {
    line("residual " + residual.to_canonical());
  }
  for (const auto& request : plan.requests) {
    line("request " + request.to_canonical());
  }
  if (plan.grant.has_value()) {
    line("grant epoch " + to_string(plan.grant->epoch) + " revision " + to_string(plan.grant->revision) +
         " commit " + to_string(plan.grant->granted_commit) + " evidence " +
         plan.grant->evidence_digest.to_hex());
  } else {
    line("grant none");
  }
  if (plan.fence.has_value()) {
    line("fence " + std::string{to_token(plan.fence->reason)} + " floor " + to_string(plan.fence->floor) +
         " commit " + to_string(plan.fence->commit));
  } else {
    line("fence none");
  }
  line(std::string{"cancelled "} + (plan.cancelled ? "1" : "0"));
  line(std::string{"failed "} + (plan.failed ? "1" : "0"));
  line("last-observation " + to_string(plan.last_observation));
  line("last-commit " + to_string(plan.last_commit));
  return text;
}

ContentDigest plan_digest(const PlanRecord& plan, ControlEpoch /*current_epoch*/) {
  const std::array<ContentDigest, 1> parts{digest_text(plan_canonical_text(plan))};
  return combine_digests("plan-state", parts);
}

ContentDigest coordinator_digest(const CoordinatorState& state) {
  std::vector<ContentDigest> parts;
  parts.reserve(state.plans.size() + 1);
  for (const auto& plan : state.plans) {
    parts.push_back(digest_text(plan_canonical_text(plan)));
  }
  parts.push_back(digest_text("plans " + std::to_string(state.plans.size()) + " epoch " +
                              to_string(state.control_epoch) + " sequence " +
                              to_string(state.commit_sequence) + " observation " +
                              to_string(state.observation_sequence)));
  return combine_digests("coordinator-state", parts);
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

DrainPlanSnapshot build_plan_snapshot(const PlanRecord& plan, ControlEpoch current_epoch) {
  DrainPlanSnapshot snapshot;
  snapshot.spec = plan.spec;
  snapshot.revision = plan.spec.revision;
  snapshot.cancelled = plan.cancelled;
  snapshot.cancellation_detail = plan.cancellation_detail;
  snapshot.failed = plan.failed;
  snapshot.failure_detail = plan.failure_detail;
  snapshot.last_observation = plan.last_observation;
  snapshot.last_commit = plan.last_commit;
  snapshot.residuals = plan.residuals;
  snapshot.requests = plan.requests;
  snapshot.grant = plan.grant;
  snapshot.grant_live = grant_is_live(plan, current_epoch);
  snapshot.fence = plan.fence;
  snapshot.history = plan.history;
  snapshot.required_domains = required_domains_for_plan(plan);

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    const ObservationSequence floor = plan.fence.has_value() ? plan.fence->floor : ObservationSequence{};

    if (const EnumerationRecord* enumeration = newest_enumeration(plan, domain); enumeration != nullptr) {
      DomainEnumerationSnapshot& view = snapshot.enumerations[index];
      view.present = true;
      view.evidence = enumeration->evidence;
      view.consumers = enumeration->consumers;
      const ErrorCode acceptance = enumeration_acceptance(plan, enumeration->evidence);
      view.accepted = acceptance == ErrorCode::kOk;
      view.rejection = acceptance;
    }

    if (const CompletionEvidence* completion = newest_completion(plan, domain); completion != nullptr) {
      DomainCompletionSnapshot& view = snapshot.completions[index];
      view.present = true;
      view.evidence = *completion;
      const ErrorCode compatibility = completion_compatibility(plan, *completion);
      view.compatible = compatibility == ErrorCode::kOk;
      view.rejection = compatibility;
    }

    const auto& consumers = manifest_of_record(plan, domain);
    snapshot.consumers.insert(snapshot.consumers.end(), consumers.begin(), consumers.end());
  }

  std::sort(snapshot.consumers.begin(), snapshot.consumers.end());
  snapshot.state = derive_state_from_snapshot(snapshot, current_epoch);
  snapshot.plan_digest = plan_digest(plan, current_epoch);
  return snapshot;
}

CoordinatorSnapshot build_snapshot(const CoordinatorState& state, bool durable, const std::string& store_root) {
  CoordinatorSnapshot snapshot;
  snapshot.report_format_version = kReportFormatVersion;
  snapshot.control_epoch = state.control_epoch;
  snapshot.incarnation = state.incarnation;
  snapshot.commit_sequence = state.commit_sequence;
  snapshot.observation_sequence = state.observation_sequence;
  snapshot.limits = state.limits;
  snapshot.durable = durable;
  snapshot.store_root = store_root;
  snapshot.plans.reserve(state.plans.size());
  for (const auto& plan : state.plans) {
    snapshot.plans.push_back(build_plan_snapshot(plan, state.control_epoch));
  }
  snapshot.state_digest = coordinator_digest(state);
  return snapshot;
}

DrainState derive_state(const PlanRecord& plan, ControlEpoch current_epoch) noexcept {
  // The derivation runs over the same snapshot the queries return, so a plan
  // can never report one state to an operator and a different one to a verdict.
  const DrainPlanSnapshot snapshot = build_plan_snapshot(plan, current_epoch);
  return snapshot.state;
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

ContentDigest obligation_digest_for(const PlanRecord& plan, OwnerDomain domain) {
  return consumer_manifest_digest(domain, manifest_of_record(plan, domain));
}

std::string target_system_for(OwnerDomain domain) {
  switch (domain) {
    case OwnerDomain::kAsi:
      return "asi";
    case OwnerDomain::kDfi:
      return "dfi";
    case OwnerDomain::kFacility:
      return "facility";
    case OwnerDomain::kMonitoring:
      return "monitoring";
  }
  return "unknown";
}

std::string instruction_for(OwnerDomain domain, const DrainScope& scope, std::uint32_t bound,
                            std::size_t obligation_count) {
  // The text is a function of the key and the bound only, so a replayed key
  // produces byte identical text and an owner can compare it directly.
  return "drain domain=" + std::string{to_token(domain)} + " scope=" + format_scope(scope) +
         " obligations=" + std::to_string(obligation_count) + " bound=" + std::to_string(bound);
}

// ---------------------------------------------------------------------------
// Fencing and history
// ---------------------------------------------------------------------------

void apply_fence(PlanRecord& plan, FenceReason reason, ObservationSequence floor, Revision revision,
                 ControlEpoch epoch, CommitSequence commit, std::int64_t clock_milliseconds,
                 std::string detail) {
  FenceRecord fence;
  fence.reason = reason;
  fence.floor = floor;
  fence.revision = revision;
  fence.epoch = epoch;
  fence.commit = commit;
  fence.recorded_at_milliseconds = clock_milliseconds;
  fence.detail = std::move(detail);
  plan.fence = fence;
}

std::optional<FenceRecord> fence_live_grant(PlanRecord& plan, FenceReason reason, ObservationSequence floor,
                                            ControlEpoch epoch, CommitSequence commit,
                                            std::int64_t clock_milliseconds, std::string detail) {
  if (!plan.grant.has_value()) {
    return std::nullopt;
  }
  const bool was_live = plan.grant->epoch == epoch || plan.grant->revision == plan.spec.revision;
  if (!was_live) {
    // Remembering the old grant is honest history; keeping it as the plan's
    // current answer is not, so it is dropped either way.
    plan.grant.reset();
    return std::nullopt;
  }
  apply_fence(plan, reason, floor, plan.spec.revision, epoch, commit, clock_milliseconds, std::move(detail));
  FenceRecord recorded = plan.fence.value();
  plan.grant.reset();
  return recorded;
}

bool record_history(PlanRecord& plan, DrainState previous, DrainState current, PlanOperation cause,
                    ObservationSequence observation, CommitSequence commit, std::int64_t clock_milliseconds,
                    const Limits& limits, std::string detail) {
  if (previous == current) {
    return false;
  }
  PlanHistoryEntry entry;
  entry.from_state = previous;
  entry.to_state = current;
  entry.cause = cause;
  entry.observed_at = observation;
  entry.commit = commit;
  entry.recorded_at_milliseconds = clock_milliseconds;
  entry.detail = std::move(detail);
  plan.history.push_back(entry);
  if (limits.max_history_per_plan != 0 && plan.history.size() > limits.max_history_per_plan) {
    const std::size_t excess = plan.history.size() - limits.max_history_per_plan;
    plan.history.erase(plan.history.begin(), plan.history.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  return true;
}

}  // namespace detail

const DrainPlanSnapshot* CoordinatorSnapshot::find_plan(PlanId id) const noexcept {
  const auto it = std::lower_bound(plans.begin(), plans.end(), id,
                                   [](const DrainPlanSnapshot& plan, PlanId wanted) {
                                     return plan.spec.id < wanted;
                                   });
  if (it == plans.end() || it->spec.id != id) {
    return nullptr;
  }
  return &*it;
}

}  // namespace facilitydrain
