// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Durability: what a durable store actually guarantees. A mutation is either
// published in full or the previous generation is still the truth; reopening
// adopts exactly one generation; and nothing about the layout invites a reader
// to guess.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{31};

DomainMask asi_and_dfi() {
  return DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
}

}  // namespace

FDC_TEST(persistence, a_durable_store_reopens_with_the_same_facts) {
  TempDir dir{"persistence-reopen"};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    FDC_CHECK(scaffold.coordinator().durable());
    FDC_CHECK(scaffold.coordinator().recovery().created_new_store);
    scaffold.create_plan(kPlan, {workload(1001)});
    scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
    FDC_CHECK(scaffold.coordinator().recovery().current_epoch.value() == 1U);
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  const DrainPlanSnapshot plan = reopened.plan(kPlan);
  FDC_CHECK_EQ(plan.spec.revision.value(), 1U);
  FDC_CHECK_EQ(plan.consumers.size(), 1U);
  FDC_CHECK(plan.enumerations[domain_index(OwnerDomain::kAsi)].present);
  FDC_CHECK(plan.enumerations[domain_index(OwnerDomain::kAsi)].accepted);
  FDC_CHECK(reopened.coordinator().recovery().opened_existing_store);
  FDC_CHECK(!reopened.coordinator().recovery().created_new_store);
  // Recovering is a new writer incarnation under a new control epoch.
  FDC_CHECK_EQ(reopened.coordinator().recovery().previous_epoch.value(), 1U);
  FDC_CHECK_EQ(reopened.coordinator().control_epoch().value(), 2U);
}

FDC_TEST(persistence, every_accepted_mutation_advances_the_commit_sequence) {
  TempDir dir{"persistence-sequence"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 0U);

  scaffold.create_plan(kPlan);
  FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 1U);
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {});
  FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 2U);

  const auto inspection = inspect_store(dir.path(), Limits{});
  FDC_REQUIRE_OK(inspection);
  FDC_CHECK(inspection.value().exists);
  FDC_CHECK(inspection.value().has_pointer);
  FDC_CHECK_EQ(inspection.value().sequence.value(), 2U);
  FDC_CHECK(inspection.value().generation_files >= 1U);
}

FDC_TEST(persistence, a_rejected_mutation_publishes_nothing) {
  TempDir dir{"persistence-rejected"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  scaffold.create_plan(kPlan);
  const CommitSequence before = scaffold.coordinator().commit_sequence();

  // A wrong revision is refused before anything is staged, so the durable
  // sequence must not move at all.
  MutationContext context = scaffold.next(kPlan, Revision{42});
  RecordEnumerationRequest request;
  request.context = context;
  request.domain = OwnerDomain::kAsi;
  request.coverage = CoverageState::kComplete;
  request.generation = EvidenceGeneration{1};
  request.observed_at = context.observation;
  request.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
  request.generations = fdc_test::generations(1);
  auto outcome = scaffold.coordinator().record_enumeration(request);
  FDC_CHECK_CODE(outcome, ErrorCode::kRevisionConflict);
  FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), before.value());

  const auto inspection = inspect_store(dir.path(), Limits{});
  FDC_REQUIRE_OK(inspection);
  FDC_CHECK_EQ(inspection.value().sequence.value(), before.value());
}

FDC_TEST(persistence, retained_generations_stay_bounded) {
  TempDir dir{"persistence-retention"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  scaffold.create_plan(kPlan);
  // Ten accepted mutations, one per plan revision, is more than the retention
  // bound, so old generations have to be pruned rather than accumulated.
  for (std::uint32_t index = 0; index < 10U; ++index) {
    RevisePlanRequest revise;
    revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
    revise.reason = FenceReason::kPlanRevised;
    revise.detail = "retention test";
    auto outcome = scaffold.coordinator().revise_plan(revise);
    FDC_REQUIRE_OK(outcome);
  }
  FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 11U);
  const std::size_t generations = fdc_test::count_files_with_prefix(dir.path(), kGenerationFilePrefix);
  FDC_CHECK(generations <= kRetainedGenerations);
  FDC_CHECK(generations >= 1U);

  // The store still recovers, and it recovers the newest generation.
  const CommitSequence expected = scaffold.coordinator().commit_sequence();
  // The first session must end before a second one opens the store: one writer
  // at a time is the whole point of the lock.
  scaffold = Scaffold::ephemeral();
  Scaffold reopened = Scaffold::durable(dir.path());
  FDC_CHECK_EQ(reopened.coordinator().recovery().recovered_sequence.value(), expected.value());
  FDC_CHECK_EQ(reopened.plan(kPlan).spec.revision.value(), 11U);
}

FDC_TEST(persistence, reopening_with_smaller_limits_is_refused) {
  TempDir dir{"persistence-limits"};
  Limits generous;
  generous.max_plans = 32;
  {
    Scaffold scaffold = Scaffold::durable(dir.path(), generous);
    scaffold.create_plan(kPlan);
  }

  Limits narrower = generous;
  narrower.max_plans = 4;
  CoordinatorOpenRequest request;
  request.root = dir.path();
  request.limits = narrower;
  auto outcome = Coordinator::open(request);
  FDC_REQUIRE(!outcome);
  FDC_CHECK(outcome.error().code() == ErrorCode::kLimitExceeded);
  FDC_CHECK(outcome.error().detail().find("max_plans") != std::string::npos);

  Limits wider = generous;
  wider.max_plans = 64;
  CoordinatorOpenRequest wider_request;
  wider_request.root = dir.path();
  wider_request.limits = wider;
  auto reopened = Coordinator::open(wider_request);
  FDC_REQUIRE_OK(reopened);
  FDC_CHECK_EQ(reopened.value().limits().max_plans, 64U);
}

FDC_TEST(persistence, opening_a_missing_store_without_creation_is_refused) {
  TempDir dir{"persistence-missing"};
  CoordinatorOpenRequest request;
  request.root = dir.sub("absent");
  request.create_if_missing = false;
  auto outcome = Coordinator::open(request);
  FDC_REQUIRE(!outcome);
  FDC_CHECK(outcome.error().code() == ErrorCode::kStoreNotFound);
}

FDC_TEST(persistence, a_read_only_session_cannot_mutate) {
  TempDir dir{"persistence-readonly"};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan);
  }

  CoordinatorOpenRequest request;
  request.root = dir.path();
  request.read_only = true;
  request.create_if_missing = false;
  auto opened = Coordinator::open(request);
  FDC_REQUIRE_OK(opened);
  Coordinator& reader = opened.value();
  FDC_CHECK_EQ(reader.commit_sequence().value(), 1U);

  CreatePlanRequest create;
  create.context.plan = PlanId{99};
  create.context.expected_revision = Revision{1};
  create.context.incarnation = reader.incarnation();
  create.context.expected_epoch = reader.control_epoch();
  create.context.observation = ObservationSequence{5000};
  create.context.principal = "fdc-tests";
  create.id = PlanId{99};
  create.scope = DrainScope{ScopeKind::kRack, 9};
  create.targets = {DrainScope{ScopeKind::kRack, 9}};
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  create.generations = fdc_test::generations(1);
  create.policy_id = PolicyId{7};
  create.policy_digest = digest_text("policy");
  auto rejected = reader.create_plan(create);
  FDC_REQUIRE(!rejected);
  FDC_CHECK(rejected.error().code() == ErrorCode::kReadOnlyStore);
  FDC_CHECK_EQ(reader.commit_sequence().value(), 1U);
}

FDC_TEST(persistence, a_store_holding_many_plans_round_trips_exactly) {
  TempDir dir{"persistence-many"};
  std::vector<PlanId> plans;
  std::vector<std::string> digests_before;
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    for (std::uint32_t index = 0; index < 40U; ++index) {
      const PlanId id{100 + index};
      plans.push_back(id);
      scaffold.create_plan(id, {workload(1000 + index)}, {DrainScope{ScopeKind::kAsset, 500 + index}},
                           DomainMask::of(OwnerDomain::kAsi));
    }
    for (const PlanId id : plans) {
      scaffold.enumerate(id, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1000 + (id.value() - 100))});
    }
    // The snapshot is materialised before it is iterated: binding a range to a
    // member of a temporary does not extend that temporary's lifetime.
    const CoordinatorSnapshot before = scaffold.coordinator().snapshot().value();
    for (const DrainPlanSnapshot& plan : before.plans) {
      digests_before.push_back(plan.plan_digest.to_hex());
    }
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  const CoordinatorSnapshot snapshot = reopened.coordinator().snapshot().value();
  FDC_CHECK_EQ(snapshot.plans.size(), 40U);
  for (std::size_t index = 0; index < snapshot.plans.size(); ++index) {
    FDC_CHECK(snapshot.plans[index].spec.id == plans[index]);
    FDC_CHECK(snapshot.plans[index].enumerations[domain_index(OwnerDomain::kAsi)].present);
  }
  // Each plan's canonical digest is computed from that plan's recorded facts, so
  // it is the same before and after the restart: recovery restores facts, not
  // conclusions. The coordinator level digest is deliberately not compared: it
  // covers the control epoch and the commit sequence, and a restart moves both.
  FDC_REQUIRE(snapshot.plans.size() == digests_before.size());
  for (std::size_t index = 0; index < snapshot.plans.size(); ++index) {
    FDC_CHECK_EQ(snapshot.plans[index].plan_digest.to_hex(), digests_before[index]);
  }
}
