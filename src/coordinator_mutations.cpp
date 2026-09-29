// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "coordinator_session.hpp"

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace facilitydrain {
namespace {

using detail::CoordinatorState;
using detail::PlanRecord;

/// Orders records canonically before validating them, so the same manifest with
/// its entries listed in a different order always reports the same primary
/// error. Canonical order is by obligation, then by the full canonical text.
std::vector<ConsumerRecord> canonical_consumer_order(const std::vector<ConsumerRecord>& consumers) {
  std::vector<ConsumerRecord> ordered = consumers;
  std::sort(ordered.begin(), ordered.end(), [](const ConsumerRecord& lhs, const ConsumerRecord& rhs) {
    if (lhs.obligation != rhs.obligation) {
      return lhs.obligation < rhs.obligation;
    }
    return lhs.to_canonical() < rhs.to_canonical();
  });
  return ordered;
}

/// Validates every record and then each domain's manifest as a whole. The
/// grouping is derived from the category, which is the only thing that decides
/// which system owns an obligation.
Status validate_consumers(const std::vector<ConsumerRecord>& consumers, const Limits& limits,
                          std::array<std::vector<ConsumerRecord>, kOwnerDomainCount>& grouped) {
  const std::vector<ConsumerRecord> ordered = canonical_consumer_order(consumers);
  for (const auto& consumer : ordered) {
    const Status valid = consumer.validate(limits);
    if (!valid.ok()) {
      return valid;
    }
  }
  for (const auto& consumer : ordered) {
    grouped[domain_index(consumer.domain())].push_back(consumer);
  }
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const Status valid = validate_domain_manifest(owner_domain_at(index), grouped[index], limits);
    if (!valid.ok()) {
      return valid;
    }
  }
  return Status::success();
}

/// The fence floor after a change. Only a reason that invalidates the meaning of
/// earlier evidence moves the floor forward; a restart or an epoch change means
/// the authority is gone, not that the evidence became false.
ObservationSequence advanced_floor(const PlanRecord& plan, FenceReason reason, ObservationSequence observation) {
  const ObservationSequence existing = plan.fence.has_value() ? plan.fence->floor : ObservationSequence{};
  switch (reason) {
    case FenceReason::kRestart:
    case FenceReason::kControlEpochChanged:
    case FenceReason::kPlanCancelled:
    case FenceReason::kPlanFailed:
      return existing;
    case FenceReason::kNone:
    case FenceReason::kNewObligation:
    case FenceReason::kPlanRevised:
    case FenceReason::kEvidenceSuperseded:
    case FenceReason::kScopeManifestChanged:
    case FenceReason::kOperatorFence:
    case FenceReason::kDependencyChange:
      return observation > existing ? observation : existing;
  }
  return observation > existing ? observation : existing;
}

/// Withdraws a removal answer if one is recorded, and records why. Returns true
/// when something was withdrawn.
bool withdraw_authority(PlanRecord& plan, FenceReason reason, ObservationSequence observation,
                        ControlEpoch epoch, CommitSequence commit, std::int64_t milliseconds,
                        std::string detail) {
  if (!plan.grant.has_value()) {
    return false;
  }
  const ObservationSequence floor = advanced_floor(plan, reason, observation);
  detail::apply_fence(plan, reason, floor, plan.spec.revision, epoch, commit, milliseconds, std::move(detail));
  plan.grant.reset();
  return true;
}

ContentDigest scope_digest_of(const PlanRecord& plan) { return plan.spec.targets.digest(); }

// An enumeration value that is outside its defined set is not a default, a
// wildcard or an unknown case to be handled later: it indexes tables and
// selects branches. It is checked here, at the boundary, before anything reads
// it, and it is checked exhaustively rather than with a range test so that a
// value added to an enumeration cannot slip through unnoticed.
[[nodiscard]] bool is_known(OwnerDomain domain) noexcept {
  switch (domain) {
    case OwnerDomain::kAsi:
    case OwnerDomain::kDfi:
    case OwnerDomain::kFacility:
    case OwnerDomain::kMonitoring:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known(CoverageState coverage) noexcept {
  switch (coverage) {
    case CoverageState::kNotEnumerated:
    case CoverageState::kPartial:
    case CoverageState::kComplete:
    case CoverageState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known(CompletionState state) noexcept {
  switch (state) {
    case CompletionState::kUnknown:
    case CompletionState::kRequested:
    case CompletionState::kAcknowledged:
    case CompletionState::kDraining:
    case CompletionState::kDrainedWithResiduals:
    case CompletionState::kDrained:
    case CompletionState::kRefused:
    case CompletionState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known(ResidualKind kind) noexcept {
  switch (kind) {
    case ResidualKind::kObligationActive:
    case ResidualKind::kObligationUnknown:
    case ResidualKind::kOwnerRefused:
    case ResidualKind::kEnumerationIncomplete:
    case ResidualKind::kEvidenceMissing:
    case ResidualKind::kResidualCountUnknown:
    case ResidualKind::kRequestUnacknowledged:
    case ResidualKind::kDomainFailed:
    case ResidualKind::kEvidenceStale:
    case ResidualKind::kProtectedObligation:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::kNone:
    case FenceReason::kRestart:
    case FenceReason::kControlEpochChanged:
    case FenceReason::kNewObligation:
    case FenceReason::kPlanRevised:
    case FenceReason::kEvidenceSuperseded:
    case FenceReason::kScopeManifestChanged:
    case FenceReason::kOperatorFence:
    case FenceReason::kPlanCancelled:
    case FenceReason::kPlanFailed:
    case FenceReason::kDependencyChange:
      return true;
  }
  return false;
}

[[nodiscard]] Status require_known_domain(OwnerDomain domain) {
  if (is_known(domain)) {
    return Status::success();
  }
  return Status::failure(ErrorCode::kInvalidEnumValue,
                         "the owning domain value is outside the four defined domains");
}

void trim_records(std::vector<detail::EnumerationRecord>& records, std::uint32_t bound) {
  if (bound != 0 && records.size() > bound) {
    const std::size_t excess = records.size() - bound;
    records.erase(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

void trim_records(std::vector<CompletionEvidence>& records, std::uint32_t bound) {
  if (bound != 0 && records.size() > bound) {
    const std::size_t excess = records.size() - bound;
    records.erase(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

DrainPlanSnapshot committed_plan(const Coordinator::Impl& impl, PlanId id) {
  const PlanRecord* record = detail::find_plan(impl.state, id);
  if (record == nullptr) {
    return DrainPlanSnapshot{};
  }
  return detail::build_plan_snapshot(*record, impl.state.control_epoch);
}

}  // namespace

// ---------------------------------------------------------------------------
// create_plan
// ---------------------------------------------------------------------------

Result<CreatePlanOutcome> Coordinator::create_plan(const CreatePlanRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const Status context = detail::validate_context(*impl_, request.context, nullptr, false, limits);
  if (!context.ok()) {
    return Result<CreatePlanOutcome>{context.error()};
  }
  if (request.context.plan != request.id) {
    return make_error<CreatePlanOutcome>(ErrorCode::kConflictingField,
                                         "the context names plan " + to_string(request.context.plan) +
                                             " but the plan being created is " + to_string(request.id));
  }
  if (request.id.value() == 0) {
    return make_error<CreatePlanOutcome>(ErrorCode::kMissingRequiredField, "the plan id is not set");
  }
  if (request.scope.id == 0) {
    return make_error<CreatePlanOutcome>(ErrorCode::kInvalidScope, "the plan scope id is zero");
  }
  if (request.declared_required_domains.empty()) {
    // A plan that requires nothing proven could authorise anything, so the
    // requirement is part of the plan rather than an optional refinement.
    return make_error<CreatePlanOutcome>(ErrorCode::kMissingRequiredField,
                                         "the plan declares no required domain");
  }
  if (!request.generations.is_complete()) {
    return make_error<CreatePlanOutcome>(ErrorCode::kMissingRequiredField,
                                         "the plan generation set is incomplete");
  }
  if (request.policy_digest.is_zero()) {
    return make_error<CreatePlanOutcome>(ErrorCode::kMissingRequiredField, "the policy digest is not set");
  }
  if (request.policy_id.value() == 0) {
    return make_error<CreatePlanOutcome>(ErrorCode::kMissingRequiredField, "the policy id is not set");
  }
  const Status label = detail::validate_text(request.label, "label", limits.max_text_bytes);
  if (!label.ok()) {
    return Result<CreatePlanOutcome>{label.error()};
  }
  const Status author = detail::validate_text(request.requested_by, "requested_by", limits.max_text_bytes);
  if (!author.ok()) {
    return Result<CreatePlanOutcome>{author.error()};
  }

  auto manifest = DrainTargetManifest::create(request.targets, limits);
  if (!manifest) {
    return Result<CreatePlanOutcome>{manifest.error()};
  }
  if (!manifest.value().contains(request.scope)) {
    return make_error<CreatePlanOutcome>(
        ErrorCode::kInvalidScope,
        "the plan scope " + format_scope(request.scope) + " is not a member of its own target manifest");
  }
  if (detail::find_plan(impl_->state, request.id) != nullptr) {
    return make_error<CreatePlanOutcome>(ErrorCode::kDuplicateIdentifier,
                                         "plan " + to_string(request.id) + " already exists");
  }
  if (impl_->state.plans.size() >= limits.max_plans) {
    return make_error<CreatePlanOutcome>(
        ErrorCode::kTooManyEntries, "the coordinator already holds " + std::to_string(limits.max_plans) +
                                        " plans, which is the configured bound");
  }

  std::array<std::vector<ConsumerRecord>, kOwnerDomainCount> grouped{};
  const Status consumers = validate_consumers(request.consumers, limits, grouped);
  if (!consumers.ok()) {
    return Result<CreatePlanOutcome>{consumers.error()};
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;

  PlanRecord record;
  record.spec.id = request.id;
  record.spec.scope = request.scope;
  record.spec.targets = std::move(manifest).value();
  record.spec.bindings.facility_epoch = impl_->state.control_epoch;
  record.spec.bindings.generations = request.generations;
  record.spec.bindings.policy_id = request.policy_id;
  record.spec.bindings.policy_digest = request.policy_digest;
  record.spec.revision = Revision{1};
  record.spec.declared_required_domains = request.declared_required_domains;
  record.spec.label = request.label;
  record.spec.requested_by = request.requested_by;
  record.spec.created_at_milliseconds = detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);
  record.revision = record.spec.revision;
  record.last_observation = request.context.observation;
  record.last_commit = next.commit_sequence;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    record.planning_consumers[index] = std::move(grouped[index]);
    if (request.bind_consumer_manifests) {
      record.spec.bindings.set_manifest_digest(
          owner_domain_at(index),
          consumer_manifest_digest(owner_domain_at(index), record.planning_consumers[index]));
    }
  }

  const auto position = std::lower_bound(
      next.plans.begin(), next.plans.end(), request.id,
      [](const PlanRecord& plan, PlanId wanted) { return plan.spec.id < wanted; });
  next.plans.insert(position, std::move(record));

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<CreatePlanOutcome>{published.error()};
  }

  CreatePlanOutcome outcome;
  outcome.plan = committed_plan(*impl_, request.id);
  return Result<CreatePlanOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// revise_plan
// ---------------------------------------------------------------------------

Result<RevisePlanOutcome> Coordinator::revise_plan(const RevisePlanRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<RevisePlanOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kRevise);
  if (!admissible.ok()) {
    return Result<RevisePlanOutcome>{admissible.error()};
  }
  if (!is_known(request.reason) || request.reason == FenceReason::kNone) {
    return make_error<RevisePlanOutcome>(ErrorCode::kInvalidEnumValue,
                                         "a revision must state why it is being made");
  }
  const Status detail_text = detail::validate_text(request.detail, "revision detail", limits.max_annotation_bytes);
  if (!detail_text.ok()) {
    return Result<RevisePlanOutcome>{detail_text.error()};
  }

  std::array<std::vector<ConsumerRecord>, kOwnerDomainCount> grouped{};
  bool rebind = false;
  if (request.consumers.has_value()) {
    const Status consumers = validate_consumers(request.consumers.value(), limits, grouped);
    if (!consumers.ok()) {
      return Result<RevisePlanOutcome>{consumers.error()};
    }
    rebind = true;
  }
  if (request.generations.has_value() && !request.generations->is_complete()) {
    return make_error<RevisePlanOutcome>(ErrorCode::kMissingRequiredField,
                                         "the revised generation set is incomplete");
  }
  if (request.declared_required_domains.has_value() && request.declared_required_domains->empty()) {
    return make_error<RevisePlanOutcome>(ErrorCode::kMissingRequiredField,
                                         "the revised plan declares no required domain");
  }
  if (request.policy_digest.has_value() && request.policy_digest->is_zero()) {
    return make_error<RevisePlanOutcome>(ErrorCode::kMissingRequiredField, "the revised policy digest is zero");
  }

  std::optional<DrainTargetManifest> manifest;
  if (request.targets.has_value()) {
    auto created = DrainTargetManifest::create(request.targets.value(), limits);
    if (!created) {
      return Result<RevisePlanOutcome>{created.error()};
    }
    if (!created.value().contains(current->spec.scope)) {
      return make_error<RevisePlanOutcome>(
          ErrorCode::kInvalidScope,
          "the revised manifest does not contain the plan scope " + format_scope(current->spec.scope));
    }
    manifest = std::move(created).value();
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;

  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const bool had_grant = plan->grant.has_value();

  plan->spec.revision = Revision{plan->spec.revision.value() + 1U};
  plan->revision = plan->spec.revision;
  if (manifest.has_value()) {
    plan->spec.targets = std::move(manifest).value();
  }
  if (request.generations.has_value()) {
    plan->spec.bindings.generations = request.generations.value();
  }
  if (request.policy_id.has_value()) {
    plan->spec.bindings.policy_id = request.policy_id.value();
  }
  if (request.policy_digest.has_value()) {
    plan->spec.bindings.policy_digest = request.policy_digest.value();
  }
  if (request.declared_required_domains.has_value()) {
    plan->spec.declared_required_domains = request.declared_required_domains.value();
  }
  if (rebind) {
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      plan->planning_consumers[index] = std::move(grouped[index]);
      plan->spec.bindings.set_manifest_digest(
          owner_domain_at(index),
          consumer_manifest_digest(owner_domain_at(index), plan->planning_consumers[index]));
    }
  }

  // A revision always withdraws a recorded answer: the plan a verdict applied
  // to no longer exists. It only moves the fence floor when it changes what the
  // scope physically contains, or when it announces an obligation that did not
  // exist before. A generation or policy change is caught by the compatibility
  // comparison instead, which is a content check rather than an ordering one:
  // moving the floor as well would refuse evidence that is still perfectly
  // valid, and a system that refuses valid evidence is a system operators work
  // around.
  const bool membership_changed = manifest.has_value() || rebind;
  const bool announces_new_obligations = request.reason == FenceReason::kNewObligation;
  const ObservationSequence floor =
      (membership_changed || announces_new_obligations)
          ? advanced_floor(*plan, request.reason, request.context.observation)
          : (plan->fence.has_value() ? plan->fence->floor : ObservationSequence{});
  plan->grant.reset();
  detail::apply_fence(*plan, request.reason, floor, plan->spec.revision, next.control_epoch,
                      next.commit_sequence,
                      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds),
                      request.detail.empty() ? std::string{"plan revised"} : request.detail);
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;

  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kRevise, request.context.observation,
                          next.commit_sequence,
                          detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds),
                          limits, "revision " + to_string(plan->spec.revision));

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<RevisePlanOutcome>{published.error()};
  }

  RevisePlanOutcome outcome;
  outcome.plan = committed_plan(*impl_, request.context.plan);
  outcome.fenced = had_grant;
  outcome.fence = outcome.plan.fence;
  return Result<RevisePlanOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// record_enumeration
// ---------------------------------------------------------------------------

Result<RecordEnumerationOutcome> Coordinator::record_enumeration(const RecordEnumerationRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<RecordEnumerationOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kRecordEnumeration);
  if (!admissible.ok()) {
    return Result<RecordEnumerationOutcome>{admissible.error()};
  }
  const Status domain_known = require_known_domain(request.domain);
  if (!domain_known.ok()) {
    return Result<RecordEnumerationOutcome>{domain_known.error()};
  }
  if (request.generation.value() == 0) {
    return make_error<RecordEnumerationOutcome>(ErrorCode::kMissingRequiredField,
                                                "the enumeration generation is zero");
  }
  if (!is_known(request.coverage)) {
    return make_error<RecordEnumerationOutcome>(ErrorCode::kInvalidEnumValue,
                                                "the enumeration coverage is outside the defined states");
  }
  if (request.coverage == CoverageState::kNotEnumerated) {
    return make_error<RecordEnumerationOutcome>(
        ErrorCode::kInvalidEnumValue, "not-enumerated is not a coverage a report may claim");
  }
  if (request.observed_at.value() == 0) {
    return make_error<RecordEnumerationOutcome>(ErrorCode::kMissingRequiredField,
                                                "the enumeration observation sequence is zero");
  }
  if (!request.generations.is_complete()) {
    return make_error<RecordEnumerationOutcome>(ErrorCode::kMissingRequiredField,
                                                "the enumeration generation set is incomplete");
  }
  if (request.scope_manifest_digest.is_zero()) {
    return make_error<RecordEnumerationOutcome>(ErrorCode::kMissingRequiredField,
                                                "the scope manifest digest is not set");
  }
  if (request.scope_manifest_digest != scope_digest_of(*current)) {
    return make_error<RecordEnumerationOutcome>(
        ErrorCode::kScopeManifestMismatch,
        "the owner enumerated a different physical membership than the plan covers");
  }
  const Status source = detail::validate_text(request.source, "source", limits.max_text_bytes);
  if (!source.ok()) {
    return Result<RecordEnumerationOutcome>{source.error()};
  }
  const Status annotation = detail::validate_text(request.annotation, "annotation", limits.max_annotation_bytes);
  if (!annotation.ok()) {
    return Result<RecordEnumerationOutcome>{annotation.error()};
  }
  if (request.generation <= detail::latest_evidence_generation(*current, request.domain)) {
    return make_error<RecordEnumerationOutcome>(
        ErrorCode::kStaleEvidence,
        "evidence generation " + to_string(request.generation) + " does not advance the newest generation " +
            to_string(detail::latest_evidence_generation(*current, request.domain)) + " for this domain");
  }


  std::array<std::vector<ConsumerRecord>, kOwnerDomainCount> grouped{};
  const Status consumers = validate_consumers(request.consumers, limits, grouped);
  if (!consumers.ok()) {
    return Result<RecordEnumerationOutcome>{consumers.error()};
  }

  EnumerationEvidence evidence;
  evidence.domain = request.domain;
  evidence.coverage = request.coverage;
  evidence.generation = request.generation;
  evidence.observed_at = request.observed_at;
  evidence.manifest_digest = consumer_manifest_digest(
      request.domain, grouped[domain_index(request.domain)]);
  evidence.scope_manifest_digest = request.scope_manifest_digest;
  evidence.generations = request.generations;
  evidence.source = request.source;
  evidence.annotation = request.annotation;
  evidence.observed_at_milliseconds = detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  const auto& bound = current->spec.bindings.manifest_digest(request.domain);
  RecordEnumerationOutcome outcome;
  if (!bound.has_value()) {
    if (request.coverage != CoverageState::kComplete) {
      return make_error<RecordEnumerationOutcome>(
          ErrorCode::kConsumerDigestMismatch,
          "no consumer manifest digest is bound for this domain yet, and only a complete enumeration can "
          "bind one");
    }
    outcome.bound_manifest = true;
    outcome.accepted = true;
  } else if (bound.value() != evidence.manifest_digest) {
    // The owner reports a different set of consumers than the plan was planned
    // against. Silently adopting it would change what the plan means, so the
    // plan must be revised deliberately instead.
    return make_error<RecordEnumerationOutcome>(
        ErrorCode::kConsumerDigestMismatch,
        "the reported consumer manifest " + evidence.manifest_digest.to_hex() +
            " does not match the plan binding " + bound.value().to_hex() +
            "; revise the plan to adopt the new manifest");
  } else {
    outcome.accepted = true;
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);

  if (outcome.bound_manifest) {
    plan->spec.bindings.set_manifest_digest(request.domain, evidence.manifest_digest);
  }
  outcome.fenced = withdraw_authority(
      *plan, FenceReason::kEvidenceSuperseded, request.context.observation, next.control_epoch,
      next.commit_sequence,
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds),
      "new enumeration evidence was recorded for " + std::string{to_token(request.domain)});
  if (outcome.fenced) {
    outcome.fence = plan->fence;
  }

  detail::EnumerationRecord record;
  record.evidence = evidence;
  record.consumers = std::move(grouped[domain_index(request.domain)]);
  plan->enumerations[domain_index(request.domain)].push_back(std::move(record));
  trim_records(plan->enumerations[domain_index(request.domain)], limits.max_evidence_per_domain);
  outcome.consumers_recorded =
      static_cast<std::uint32_t>(plan->enumerations[domain_index(request.domain)].back().consumers.size());
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;

  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kRecordEnumeration, request.context.observation,
                          next.commit_sequence,
                          detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds),
                          limits, std::string{to_token(request.domain)} + " enumeration " +
                                     to_string(request.generation));

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<RecordEnumerationOutcome>{published.error()};
  }
  outcome.evidence = evidence;
  return Result<RecordEnumerationOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// issue_requests
// ---------------------------------------------------------------------------

Result<IssueRequestsOutcome> Coordinator::issue_requests(const IssueRequestsRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<IssueRequestsOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kIssueRequests);
  if (!admissible.ok()) {
    return Result<IssueRequestsOutcome>{admissible.error()};
  }
  if (request.domains.empty()) {
    return make_error<IssueRequestsOutcome>(ErrorCode::kMissingRequiredField,
                                            "no domain was named for the drain request");
  }
  if (request.bound_operations > limits.max_consumers_per_domain) {
    return make_error<IssueRequestsOutcome>(
        ErrorCode::kLimitExceeded, "the request bound exceeds the configured consumer bound");
  }

  // Every named domain must have an accepted, complete enumeration before it can
  // be asked to drain: a request is a statement about a known set of
  // obligations, and an unenumerated set is not known.
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!request.domains.contains(domain)) {
      continue;
    }
    const detail::EnumerationRecord* enumeration = detail::newest_enumeration(*current, domain);
    if (enumeration == nullptr) {
      return make_error<IssueRequestsOutcome>(
          ErrorCode::kIncompleteEnumeration,
          "no enumeration has been recorded for " + std::string{to_token(domain)} +
              ", so its obligations are not known");
    }
    if (enumeration->evidence.coverage != CoverageState::kComplete) {
      return make_error<IssueRequestsOutcome>(
          ErrorCode::kIncompleteEnumeration,
          "the enumeration for " + std::string{to_token(domain)} + " is not complete");
    }
    const ErrorCode acceptance = detail::enumeration_acceptance(*current, enumeration->evidence);
    if (acceptance != ErrorCode::kOk) {
      return make_error<IssueRequestsOutcome>(
          acceptance, "the enumeration for " + std::string{to_token(domain)} +
                          " is not accepted by the plan binding");
    }
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  IssueRequestsOutcome outcome;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!request.domains.contains(domain)) {
      continue;
    }
    const auto& manifest = detail::manifest_of_record(*plan, domain);
    const ContentDigest obligations = consumer_manifest_digest(domain, manifest);

    std::uint32_t mandatory = 0;
    for (const auto& consumer : manifest) {
      if (consumer.strength == ObligationStrength::kMandatory) {
        ++mandatory;
      }
    }
    if (manifest.empty()) {
      IssueItem item;
      item.deliver = false;
      item.detail = "no obligations are declared for " + std::string{to_token(domain)};
      outcome.items.push_back(std::move(item));
      continue;
    }

    const std::uint32_t bound = request.bound_operations != 0 ? request.bound_operations : mandatory;
    if (bound == 0) {
      return make_error<IssueRequestsOutcome>(
          ErrorCode::kMissingRequiredField,
          "the derived bound for " + std::string{to_token(domain)} + " is zero");
    }

    DrainRequestKey key;
    key.plan = plan->spec.id;
    key.domain = domain;
    key.scope = plan->spec.scope;
    key.obligation_digest = obligations;
    key.policy_generation = plan->spec.bindings.generations.policy;
    key.attempt = AttemptId{1};

    // The idempotency key deliberately excludes the revision: a bookkeeping
    // change must never turn one physical drain into two. Only an explicit
    // supersede, which records who decided and why, starts a new attempt.
    const DrainRequestId deriving_id = request_id_for(key);
    const DrainRequest* existing = nullptr;
    AttemptId highest_attempt = AttemptId{0};
    for (const auto& recorded : plan->requests) {
      if (recorded.key.domain != domain || recorded.key.scope != key.scope ||
          recorded.key.obligation_digest != key.obligation_digest ||
          recorded.key.policy_generation != key.policy_generation) {
        continue;
      }
      if (recorded.key.attempt > highest_attempt) {
        highest_attempt = recorded.key.attempt;
      }
      if (recorded.state != RequestState::kSuperseded) {
        existing = &recorded;
      }
    }

    if (existing != nullptr) {
      IssueItem item;
      item.request = *existing;
      item.duplicate = true;
      // A record that is still staged has a durable intent but no confirmed
      // delivery, so it is offered again with the same idempotency key: the
      // owner sees a replay, not a second drain.
      item.deliver = existing->state == RequestState::kStaged;
      item.detail = item.deliver ? "re-offering a staged request with the same idempotency key"
                                 : std::string{"already recorded in state "} + std::string{to_token(existing->state)};
      if (item.deliver) {
        outcome.to_deliver.push_back(item.request);
      }
      ++outcome.duplicates;
      outcome.items.push_back(std::move(item));
      continue;
    }

    DrainRequest drafted;
    drafted.key = key;
    if (highest_attempt.value() != 0) {
      if (highest_attempt.value() + 1U > limits.max_attempts_per_key) {
        return make_error<IssueRequestsOutcome>(
            ErrorCode::kCounterExhausted,
            "the attempt counter for this request key has reached its configured bound");
      }
      drafted.key.attempt = AttemptId{highest_attempt.value() + 1U};
    }
    drafted.id = deriving_id;
    drafted.id = request_id_for(drafted.key);
    drafted.idempotency_key = request_idempotency_key(drafted.key);
    drafted.state = RequestState::kStaged;
    drafted.staged_at = request.context.observation;
    drafted.bound_operations = bound;
    drafted.target_system = detail::target_system_for(domain);
    drafted.instruction = detail::instruction_for(domain, plan->spec.scope, bound, manifest.size());
    drafted.staged_at_milliseconds = milliseconds;
    const Status valid = drafted.validate(limits);
    if (!valid.ok()) {
      return Result<IssueRequestsOutcome>{valid.error()};
    }
    if (plan->requests.size() >= limits.max_requests_per_plan) {
      return make_error<IssueRequestsOutcome>(
          ErrorCode::kTooManyEntries, "the plan already holds the configured maximum of drain requests");
    }

    IssueItem item;
    item.request = drafted;
    item.deliver = true;
    item.detail = "staged";
    outcome.to_deliver.push_back(drafted);
    ++outcome.newly_staged;
    plan->requests.push_back(std::move(drafted));
    std::sort(plan->requests.begin(), plan->requests.end());
    outcome.items.push_back(std::move(item));
  }

  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kIssueRequests, request.context.observation,
                          next.commit_sequence, milliseconds, limits,
                          std::to_string(outcome.newly_staged) + " staged, " +
                              std::to_string(outcome.duplicates) + " already recorded");

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<IssueRequestsOutcome>{published.error()};
  }
  return Result<IssueRequestsOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// confirm_delivery
// ---------------------------------------------------------------------------

Result<ConfirmDeliveryOutcome> Coordinator::confirm_delivery(const ConfirmDeliveryRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<ConfirmDeliveryOutcome>{context.error()};
  }
  const DrainRequest* found = nullptr;
  for (const auto& recorded : current->requests) {
    if (recorded.id == request.request) {
      found = &recorded;
      break;
    }
  }
  if (found == nullptr) {
    return make_error<ConfirmDeliveryOutcome>(
        ErrorCode::kRequestNotFound, "request " + to_string(request.request) + " is not recorded on this plan");
  }
  if (found->state == RequestState::kSuperseded) {
    return make_error<ConfirmDeliveryOutcome>(ErrorCode::kRequestSuperseded,
                                              "the request was replaced by a later attempt");
  }
  if (found->state == RequestState::kCancelled) {
    return make_error<ConfirmDeliveryOutcome>(ErrorCode::kInvalidStateTransition,
                                              "the request was cancelled with the plan");
  }
  if (found->state != RequestState::kStaged) {
    // Delivery confirmed twice is a replay of the same statement, not a second
    // delivery, so it is accepted and changes nothing.
    ConfirmDeliveryOutcome outcome;
    outcome.request = *found;
    return Result<ConfirmDeliveryOutcome>{std::move(outcome)};
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  for (auto& recorded : plan->requests) {
    if (recorded.id != request.request) {
      continue;
    }
    recorded.state = RequestState::kIssued;
    recorded.issued_at = request.context.observation;
    recorded.issued_at_milliseconds = milliseconds;
    recorded.settlement_detail = request.delivery_reference;
    break;
  }
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kIssueRequests, request.context.observation,
                          next.commit_sequence, milliseconds, limits,
                          "delivery confirmed for request " + to_string(request.request));

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<ConfirmDeliveryOutcome>{published.error()};
  }
  ConfirmDeliveryOutcome outcome;
  for (const auto& recorded : detail::find_plan(impl_->state, request.context.plan)->requests) {
    if (recorded.id == request.request) {
      outcome.request = recorded;
      break;
    }
  }
  return Result<ConfirmDeliveryOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// record_acknowledgement
// ---------------------------------------------------------------------------

Result<RecordAcknowledgementOutcome> Coordinator::record_acknowledgement(
    const RecordAcknowledgementRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<RecordAcknowledgementOutcome>{context.error()};
  }
  const DrainRequest* found = nullptr;
  for (const auto& recorded : current->requests) {
    if (recorded.id == request.request) {
      found = &recorded;
      break;
    }
  }
  if (found == nullptr) {
    return make_error<RecordAcknowledgementOutcome>(
        ErrorCode::kRequestNotFound, "request " + to_string(request.request) + " is not recorded on this plan");
  }
  if (found->state == RequestState::kStaged) {
    // An owner cannot acknowledge something it was never handed. Confirming
    // delivery first is what keeps the record honest.
    return make_error<RecordAcknowledgementOutcome>(
        ErrorCode::kInvalidStateTransition,
        "the request has not been delivered, so it cannot have been acknowledged");
  }
  if (found->state == RequestState::kSuperseded) {
    return make_error<RecordAcknowledgementOutcome>(ErrorCode::kRequestSuperseded,
                                                    "the request was replaced by a later attempt");
  }
  if (found->state == RequestState::kCancelled) {
    return make_error<RecordAcknowledgementOutcome>(ErrorCode::kInvalidStateTransition,
                                                    "the request was cancelled with the plan");
  }
  const Status source =
      detail::validate_text(request.acknowledging_system, "acknowledging_system", limits.max_text_bytes);
  if (!source.ok()) {
    return Result<RecordAcknowledgementOutcome>{source.error()};
  }
  if (is_request_acknowledged(found->state)) {
    RecordAcknowledgementOutcome outcome;
    outcome.request = *found;
    return Result<RecordAcknowledgementOutcome>{std::move(outcome)};
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  for (auto& recorded : plan->requests) {
    if (recorded.id != request.request) {
      continue;
    }
    recorded.state = RequestState::kAcknowledged;
    recorded.acknowledged_at = request.context.observation;
    recorded.acknowledgement_source = request.acknowledging_system;
    break;
  }
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kRecordAcknowledgement,
                          request.context.observation, next.commit_sequence, milliseconds, limits,
                          "acknowledged by " + request.acknowledging_system);

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<RecordAcknowledgementOutcome>{published.error()};
  }
  RecordAcknowledgementOutcome outcome;
  for (const auto& recorded : detail::find_plan(impl_->state, request.context.plan)->requests) {
    if (recorded.id == request.request) {
      outcome.request = recorded;
      break;
    }
  }
  return Result<RecordAcknowledgementOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// ingest_completion
// ---------------------------------------------------------------------------

Result<IngestCompletionOutcome> Coordinator::ingest_completion(const IngestCompletionRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<IngestCompletionOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kIngestCompletion);
  if (!admissible.ok()) {
    return Result<IngestCompletionOutcome>{admissible.error()};
  }

  const Status domain_known = require_known_domain(request.domain);
  if (!domain_known.ok()) {
    return Result<IngestCompletionOutcome>{domain_known.error()};
  }
  if (!is_known(request.state)) {
    return make_error<IngestCompletionOutcome>(ErrorCode::kInvalidEnumValue,
                                               "the completion state is outside the defined states");
  }

  CompletionEvidence evidence;
  evidence.id = detail::evidence_id_for(request.context.plan, request.domain, request.generation);
  evidence.domain = request.domain;
  evidence.state = request.state;
  evidence.generation = request.generation;
  evidence.observed_at = request.observed_at;
  evidence.payload_digest = request.payload_digest;
  evidence.manifest_digest = request.manifest_digest;
  evidence.scope_manifest_digest = request.scope_manifest_digest;
  evidence.generations = request.generations;
  evidence.residual_count_known = request.residual_count_known;
  evidence.residual_count = request.residual_count;
  evidence.source = request.source;
  evidence.annotation = request.annotation;
  evidence.observed_at_milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  const Status valid = evidence.validate(limits);
  if (!valid.ok()) {
    return Result<IngestCompletionOutcome>{valid.error()};
  }
  if (evidence.generation <= detail::latest_evidence_generation(*current, request.domain)) {
    return make_error<IngestCompletionOutcome>(
        ErrorCode::kStaleEvidence,
        "evidence generation " + to_string(evidence.generation) +
            " does not advance the newest generation " +
            to_string(detail::latest_evidence_generation(*current, request.domain)) + " for this domain");
  }


  std::vector<ResidualEntry> residuals = request.residuals;
  for (auto& entry : residuals) {
    entry.domain = request.domain;
    entry.state = ResidualState::kOpen;
    entry.recorded_at = request.context.observation;
    if (entry.detail.empty()) {
      entry.detail = "reported by " + request.source;
    }
    const Status detail = detail::validate_text(entry.detail, "residual detail", limits.max_annotation_bytes);
    if (!detail.ok()) {
      return Result<IngestCompletionOutcome>{detail.error()};
    }
  }
  const bool reveals_obligations = evidence.residual_count_known && evidence.residual_count > 0;

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  IngestCompletionOutcome outcome;
  outcome.compatible = detail::completion_compatibility(*plan, evidence) == ErrorCode::kOk;
  outcome.rejection = detail::completion_compatibility(*plan, evidence);

  // Any new evidence recorded while an answer is live withdraws that answer:
  // the answer was about the evidence set that existed when it was given.
  outcome.fenced = withdraw_authority(
      *plan, reveals_obligations ? FenceReason::kNewObligation : FenceReason::kEvidenceSuperseded,
      request.context.observation, next.control_epoch, next.commit_sequence, milliseconds,
      reveals_obligations ? std::string{"new obligations were reported by "} + request.source
                          : std::string{"new completion evidence was recorded for "} +
                                std::string{to_token(request.domain)});
  if (outcome.fenced) {
    outcome.fence = plan->fence;
  }

  for (auto& entry : residuals) {
    if (detail::upsert_residual(*plan, entry, limits)) {
      outcome.fenced = true;
    }
    ++outcome.residuals_recorded;
  }

  plan->completions[domain_index(request.domain)].push_back(evidence);
  trim_records(plan->completions[domain_index(request.domain)], limits.max_evidence_per_domain);
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;

  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kIngestCompletion,
                          request.context.observation, next.commit_sequence, milliseconds, limits,
                          std::string{to_token(request.domain)} + " completion " +
                              std::string{to_token(request.state)});

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<IngestCompletionOutcome>{published.error()};
  }
  outcome.evidence = evidence;
  return Result<IngestCompletionOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// record_residual
// ---------------------------------------------------------------------------

Result<RecordResidualOutcome> Coordinator::record_residual(const RecordResidualRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<RecordResidualOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kRecordResidual);
  if (!admissible.ok()) {
    return Result<RecordResidualOutcome>{admissible.error()};
  }
  const Status domain_known = require_known_domain(request.entry.domain);
  if (!domain_known.ok()) {
    return Result<RecordResidualOutcome>{domain_known.error()};
  }
  if (!is_known(request.entry.kind)) {
    return make_error<RecordResidualOutcome>(ErrorCode::kInvalidEnumValue,
                                             "the residual kind is outside the defined kinds");
  }

  ResidualEntry entry = request.entry;
  entry.state = ResidualState::kOpen;
  entry.recorded_at = request.context.observation;
  entry.resolved_at = ObservationSequence{};
  entry.resolution_evidence_generation = EvidenceGeneration{};
  const Status detail = detail::validate_text(entry.detail, "residual detail", limits.max_annotation_bytes);
  if (!detail.ok()) {
    return Result<RecordResidualOutcome>{detail.error()};
  }
  if (entry.recorded_at.value() == 0) {
    return make_error<RecordResidualOutcome>(ErrorCode::kMissingRequiredField,
                                             "the residual observation sequence is zero");
  }
  if (current->residuals.entries.size() >= limits.max_residuals_per_plan) {
    const bool already_known = std::find(current->residuals.entries.begin(), current->residuals.entries.end(),
                                         entry) != current->residuals.entries.end();
    if (!already_known) {
      return make_error<RecordResidualOutcome>(
          ErrorCode::kTooManyEntries, "the plan already holds the configured maximum of residual entries");
    }
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  const bool reopened = detail::upsert_residual(*plan, entry, limits);
  // An obligation that was resolved and is now present again is a new
  // obligation appearing, which is exactly what withdraws removal authority.
  const bool withdrawn = withdraw_authority(
      *plan, reopened ? FenceReason::kNewObligation : FenceReason::kEvidenceSuperseded,
      request.context.observation, next.control_epoch, next.commit_sequence, milliseconds,
      "a residual entry was recorded for " + std::string{to_token(entry.domain)});
  RecordResidualOutcome outcome;
  outcome.entry = entry;
  outcome.fenced = reopened || withdrawn;
  if (outcome.fenced) {
    outcome.fence = plan->fence;
  }

  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kRecordResidual, request.context.observation,
                          next.commit_sequence, milliseconds, limits,
                          std::string{"residual "} + std::string{to_token(entry.kind)});

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<RecordResidualOutcome>{published.error()};
  }
  return Result<RecordResidualOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// resolve_residual
// ---------------------------------------------------------------------------

Result<ResolveResidualOutcome> Coordinator::resolve_residual(const ResolveResidualRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<ResolveResidualOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kResolveResidual);
  if (!admissible.ok()) {
    return Result<ResolveResidualOutcome>{admissible.error()};
  }
  const Status domain_known = require_known_domain(request.domain);
  if (!domain_known.ok()) {
    return Result<ResolveResidualOutcome>{domain_known.error()};
  }
  if (!is_known(request.kind)) {
    return make_error<ResolveResidualOutcome>(ErrorCode::kInvalidEnumValue,
                                              "the residual kind is outside the defined kinds");
  }
  if (request.resolution_evidence_generation.value() == 0) {
    return make_error<ResolveResidualOutcome>(
        ErrorCode::kMissingRequiredField, "a resolution must name the evidence generation that resolved it");
  }
  const Status detail = detail::validate_text(request.detail, "resolution detail", limits.max_annotation_bytes);
  if (!detail.ok()) {
    return Result<ResolveResidualOutcome>{detail.error()};
  }

  ResidualEntry probe;
  probe.obligation = request.obligation;
  probe.domain = request.domain;
  probe.kind = request.kind;
  const auto found = std::find(current->residuals.entries.begin(), current->residuals.entries.end(), probe);
  if (found == current->residuals.entries.end()) {
    return make_error<ResolveResidualOutcome>(
        ErrorCode::kObligationNotDeclared,
        "no residual entry exists for " + std::string{to_token(request.domain)} + " obligation " +
            to_string(request.obligation) + " of kind " + std::string{to_token(request.kind)});
  }
  if (found->state == ResidualState::kRelinquished) {
    // Resolving an already resolved entry repeats a statement that is already
    // recorded, so it is accepted and changes nothing.
    ResolveResidualOutcome outcome;
    outcome.entry = *found;
    return Result<ResolveResidualOutcome>{std::move(outcome)};
  }
  // A resolution may only cite evidence the coordinator has actually recorded.
  // Citing a generation nobody produced would make the relinquishment an
  // assertion rather than a proof.
  if (request.resolution_evidence_generation > detail::latest_evidence_generation(*current, request.domain)) {
    return make_error<ResolveResidualOutcome>(
        ErrorCode::kInvalidGenerationOrder,
        "the resolution names evidence generation " +
            to_string(request.resolution_evidence_generation) +
            " but the newest recorded generation for this domain is " +
            to_string(detail::latest_evidence_generation(*current, request.domain)));
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  ResolveResidualOutcome outcome;
  for (auto& entry : plan->residuals.entries) {
    if (!(entry == probe)) {
      continue;
    }
    entry.state = ResidualState::kRelinquished;
    entry.resolved_at = request.context.observation;
    entry.resolution_evidence_generation = request.resolution_evidence_generation;
    if (!request.detail.empty()) {
      entry.detail = request.detail;
    }
    outcome.entry = entry;
    break;
  }
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kResolveResidual, request.context.observation,
                          next.commit_sequence, milliseconds, limits,
                          "residual relinquished: " + std::string{to_token(request.domain)});

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<ResolveResidualOutcome>{published.error()};
  }
  return Result<ResolveResidualOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// grant_safe_to_remove
// ---------------------------------------------------------------------------

Result<GrantSafeToRemoveOutcome> Coordinator::grant_safe_to_remove(const GrantSafeToRemoveRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<GrantSafeToRemoveOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kGrant);
  if (!admissible.ok()) {
    return Result<GrantSafeToRemoveOutcome>{admissible.error()};
  }
  const Status granted_by = detail::validate_text(request.granted_by, "granted_by", limits.max_text_bytes);
  if (!granted_by.ok()) {
    return Result<GrantSafeToRemoveOutcome>{granted_by.error()};
  }

  // The grant re-evaluates under the lock at the current revision, so a caller
  // can never grant on the strength of an evaluation that has since stopped
  // being true.
  const DrainPlanSnapshot view = detail::build_plan_snapshot(*current, impl_->state.control_epoch);
  const SafeToRemoveEvaluation evaluation =
      detail::evaluate_safe_to_remove(view, impl_->state.control_epoch);
  if (evaluation.verdict != SafeToRemoveVerdict::kGranted) {
    return Result<GrantSafeToRemoveOutcome>{
        Error{evaluation.primary_blocking_code, evaluation.explanation}};
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  SafeToRemoveGrant grant;
  grant.plan = plan->spec.id;
  grant.revision = plan->spec.revision;
  grant.epoch = next.control_epoch;
  grant.generations = plan->spec.bindings.generations;
  grant.evidence_digest = evaluation.evidence_digest;
  grant.manifest_digest = evaluation.manifest_digest;
  grant.observation_floor = request.context.observation;
  grant.granted_commit = next.commit_sequence;
  grant.granted_at_milliseconds = milliseconds;
  grant.granted_by = request.granted_by;
  plan->grant = grant;

  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kGrant, request.context.observation,
                          next.commit_sequence, milliseconds, limits, "safe to remove granted");

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<GrantSafeToRemoveOutcome>{published.error()};
  }

  GrantSafeToRemoveOutcome outcome;
  outcome.grant = detail::find_plan(impl_->state, request.context.plan)->grant;
  const DrainPlanSnapshot committed =
      detail::build_plan_snapshot(*detail::find_plan(impl_->state, request.context.plan),
                                  impl_->state.control_epoch);
  outcome.evaluation = detail::evaluate_safe_to_remove(committed, impl_->state.control_epoch);
  return Result<GrantSafeToRemoveOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// fence_safe_to_remove
// ---------------------------------------------------------------------------

Result<FenceSafeToRemoveOutcome> Coordinator::fence_safe_to_remove(const FenceSafeToRemoveRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<FenceSafeToRemoveOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kFence);
  if (!admissible.ok()) {
    return Result<FenceSafeToRemoveOutcome>{admissible.error()};
  }
  if (!is_known(request.reason)) {
    return make_error<FenceSafeToRemoveOutcome>(ErrorCode::kInvalidEnumValue,
                                                "the fence reason is outside the defined reasons");
  }
  if (request.reason == FenceReason::kNone || request.reason == FenceReason::kPlanCancelled ||
      request.reason == FenceReason::kPlanFailed || request.reason == FenceReason::kRestart ||
      request.reason == FenceReason::kControlEpochChanged) {
    return make_error<FenceSafeToRemoveOutcome>(
        ErrorCode::kInvalidEnumValue,
        "an operator fence must state one of the operator-visible reasons");
  }
  const Status detail = detail::validate_text(request.detail, "fence detail", limits.max_annotation_bytes);
  if (!detail.ok()) {
    return Result<FenceSafeToRemoveOutcome>{detail.error()};
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  FenceSafeToRemoveOutcome outcome;
  outcome.had_live_grant = detail::grant_is_live(*plan, next.control_epoch);
  const ObservationSequence floor = advanced_floor(*plan, request.reason, request.context.observation);
  plan->grant.reset();
  detail::apply_fence(*plan, request.reason, floor, plan->spec.revision, next.control_epoch, next.commit_sequence,
                      milliseconds, request.detail);
  outcome.fence = plan->fence.value();

  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kFence, request.context.observation,
                          next.commit_sequence, milliseconds, limits,
                          std::string{"fenced: "} + std::string{to_token(request.reason)});

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<FenceSafeToRemoveOutcome>{published.error()};
  }
  return Result<FenceSafeToRemoveOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// cancel_plan and fail_plan
// ---------------------------------------------------------------------------

Result<CancelPlanOutcome> Coordinator::cancel_plan(const CancelPlanRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<CancelPlanOutcome>{context.error()};
  }
  const Status detail = detail::validate_text(request.reason, "cancellation reason", limits.max_annotation_bytes);
  if (!detail.ok()) {
    return Result<CancelPlanOutcome>{detail.error()};
  }
  if (current->cancelled) {
    // Cancelling again repeats a decision that is already recorded.
    CancelPlanOutcome outcome;
    outcome.plan = committed_plan(*impl_, request.context.plan);
    return Result<CancelPlanOutcome>{std::move(outcome)};
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  plan->cancelled = true;
  plan->cancellation_detail = request.reason;
  plan->grant.reset();
  std::uint32_t cancelled = 0;
  for (auto& recorded : plan->requests) {
    if (is_request_outstanding(recorded.state) || recorded.state == RequestState::kAcknowledged) {
      recorded.state = RequestState::kCancelled;
      recorded.settled_at = request.context.observation;
      recorded.settlement_detail = "plan cancelled: " + request.reason;
      ++cancelled;
    }
  }
  const ObservationSequence floor = advanced_floor(*plan, FenceReason::kPlanCancelled, request.context.observation);
  detail::apply_fence(*plan, FenceReason::kPlanCancelled, floor, plan->spec.revision, next.control_epoch,
                      next.commit_sequence, milliseconds, request.reason);
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;

  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kCancel, request.context.observation,
                          next.commit_sequence, milliseconds, limits, "plan cancelled");

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<CancelPlanOutcome>{published.error()};
  }
  CancelPlanOutcome outcome;
  outcome.plan = committed_plan(*impl_, request.context.plan);
  outcome.requests_cancelled = cancelled;
  outcome.fenced = true;
  return Result<CancelPlanOutcome>{std::move(outcome)};
}

Result<FailPlanOutcome> Coordinator::fail_plan(const FailPlanRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<FailPlanOutcome>{context.error()};
  }
  const Status detail = detail::validate_text(request.reason, "failure reason", limits.max_annotation_bytes);
  if (!detail.ok()) {
    return Result<FailPlanOutcome>{detail.error()};
  }
  if (current->cancelled) {
    return make_error<FailPlanOutcome>(ErrorCode::kPlanCancelled,
                                       "a cancelled plan cannot be marked failed");
  }
  if (current->failed) {
    FailPlanOutcome outcome;
    outcome.plan = committed_plan(*impl_, request.context.plan);
    return Result<FailPlanOutcome>{std::move(outcome)};
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  plan->failed = true;
  plan->failure_detail = request.reason;
  plan->grant.reset();
  for (auto& recorded : plan->requests) {
    if (is_request_outstanding(recorded.state) || recorded.state == RequestState::kAcknowledged) {
      recorded.state = RequestState::kFailed;
      recorded.settled_at = request.context.observation;
      recorded.settlement_detail = "plan failed: " + request.reason;
    }
  }
  const ObservationSequence floor = advanced_floor(*plan, FenceReason::kPlanFailed, request.context.observation);
  detail::apply_fence(*plan, FenceReason::kPlanFailed, floor, plan->spec.revision, next.control_epoch,
                      next.commit_sequence, milliseconds, request.reason);
  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;

  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kFail, request.context.observation,
                          next.commit_sequence, milliseconds, limits, "plan failed");

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<FailPlanOutcome>{published.error()};
  }
  FailPlanOutcome outcome;
  outcome.plan = committed_plan(*impl_, request.context.plan);
  outcome.fenced = true;
  return Result<FailPlanOutcome>{std::move(outcome)};
}

// ---------------------------------------------------------------------------
// supersede_request
// ---------------------------------------------------------------------------

Result<SupersedeRequestOutcome> Coordinator::supersede_request(const SupersedeRequestRequest& request) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Limits& limits = impl_->state.limits;

  const PlanRecord* current = detail::find_plan(impl_->state, request.context.plan);
  const Status context = detail::validate_context(*impl_, request.context, current, true, limits);
  if (!context.ok()) {
    return Result<SupersedeRequestOutcome>{context.error()};
  }
  const DrainState state = detail::derive_state(*current, impl_->state.control_epoch);
  const Status admissible = check_plan_operation(state, PlanOperation::kSupersedeRequest);
  if (!admissible.ok()) {
    return Result<SupersedeRequestOutcome>{admissible.error()};
  }
  const DrainRequest* found = nullptr;
  for (const auto& recorded : current->requests) {
    if (recorded.id == request.request) {
      found = &recorded;
      break;
    }
  }
  if (found == nullptr) {
    return make_error<SupersedeRequestOutcome>(
        ErrorCode::kRequestNotFound, "request " + to_string(request.request) + " is not recorded on this plan");
  }
  if (found->state == RequestState::kSuperseded) {
    return make_error<SupersedeRequestOutcome>(ErrorCode::kRequestSuperseded,
                                               "the request was already replaced by a later attempt");
  }
  if (found->state == RequestState::kCancelled) {
    return make_error<SupersedeRequestOutcome>(ErrorCode::kInvalidStateTransition,
                                               "a cancelled request cannot be superseded");
  }
  if (request.reason.empty()) {
    return make_error<SupersedeRequestOutcome>(ErrorCode::kMissingRequiredField,
                                               "superseding an externally consequential request requires a reason");
  }
  const Status detail = detail::validate_text(request.reason, "supersede reason", limits.max_annotation_bytes);
  if (!detail.ok()) {
    return Result<SupersedeRequestOutcome>{detail.error()};
  }
  if (found->key.attempt.value() + 1U > limits.max_attempts_per_key) {
    return make_error<SupersedeRequestOutcome>(
        ErrorCode::kCounterExhausted, "the attempt counter for this request key has reached its bound");
  }

  CoordinatorState next = impl_->state;
  next.commit_sequence = detail::next_sequence(impl_->state);
  next.observation_sequence = request.context.observation > next.observation_sequence
                                  ? request.context.observation
                                  : next.observation_sequence;
  PlanRecord* plan = detail::find_plan(next, request.context.plan);
  const DrainState previous = detail::derive_state(*plan, next.control_epoch);
  const std::int64_t milliseconds =
      detail::resolve_milliseconds(impl_->clock, request.context.requested_at_milliseconds);

  SupersedeRequestOutcome outcome;
  DrainRequest replacement;
  for (auto& recorded : plan->requests) {
    if (recorded.id != request.request) {
      continue;
    }
    recorded.state = RequestState::kSuperseded;
    recorded.settled_at = request.context.observation;
    recorded.settlement_detail = request.reason;
    outcome.superseded = recorded;

    replacement = recorded;
    replacement.key.attempt = AttemptId{recorded.key.attempt.value() + 1U};
    replacement.id = request_id_for(replacement.key);
    replacement.idempotency_key = request_idempotency_key(replacement.key);
    replacement.state = RequestState::kStaged;
    replacement.staged_at = request.context.observation;
    replacement.issued_at = ObservationSequence{};
    replacement.acknowledged_at = ObservationSequence{};
    replacement.acknowledgement_source.clear();
    replacement.settled_at = ObservationSequence{};
    replacement.settlement_detail.clear();
    replacement.staged_at_milliseconds = milliseconds;
    replacement.issued_at_milliseconds = 0;
    break;
  }
  const Status valid = replacement.validate(limits);
  if (!valid.ok()) {
    return Result<SupersedeRequestOutcome>{valid.error()};
  }
  plan->requests.push_back(replacement);
  std::sort(plan->requests.begin(), plan->requests.end());
  outcome.replacement = replacement;

  plan->last_observation = request.context.observation;
  plan->last_commit = next.commit_sequence;
  const DrainState derived = detail::derive_state(*plan, next.control_epoch);
  detail::note_transition(*plan, previous, derived, PlanOperation::kSupersedeRequest, request.context.observation,
                          next.commit_sequence, milliseconds, limits,
                          "request superseded by attempt " + to_string(replacement.key.attempt));

  const Status published = detail::publish_locked(*impl_, std::move(next));
  if (!published.ok()) {
    return Result<SupersedeRequestOutcome>{published.error()};
  }
  return Result<SupersedeRequestOutcome>{std::move(outcome)};
}

}  // namespace facilitydrain
