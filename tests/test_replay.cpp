// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Replay safety. A bounded drain request is externally consequential: the
// owning system will act on it. The coordinator therefore makes one physical
// drain produce exactly one request, no matter how many times the intent is
// submitted, how many restarts happen, or how a plan revision moves underneath
// it. The idempotency key is the whole defence, so these tests attack it from
// every direction they can without a second machine.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{51};

DomainMask asi_only() { return DomainMask::of(OwnerDomain::kAsi); }

/// Stages one request on a durable plan and returns it.
DrainRequest stage_one(Scaffold& scaffold) {
  const IssueRequestsOutcome issued = scaffold.issue(kPlan, asi_only());
  if (issued.to_deliver.size() != 1U) {
    fdc_test::fail_now("expected exactly one request to deliver");
  }
  return issued.to_deliver.front();
}

}  // namespace

FDC_TEST(replay, a_restart_re_offers_the_same_key_rather_than_a_second_drain) {
  TempDir dir{"replay-restart"};
  DrainRequestId first_id{};
  ContentDigest first_key{};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan, {workload(1001)});
    scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
    const DrainRequest staged = stage_one(scaffold);
    first_id = staged.id;
    first_key = staged.idempotency_key;
    // The caller dies here: the record is durable, the delivery is not.
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  const IssueRequestsOutcome issued = reopened.issue(kPlan, asi_only());
  FDC_REQUIRE(issued.to_deliver.size() == 1U);
  FDC_CHECK_EQ(issued.newly_staged, 0U);
  FDC_CHECK_EQ(issued.duplicates, 1U);
  FDC_CHECK(issued.to_deliver.front().id == first_id);
  FDC_CHECK(issued.to_deliver.front().idempotency_key == first_key);

  // Exactly one request exists in the durable state. The restart re-offered an
  // intent; it did not invent a second one.
  FDC_CHECK(reopened.coordinator().requests(kPlan).value().size() == 1U);
  FDC_CHECK(reopened.plan(kPlan).requests.size() == 1U);
  FDC_CHECK(reopened.plan(kPlan).requests.front().state == RequestState::kStaged);
}

FDC_TEST(replay, a_confirmed_delivery_is_never_offered_again) {
  TempDir dir{"replay-delivered"};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan, {workload(1001)});
    scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
    const DrainRequest staged = stage_one(scaffold);
    scaffold.deliver(kPlan, staged.id);
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  const IssueRequestsOutcome issued = reopened.issue(kPlan, asi_only());
  FDC_CHECK(issued.to_deliver.empty());
  FDC_CHECK_EQ(issued.duplicates, 1U);
  FDC_CHECK_EQ(issued.newly_staged, 0U);
  FDC_REQUIRE(issued.items.size() == 1U);
  FDC_CHECK(!issued.items.front().deliver);
  FDC_CHECK(issued.items.front().request.state == RequestState::kIssued);
  FDC_CHECK(reopened.coordinator().requests(kPlan).value().size() == 1U);
}

FDC_TEST(replay, a_plan_revision_does_not_turn_one_drain_into_two) {
  TempDir dir{"replay-revision"};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan, {workload(1001)});
    scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
    const DrainRequest staged = stage_one(scaffold);
    scaffold.deliver(kPlan, staged.id);

    // A revision is bookkeeping inside the coordinator. The obligation set is
    // unchanged, so the physical drain is unchanged, so the key is unchanged.
    RevisePlanRequest revise;
    revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
    revise.reason = FenceReason::kDependencyChange;
    revise.detail = "the dependency graph moved";
    auto outcome = scaffold.coordinator().revise_plan(revise);
    FDC_REQUIRE_OK(outcome);
    FDC_CHECK_EQ(scaffold.plan(kPlan).spec.revision.value(), 2U);

    const IssueRequestsOutcome issued = scaffold.issue(kPlan, asi_only());
    FDC_CHECK(issued.to_deliver.empty());
    FDC_CHECK_EQ(issued.duplicates, 1U);
    FDC_CHECK(issued.items.front().request.idempotency_key == staged.idempotency_key);
    FDC_CHECK(issued.items.front().request.id == staged.id);
    FDC_CHECK(scaffold.coordinator().requests(kPlan).value().size() == 1U);
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  FDC_CHECK(reopened.coordinator().requests(kPlan).value().size() == 1U);
}

FDC_TEST(replay, a_lost_acknowledgement_does_not_produce_a_second_request) {
  TempDir dir{"replay-acknowledgement"};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan, {workload(1001)});
    scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
    const DrainRequest staged = stage_one(scaffold);
    scaffold.deliver(kPlan, staged.id);
    scaffold.acknowledge(kPlan, staged.id);
    FDC_CHECK(scaffold.plan(kPlan).state == DrainState::kDraining);
  }

  // The acknowledgement is durable, so after the restart the coordinator knows
  // the owner has the request and must not ask again.
  Scaffold reopened = Scaffold::durable(dir.path());
  const IssueRequestsOutcome issued = reopened.issue(kPlan, asi_only());
  FDC_CHECK(issued.to_deliver.empty());
  FDC_CHECK_EQ(issued.duplicates, 1U);
  FDC_CHECK(reopened.coordinator().requests(kPlan).value().size() == 1U);
  FDC_CHECK(reopened.plan(kPlan).requests.front().state == RequestState::kAcknowledged);
}

FDC_TEST(replay, only_an_explicit_supersede_starts_a_new_attempt) {
  TempDir dir{"replay-supersede"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  scaffold.create_plan(kPlan, {workload(1001)});
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
  const DrainRequest first = stage_one(scaffold);
  scaffold.deliver(kPlan, first.id);

  SupersedeRequestRequest supersede;
  supersede.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  supersede.request = first.id;
  supersede.reason = "the owning system lost the request";
  auto outcome = scaffold.coordinator().supersede_request(supersede);
  FDC_REQUIRE_OK(outcome);

  const DrainRequest replacement = outcome.value().replacement;
  FDC_CHECK(outcome.value().superseded.state == RequestState::kSuperseded);
  FDC_CHECK(replacement.id != first.id);
  FDC_CHECK(replacement.idempotency_key != first.idempotency_key);
  FDC_CHECK_EQ(replacement.key.attempt.value(), 2U);
  FDC_CHECK(replacement.key.obligation_digest == first.key.obligation_digest);
  FDC_CHECK(replacement.state == RequestState::kStaged);

  // The replacement is the one that is now offered, and it is offered once.
  const IssueRequestsOutcome issued = scaffold.issue(kPlan, asi_only());
  FDC_CHECK(issued.to_deliver.size() == 1U);
  FDC_CHECK(issued.to_deliver.front().id == replacement.id);
  FDC_CHECK(scaffold.coordinator().requests(kPlan).value().size() == 2U);
}

FDC_TEST(replay, the_key_is_the_same_in_two_independent_coordinators) {
  // Two coordinators, two incarnations, two processes in general. The key has to
  // be a function of the request alone, or a replay in another process would be
  // a second physical drain.
  Scaffold first = Scaffold::ephemeral();
  Scaffold second = Scaffold::ephemeral();
  first.create_plan(kPlan, {workload(1001), workload(1002)});
  second.create_plan(kPlan, {workload(1001), workload(1002)});
  first.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001), workload(1002)});
  second.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001), workload(1002)});

  const DrainRequest left = stage_one(first);
  const DrainRequest right = stage_one(second);
  FDC_CHECK(!(first.coordinator().incarnation() == second.coordinator().incarnation()));
  FDC_CHECK(left.id == right.id);
  FDC_CHECK(left.idempotency_key == right.idempotency_key);
  FDC_CHECK_EQ(left.instruction, right.instruction);
  FDC_CHECK(left.key.scope == right.key.scope);
  FDC_CHECK(left.key.obligation_digest == right.key.obligation_digest);
}

FDC_TEST(replay, the_key_changes_when_the_obligations_change) {
  TempDir dir{"replay-obligations"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  scaffold.create_plan(kPlan, {workload(1001)});
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
  const DrainRequest first = stage_one(scaffold);

  // A different obligation set is a different drain, so it must be a different
  // request: the key is bound to exactly what is being drained.
  DrainRequestKey key = first.key;
  key.obligation_digest = digest_text("a different obligation set");
  FDC_CHECK(request_idempotency_key(key) != first.idempotency_key);
  key = first.key;
  key.domain = OwnerDomain::kDfi;
  FDC_CHECK(request_idempotency_key(key) != first.idempotency_key);
  key = first.key;
  key.scope = DrainScope{ScopeKind::kRack, 10};
  FDC_CHECK(request_idempotency_key(key) != first.idempotency_key);
  key = first.key;
  key.attempt = AttemptId{2};
  FDC_CHECK(request_idempotency_key(key) != first.idempotency_key);
  key = first.key;
  key.policy_generation = PolicyGeneration{key.policy_generation.value() + 1U};
  FDC_CHECK(request_idempotency_key(key) != first.idempotency_key);
  FDC_CHECK(request_idempotency_key(first.key) == first.idempotency_key);
  FDC_CHECK_EQ(request_id_for(first.key).value(), first.id.value());
}

FDC_TEST(replay, delivering_twice_is_not_delivering_again) {
  TempDir dir{"replay-deliver-twice"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  scaffold.create_plan(kPlan, {workload(1001)});
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
  const DrainRequest staged = stage_one(scaffold);
  scaffold.deliver(kPlan, staged.id);

  // A caller that loses the response to its own delivery confirmation asks
  // again. That is the same statement, not a new delivery.
  ConfirmDeliveryRequest again;
  again.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  again.request = staged.id;
  again.delivery_reference = "second confirmation";
  auto outcome = scaffold.coordinator().confirm_delivery(again);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK(outcome.value().request.state == RequestState::kIssued);
  FDC_CHECK(outcome.value().request.issued_at.value() > staged.staged_at.value());
  // The second confirmation repeated a statement that was already recorded, so
  // it moved nothing: the recorded delivery instant is still the first one.
  FDC_CHECK(outcome.value().request.issued_at.value() ==
            scaffold.plan(kPlan).requests.front().issued_at.value());
  FDC_CHECK(scaffold.coordinator().requests(kPlan).value().size() == 1U);
}
