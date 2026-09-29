// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Authority: who may act, against which exact revision, epoch and observation,
// and what happens the moment any of them moves. The tests here exist to prove
// that removal authority is never inherited, never carried across a revision,
// and never survives evidence that invalidates it.

#include "fdc_test_support.hpp"

#include <string>
#include <utility>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::generations;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{21};

DomainMask asi_and_dfi() {
  return DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
}

/// A plan proven safe to remove, ready for the authority tests to attack.
Scaffold granted_scaffold() {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());
  scaffold.grant(kPlan);
  return scaffold;
}

}  // namespace

FDC_TEST(authority, a_stale_incarnation_is_refused) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);

  MutationContext context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  context.incarnation = IncarnationId{context.incarnation.value() + 1U};
  IssueRequestsRequest request;
  request.context = context;
  request.domains = DomainMask::of(OwnerDomain::kAsi);
  auto outcome = scaffold.coordinator().issue_requests(request);
  FDC_CHECK_CODE(outcome, ErrorCode::kStaleAuthority);
}

FDC_TEST(authority, a_stale_control_epoch_is_refused) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);

  MutationContext context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  context.expected_epoch = ControlEpoch{context.expected_epoch.value() + 1U};
  RecordEnumerationRequest request;
  request.context = context;
  request.domain = OwnerDomain::kAsi;
  request.coverage = CoverageState::kComplete;
  request.generation = EvidenceGeneration{1};
  request.observed_at = context.observation;
  request.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
  request.generations = generations(1);
  auto outcome = scaffold.coordinator().record_enumeration(request);
  FDC_CHECK_CODE(outcome, ErrorCode::kEpochMismatch);
}

FDC_TEST(authority, a_stale_revision_is_refused) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);

  MutationContext context = scaffold.next(kPlan, Revision{99});
  RecordEnumerationRequest request;
  request.context = context;
  request.domain = OwnerDomain::kAsi;
  request.coverage = CoverageState::kComplete;
  request.generation = EvidenceGeneration{1};
  request.observed_at = context.observation;
  request.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
  request.generations = generations(1);
  auto outcome = scaffold.coordinator().record_enumeration(request);
  FDC_CHECK_CODE(outcome, ErrorCode::kRevisionConflict);
}

FDC_TEST(authority, a_replayed_observation_sequence_is_refused) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});

  // The same observation that was already accepted cannot be applied twice: a
  // replayed intent is not a second observation of the world.
  MutationContext context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  const ObservationSequence replayed = context.observation;
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 2, {});

  context.observation = replayed;
  context.principal = "replay";
  RecordEnumerationRequest request;
  request.context = context;
  request.domain = OwnerDomain::kAsi;
  request.coverage = CoverageState::kComplete;
  request.generation = EvidenceGeneration{3};
  request.observed_at = replayed;
  request.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
  request.generations = generations(1);
  auto outcome = scaffold.coordinator().record_enumeration(request);
  FDC_CHECK_CODE(outcome, ErrorCode::kInvalidGenerationOrder);
}

FDC_TEST(authority, a_mutation_context_without_identity_is_refused) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);

  const MutationContext base = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  IssueRequestsRequest request;
  request.domains = DomainMask::of(OwnerDomain::kAsi);

  // A mutation that does not say which plan, revision, incarnation, epoch and
  // observation it was planned against is not a mutation this system can bind
  // to anything, so each missing part is refused by name.
  request.context = base;
  request.context.plan = PlanId{0};
  FDC_CHECK_CODE(scaffold.coordinator().issue_requests(request), ErrorCode::kMissingRequiredField);

  request.context = base;
  request.context.expected_revision = Revision{0};
  FDC_CHECK_CODE(scaffold.coordinator().issue_requests(request), ErrorCode::kMissingRequiredField);

  request.context = base;
  request.context.incarnation = IncarnationId{0};
  FDC_CHECK_CODE(scaffold.coordinator().issue_requests(request), ErrorCode::kMissingRequiredField);

  request.context = base;
  request.context.expected_epoch = ControlEpoch{0};
  FDC_CHECK_CODE(scaffold.coordinator().issue_requests(request), ErrorCode::kMissingRequiredField);

  request.context = base;
  request.context.observation = ObservationSequence{0};
  FDC_CHECK_CODE(scaffold.coordinator().issue_requests(request), ErrorCode::kMissingRequiredField);
}

FDC_TEST(authority, a_grant_does_not_survive_a_revision) {
  Scaffold scaffold = granted_scaffold();
  FDC_CHECK(scaffold.plan(kPlan).grant_live);

  RevisePlanRequest revise;
  revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  revise.reason = FenceReason::kPlanRevised;
  revise.detail = "the physical membership changed";
  std::vector<DrainScope> targets{DrainScope{ScopeKind::kRack, 9}, DrainScope{ScopeKind::kAsset, 41}};
  revise.targets = targets;
  auto outcome = scaffold.coordinator().revise_plan(revise);
  FDC_REQUIRE_OK(outcome);

  const DrainPlanSnapshot plan = scaffold.plan(kPlan);
  FDC_CHECK_EQ(plan.spec.revision.value(), 2U);
  FDC_CHECK(!plan.grant.has_value());
  FDC_CHECK(!plan.grant_live);
  FDC_CHECK(plan.state != DrainState::kSafeToRemove);
  FDC_REQUIRE(plan.fence.has_value());
  FDC_CHECK(plan.fence->reason == FenceReason::kPlanRevised);
  FDC_CHECK(outcome.value().fenced);
  FDC_CHECK(scaffold.evaluate(kPlan).verdict == SafeToRemoveVerdict::kDenied);
}

FDC_TEST(authority, new_evidence_withdraws_a_live_grant) {
  Scaffold scaffold = granted_scaffold();
  FDC_CHECK(scaffold.plan(kPlan).grant_live);

  // A new enumeration is new evidence about the world. The answer that was
  // given for the old evidence set is no longer an answer at all.
  const RecordEnumerationOutcome enumeration =
      scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 11, {});
  FDC_CHECK(enumeration.fenced);
  FDC_REQUIRE(enumeration.fence.has_value());
  FDC_CHECK(enumeration.fence->reason == FenceReason::kEvidenceSuperseded);

  const DrainPlanSnapshot plan = scaffold.plan(kPlan);
  FDC_CHECK(!plan.grant.has_value());
  FDC_CHECK(!plan.grant_live);
  FDC_CHECK(plan.state != DrainState::kSafeToRemove);

  // Everything recorded before the fence is now on the wrong side of the floor,
  // including the very evidence that caused it: what changes the world cannot
  // also be the proof that the changed world is safe.
  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.fenced);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code == ErrorCode::kStaleEvidence);
}

FDC_TEST(authority, evidence_after_the_floor_can_support_a_new_answer) {
  Scaffold scaffold = granted_scaffold();
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 11, {});
  FDC_CHECK(scaffold.evaluate(kPlan).verdict == SafeToRemoveVerdict::kDenied);

  // Every required domain has to be observed again after the floor moved. ASI
  // moving the floor is not a reason to trust DFI's older report.
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 12, {});
  scaffold.enumerate(kPlan, OwnerDomain::kDfi, CoverageState::kComplete, 13, {});
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kDrained, 14, true, 0);
  scaffold.ingest(kPlan, OwnerDomain::kDfi, CompletionState::kDrained, 15, true, 0);
  FDC_CHECK(scaffold.evaluate(kPlan).verdict == SafeToRemoveVerdict::kGranted);

  // The new answer is a new decision, not the old one restored.
  const SafeToRemoveGrant grant = scaffold.grant(kPlan);
  FDC_CHECK(grant.revision == scaffold.plan(kPlan).spec.revision);
  FDC_CHECK(grant.epoch == scaffold.coordinator().control_epoch());
  FDC_CHECK(scaffold.plan(kPlan).grant_live);
}

FDC_TEST(authority, an_operator_fence_withdraws_authority) {
  Scaffold scaffold = granted_scaffold();

  FenceSafeToRemoveRequest fence;
  fence.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  fence.reason = FenceReason::kOperatorFence;
  fence.detail = "the operator withdrew the removal window";
  auto outcome = scaffold.coordinator().fence_safe_to_remove(fence);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK(outcome.value().had_live_grant);

  const DrainPlanSnapshot plan = scaffold.plan(kPlan);
  FDC_CHECK(!plan.grant_live);
  FDC_REQUIRE(plan.fence.has_value());
  FDC_CHECK(plan.fence->reason == FenceReason::kOperatorFence);
  FDC_CHECK_EQ(plan.fence->detail, std::string{"the operator withdrew the removal window"});
  FDC_CHECK(scaffold.evaluate(kPlan).verdict == SafeToRemoveVerdict::kDenied);
}

FDC_TEST(authority, a_fence_must_state_a_reason_it_may_use) {
  Scaffold scaffold = granted_scaffold();

  FenceSafeToRemoveRequest fence;
  fence.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  fence.reason = FenceReason::kNone;
  fence.detail = "no reason";
  auto none = scaffold.coordinator().fence_safe_to_remove(fence);
  FDC_CHECK_CODE(none, ErrorCode::kInvalidEnumValue);

  fence.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  fence.reason = FenceReason::kRestart;
  auto restart = scaffold.coordinator().fence_safe_to_remove(fence);
  FDC_CHECK_CODE(restart, ErrorCode::kInvalidEnumValue);
}

FDC_TEST(authority, a_denied_grant_reports_the_primary_blocking_code) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan, {workload(1001)}, {DrainScope{ScopeKind::kRack, 9}},
                       DomainMask::of(OwnerDomain::kAsi));

  GrantSafeToRemoveRequest grant;
  grant.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  grant.granted_by = "fdc-tests";
  auto outcome = scaffold.coordinator().grant_safe_to_remove(grant);
  FDC_REQUIRE(!outcome);
  FDC_CHECK(outcome.error().code() == ErrorCode::kIncompleteEnumeration);
  FDC_CHECK(outcome.error().detail().find("domain asi") != std::string::npos);
  FDC_CHECK(!scaffold.plan(kPlan).grant.has_value());
}

FDC_TEST(authority, a_cancelled_plan_refuses_a_grant) {
  Scaffold scaffold = granted_scaffold();

  CancelPlanRequest cancel;
  cancel.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  cancel.reason = "the maintenance window closed";
  auto cancelled = scaffold.coordinator().cancel_plan(cancel);
  FDC_REQUIRE_OK(cancelled);

  GrantSafeToRemoveRequest grant;
  grant.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  grant.granted_by = "fdc-tests";
  auto outcome = scaffold.coordinator().grant_safe_to_remove(grant);
  FDC_CHECK_CODE(outcome, ErrorCode::kPlanCancelled);
}

FDC_TEST(authority, requesting_a_drain_from_a_safe_to_remove_scope_is_refused) {
  Scaffold scaffold = granted_scaffold();

  IssueRequestsRequest request;
  request.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  request.domains = DomainMask::of(OwnerDomain::kAsi);
  auto outcome = scaffold.coordinator().issue_requests(request);
  FDC_CHECK_CODE(outcome, ErrorCode::kInvalidStateTransition);
}
