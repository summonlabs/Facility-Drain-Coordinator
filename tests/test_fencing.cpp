// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Fencing: the floor. A verdict is only ever valid for evidence observed after
// the last thing that changed the world. The floor is what makes "new
// obligations appearing during a drain fence the prior safe to remove answer"
// a rule the system enforces rather than a sentence in a document.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::generations;
using fdc_test::residual;

namespace {

constexpr PlanId kPlan{71};

DomainMask asi_and_dfi() {
  return DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
}

Scaffold granted_scaffold() {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());
  scaffold.grant(kPlan);
  return scaffold;
}

}  // namespace

FDC_TEST(fencing, an_operator_fence_moves_the_floor_past_every_recorded_observation) {
  Scaffold scaffold = granted_scaffold();
  const std::uint64_t before = scaffold.plan(kPlan).last_observation.value();

  FenceSafeToRemoveRequest fence;
  fence.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  fence.reason = FenceReason::kOperatorFence;
  fence.detail = "window withdrawn";
  auto outcome = scaffold.coordinator().fence_safe_to_remove(fence);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK(outcome.value().fence.floor.value() > before);
  FDC_CHECK_EQ(outcome.value().fence.floor.value(), fence.context.observation.value());

  // Everything recorded before the operator spoke is now unable to support a
  // new answer, including the evidence that made the plan safe to remove.
  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code == ErrorCode::kStaleEvidence);
}

FDC_TEST(fencing, the_floor_only_ever_moves_forward) {
  Scaffold scaffold = granted_scaffold();

  FenceSafeToRemoveRequest first;
  first.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  first.reason = FenceReason::kOperatorFence;
  first.detail = "first";
  auto applied = scaffold.coordinator().fence_safe_to_remove(first);
  FDC_REQUIRE_OK(applied);
  const ObservationSequence high = applied.value().fence.floor;

  // An older observation cannot lower it: a fence that could be undone by
  // supplying an older number would not be a fence at all.
  FenceSafeToRemoveRequest second;
  second.context = first.context;
  second.context.observation = ObservationSequence{first.context.observation.value() + 1U};
  second.reason = FenceReason::kEvidenceSuperseded;
  second.detail = "second";
  auto again = scaffold.coordinator().fence_safe_to_remove(second);
  FDC_REQUIRE_OK(again);
  FDC_CHECK(again.value().fence.floor.value() >= high.value());
  FDC_CHECK(scaffold.plan(kPlan).fence.has_value());
  FDC_CHECK(scaffold.plan(kPlan).fence->floor.value() >= high.value());
}

FDC_TEST(fencing, a_restart_fence_reason_is_not_available_to_a_caller) {
  // A restart is something that happens to a coordinator, not something a
  // caller can claim: a caller that could name it could make its own evidence
  // look newer than it is.
  Scaffold scaffold = granted_scaffold();
  FenceSafeToRemoveRequest fence;
  fence.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  fence.reason = FenceReason::kControlEpochChanged;
  fence.detail = "pretending to be a restart";
  auto outcome = scaffold.coordinator().fence_safe_to_remove(fence);
  FDC_CHECK_CODE(outcome, ErrorCode::kInvalidEnumValue);
}

FDC_TEST(fencing, a_revision_that_changes_the_membership_invalidates_earlier_evidence) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());
  FDC_CHECK(scaffold.evaluate(kPlan).verdict == SafeToRemoveVerdict::kGranted);

  // The rack gained a member. Evidence taken against the old membership is
  // evidence about a different physical world.
  RevisePlanRequest revise;
  revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  revise.targets = std::vector<DrainScope>{DrainScope{ScopeKind::kRack, 9}, DrainScope{ScopeKind::kAsset, 77}};
  revise.reason = FenceReason::kScopeManifestChanged;
  revise.detail = "asset 77 was added to the rack";
  auto outcome = scaffold.coordinator().revise_plan(revise);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK(outcome.value().plan.fence.has_value());

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code == ErrorCode::kStaleEvidence);
}

FDC_TEST(fencing, a_bookkeeping_revision_that_changes_nothing_physical_does_not_invalidate_evidence) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());
  const ObservationSequence floor_before =
      scaffold.plan(kPlan).fence.has_value() ? scaffold.plan(kPlan).fence->floor : ObservationSequence{};

  RevisePlanRequest revise;
  revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  revise.reason = FenceReason::kPlanRevised;
  revise.detail = "adopted a new policy generation";
  revise.generations = generations(900);
  auto outcome = scaffold.coordinator().revise_plan(revise);
  FDC_REQUIRE_OK(outcome);

  const DrainPlanSnapshot plan = scaffold.plan(kPlan);
  FDC_REQUIRE(plan.fence.has_value());
  FDC_CHECK_EQ(plan.fence->floor.value(), floor_before.value());
  // The generation set moved, so the old reports are incompatible on content
  // rather than stale by ordering. That is a different, equally firm refusal.
  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code ==
            ErrorCode::kGenerationIncompatible);
}

FDC_TEST(fencing, a_residual_recorded_after_a_grant_withdraws_it_as_a_new_obligation) {
  Scaffold scaffold = granted_scaffold();
  const RecordResidualOutcome recorded =
      scaffold.record_residual(kPlan, residual(OwnerDomain::kAsi, 4242, ResidualKind::kObligationActive));
  FDC_CHECK(recorded.fenced);
  FDC_REQUIRE(recorded.fence.has_value());
  FDC_CHECK(recorded.fence->reason == FenceReason::kEvidenceSuperseded);
  FDC_CHECK(!scaffold.plan(kPlan).grant.has_value());
  FDC_CHECK(scaffold.evaluate(kPlan).verdict == SafeToRemoveVerdict::kDenied);
}

FDC_TEST(fencing, a_plan_that_never_had_an_answer_can_still_be_fenced) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});

  FenceSafeToRemoveRequest fence;
  fence.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  fence.reason = FenceReason::kDependencyChange;
  fence.detail = "the dependency graph was rebuilt";
  auto outcome = scaffold.coordinator().fence_safe_to_remove(fence);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK(!outcome.value().had_live_grant);
  FDC_CHECK(scaffold.plan(kPlan).fence.has_value());
  FDC_CHECK_EQ(scaffold.plan(kPlan).fence->reason, FenceReason::kDependencyChange);
}
