// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// The verdict matrix. Every rule this system exists to enforce is exercised
// here against real plans and real recorded evidence, including the rules that
// only ever bite when somebody is in a hurry: zero consumers found under
// incomplete coverage, an acknowledgement mistaken for an effect, one domain's
// completion mistaken for another's, and an answer that outlived the evidence
// it was given for.

#include "fdc_test_support.hpp"

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::generations;
using fdc_test::network_path;
using fdc_test::residual;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{11};

DomainMask asi_only() { return DomainMask::of(OwnerDomain::kAsi); }
DomainMask asi_and_dfi() {
  return DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
}

}  // namespace

FDC_TEST(evaluation, denies_until_every_required_domain_is_proven) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);

  const SafeToRemoveEvaluation initial = scaffold.evaluate(kPlan);
  FDC_CHECK(initial.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(initial.primary_blocking_code == ErrorCode::kIncompleteEnumeration);
  FDC_CHECK_EQ(initial.required_domain_count, 2U);
  FDC_CHECK(initial.domains[domain_index(OwnerDomain::kAsi)].verdict == DomainVerdict::kUnknown);
  FDC_CHECK(initial.domains[domain_index(OwnerDomain::kFacility)].verdict == DomainVerdict::kNotRequired);
  FDC_CHECK(!initial.all_required_proven);
}

FDC_TEST(evaluation, zero_consumers_under_partial_coverage_is_not_proof_of_no_consumers) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);

  // The owner looked at part of the scope and found nothing. That is not the
  // same statement as having looked at all of it.
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kPartial, 1, {});
  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].verdict == DomainVerdict::kUnknown);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code ==
            ErrorCode::kIncompleteEnumeration);
  FDC_CHECK(!evaluation.domains[domain_index(OwnerDomain::kAsi)].enumeration_complete);
}

FDC_TEST(evaluation, a_proven_empty_scope_is_granted) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kGranted);
  FDC_CHECK(evaluation.primary_blocking_code == ErrorCode::kOk);
  FDC_CHECK(evaluation.all_required_proven);
  FDC_CHECK(evaluation.enumeration_complete_for_required);
  FDC_CHECK_EQ(evaluation.open_residuals, 0U);
  FDC_CHECK_EQ(evaluation.unknown_residuals, 0U);
  FDC_CHECK(evaluation.blocking_codes.empty());
  FDC_CHECK(scaffold.plan(kPlan).state == DrainState::kDrained);
}

FDC_TEST(evaluation, an_acknowledgement_is_not_an_effect) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan, {workload(1001)}, {DrainScope{ScopeKind::kRack, 9}}, asi_only());
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});

  const IssueRequestsOutcome issued = scaffold.issue(kPlan, asi_only());
  FDC_REQUIRE(!issued.to_deliver.empty());
  scaffold.deliver(kPlan, issued.to_deliver.front().id);
  scaffold.acknowledge(kPlan, issued.to_deliver.front().id);
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kAcknowledged, 2, false, 0);

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.primary_blocking_code == ErrorCode::kAcknowledgementIsNotEffect);
  FDC_CHECK(scaffold.plan(kPlan).state == DrainState::kDraining);
}

FDC_TEST(evaluation, one_domain_does_not_imply_another) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kDrained, 2, true, 0);

  // Drained ASI workloads say nothing at all about DFI paths and attachments.
  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].verdict ==
            DomainVerdict::kProvenComplete);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kDfi)].verdict == DomainVerdict::kUnknown);
  FDC_CHECK(evaluation.primary_blocking_code == ErrorCode::kIncompleteEnumeration);
  FDC_CHECK(!evaluation.all_required_proven);
}

FDC_TEST(evaluation, a_completion_from_a_different_generation_set_cannot_prove_anything) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kDrained, 2, true, 0, {}, generations(500));

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  const DomainAssessment& asi = evaluation.domains[domain_index(OwnerDomain::kAsi)];
  FDC_CHECK(asi.verdict == DomainVerdict::kIncomplete);
  FDC_CHECK(asi.blocking_code == ErrorCode::kGenerationIncompatible);
  FDC_CHECK(!asi.completion_compatible);
  // The report is recorded, not discarded: an operator has to be able to see
  // what the owning system actually said.
  FDC_CHECK(scaffold.plan(kPlan).completions[domain_index(OwnerDomain::kAsi)].present);
}

FDC_TEST(evaluation, an_unknown_residual_count_blocks) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kDrained, 2, false, 0);

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code ==
            ErrorCode::kUnknownResidualCount);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].verdict == DomainVerdict::kUnknown);
}

FDC_TEST(evaluation, a_count_nobody_can_break_down_is_an_unknown) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kDrainedWithResiduals, 2, true, 3);

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  const DomainAssessment& asi = evaluation.domains[domain_index(OwnerDomain::kAsi)];
  FDC_CHECK(asi.blocking_code == ErrorCode::kUnknownObligation);
  FDC_CHECK(asi.verdict == DomainVerdict::kUnknown);
  FDC_CHECK_EQ(asi.residual_count, 3U);
  FDC_CHECK_EQ(asi.known_obligations, 0U);
  // The ledger holds no entries at all here: the owner reported a count and
  // named nothing, which is precisely the unknown the ledger has to preserve.
  FDC_CHECK_EQ(evaluation.unknown_residuals, 0U);
  FDC_CHECK_EQ(evaluation.open_residuals, 0U);
}

FDC_TEST(evaluation, reported_obligations_must_be_relinquished_one_by_one) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan, {workload(1001), workload(1002)}, {DrainScope{ScopeKind::kRack, 9}},
                       asi_only());
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001), workload(1002)});
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kDrainedWithResiduals, 2, true, 2,
                  {residual(OwnerDomain::kAsi, 1001, ResidualKind::kObligationActive),
                   residual(OwnerDomain::kAsi, 1002, ResidualKind::kObligationActive)});

  SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code == ErrorCode::kResidualsPresent);
  FDC_CHECK_EQ(evaluation.open_residuals, 2U);

  scaffold.resolve_residual(kPlan, OwnerDomain::kAsi, 1001, ResidualKind::kObligationActive, 2);
  evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code == ErrorCode::kResidualsPresent);
  FDC_CHECK_EQ(evaluation.open_residuals, 1U);

  scaffold.resolve_residual(kPlan, OwnerDomain::kAsi, 1002, ResidualKind::kObligationActive, 2);
  evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].verdict == DomainVerdict::kProvenComplete);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code == ErrorCode::kOk);
}

FDC_TEST(evaluation, a_protected_obligation_blocks_everything) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.record_residual(kPlan, residual(OwnerDomain::kAsi, 0, ResidualKind::kProtectedObligation));
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kAsi)].blocking_code ==
            ErrorCode::kProtectedObligation);
}

FDC_TEST(evaluation, blocking_codes_are_canonical_and_unique) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  // ASI is fully drained but somebody could not measure one thing, and DFI has
  // only looked at part of its scope. Both are refusals, and the report has to
  // name them in canonical domain order with ASI first.
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});
  scaffold.ingest(kPlan, OwnerDomain::kAsi, CompletionState::kDrained, 2, true, 0);
  scaffold.record_residual(kPlan, residual(OwnerDomain::kAsi, 0, ResidualKind::kEvidenceMissing));
  scaffold.enumerate(kPlan, OwnerDomain::kDfi, CoverageState::kPartial, 3, {});

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_REQUIRE(!evaluation.blocking_codes.empty());
  FDC_CHECK(evaluation.primary_blocking_code == evaluation.blocking_codes.front());
  for (std::size_t left = 0; left < evaluation.blocking_codes.size(); ++left) {
    for (std::size_t right = left + 1; right < evaluation.blocking_codes.size(); ++right) {
      FDC_CHECK(evaluation.blocking_codes[left] != evaluation.blocking_codes[right]);
    }
  }
  // ASI is assessed before DFI, so its code is the primary one.
  FDC_CHECK(evaluation.primary_blocking_code == ErrorCode::kUnknownObligation);
}

FDC_TEST(evaluation, a_cancelled_plan_is_denied_by_its_own_state) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());

  CancelPlanRequest request;
  request.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  request.reason = "operator withdrew the drain";
  auto cancelled = scaffold.coordinator().cancel_plan(request);
  FDC_REQUIRE_OK(cancelled);

  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(evaluation.primary_blocking_code == ErrorCode::kPlanCancelled);
  FDC_CHECK(scaffold.plan(kPlan).state == DrainState::kCancelled);
}

FDC_TEST(evaluation, the_pure_function_and_the_coordinator_agree) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan, {workload(1001), network_path(2002)});
  scaffold.prove_empty_scope(kPlan, DomainMask::none());

  const CoordinatorSnapshot snapshot = scaffold.coordinator().snapshot().value();
  FDC_REQUIRE(snapshot.plans.size() == 1U);
  const SafeToRemoveEvaluation direct =
      evaluate_safe_to_remove(snapshot.plans.front(), scaffold.coordinator().control_epoch());
  const SafeToRemoveEvaluation through_coordinator = scaffold.evaluate(kPlan);
  FDC_CHECK_EQ(direct.to_canonical(), through_coordinator.to_canonical());
  FDC_CHECK_EQ(direct.explanation, through_coordinator.explanation);
  FDC_CHECK(direct.evidence_digest == through_coordinator.evidence_digest);
}

FDC_TEST(evaluation, an_advisory_obligation_does_not_require_its_domain) {
  Scaffold scaffold = Scaffold::ephemeral();
  const ConsumerRecord advisory =
      fdc_test::consumer(ConsumerCategory::kFacilityReservation, 3003, 1, ObligationStrength::kAdvisory);
  scaffold.create_plan(kPlan, {advisory});
  scaffold.prove_empty_scope(kPlan, asi_and_dfi());

  const DrainPlanSnapshot plan = scaffold.plan(kPlan);
  FDC_CHECK(!plan.required_domains.contains(OwnerDomain::kFacility));

  scaffold.record_residual(kPlan, residual(OwnerDomain::kFacility, 3003, ResidualKind::kObligationActive));
  const SafeToRemoveEvaluation evaluation = scaffold.evaluate(kPlan);
  FDC_CHECK(evaluation.domains[domain_index(OwnerDomain::kFacility)].verdict == DomainVerdict::kNotRequired);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kGranted);

  // An unknown in a domain the plan does not require still withholds authority:
  // the plan may not need that domain, but nobody can call the scope safe while
  // an identified factor is unmeasured.
  scaffold.record_residual(kPlan, residual(OwnerDomain::kFacility, 3004, ResidualKind::kObligationUnknown));
  const SafeToRemoveEvaluation fenced = scaffold.evaluate(kPlan);
  FDC_CHECK(fenced.verdict == SafeToRemoveVerdict::kDenied);
  FDC_CHECK(fenced.primary_blocking_code == ErrorCode::kUnknownObligation);
}

FDC_TEST(evaluation, explanation_is_deterministic_and_names_every_domain) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan, {workload(1001)});
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});

  const SafeToRemoveEvaluation first = scaffold.evaluate(kPlan);
  const SafeToRemoveEvaluation second = scaffold.evaluate(kPlan);
  FDC_CHECK_EQ(first.explanation, second.explanation);
  FDC_CHECK(first.explanation.find("domain asi") != std::string::npos);
  FDC_CHECK(first.explanation.find("domain dfi") != std::string::npos);
  FDC_CHECK(first.explanation.find("domain facility") != std::string::npos);
  FDC_CHECK(first.explanation.find("domain monitoring") != std::string::npos);
  FDC_CHECK(first.explanation.find("verdict denied") != std::string::npos);
}
