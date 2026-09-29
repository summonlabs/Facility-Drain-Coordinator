// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace facilitydrain;

namespace {

constexpr std::int64_t kClockMilliseconds = 1767225600000;
constexpr std::uint64_t kTestIncarnation = 0x5EEDULL;

[[nodiscard]] GenerationSet complete_generations() {
  GenerationSet generations;
  generations.scope = ScopeGeneration{1};
  generations.dependency = DependencyGeneration{2};
  generations.reservation = ReservationGeneration{3};
  generations.obligation = ObligationGeneration{4};
  generations.policy = PolicyGeneration{5};
  generations.topology = TopologyGeneration{6};
  generations.maintenance = MaintenanceGeneration{7};
  generations.capacity = CapacityGeneration{8};
  generations.hardware = HardwareGeneration{9};
  generations.firmware = FirmwareGeneration{10};
  return generations;
}

[[nodiscard]] ClockPtr test_clock() {
  return std::make_shared<const FixedClock>(kClockMilliseconds);
}

[[nodiscard]] Result<Coordinator> open_coordinator(const Limits& limits = Limits{}) {
  EphemeralOptions options;
  options.limits = limits;
  options.clock = test_clock();
  options.incarnation = IncarnationId{kTestIncarnation};
  return Coordinator::open_ephemeral(options);
}

[[nodiscard]] Revision current_revision(const Coordinator& coordinator, PlanId plan) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(plan);
  return view.has_value() ? view.value().spec.revision : Revision{1};
}

[[nodiscard]] MutationContext context_for(const Coordinator& coordinator, PlanId plan) {
  MutationContext context;
  context.plan = plan;
  context.expected_revision = current_revision(coordinator, plan);
  context.incarnation = coordinator.incarnation();
  context.expected_epoch = coordinator.control_epoch();
  context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  context.principal = "lifecycle-tests";
  context.requested_at_milliseconds = kClockMilliseconds;
  return context;
}

[[nodiscard]] ConsumerRecord asi_obligation(ObligationId obligation,
                                            ObligationStrength strength = ObligationStrength::kMandatory) {
  ConsumerRecord record;
  record.obligation = obligation;
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{100};
  record.reservation = ReservationGeneration{7};
  record.strength = strength;
  record.label = "workload-alpha";
  record.source = "asi";
  return record;
}

[[nodiscard]] CreatePlanRequest plan_request(const Coordinator& coordinator, PlanId id,
                                             const std::vector<ConsumerRecord>& consumers) {
  CreatePlanRequest request;
  request.context = context_for(coordinator, id);
  request.id = id;
  request.scope = DrainScope{ScopeKind::kAsset, 1};
  request.targets = std::vector<DrainScope>{request.scope};
  request.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  request.generations = complete_generations();
  request.policy_id = PolicyId{11};
  request.policy_digest = digest_text("policy-document");
  request.consumers = consumers;
  request.label = "lifecycle";
  request.requested_by = "operator";
  return request;
}

[[nodiscard]] ContentDigest scope_digest(const Coordinator& coordinator, PlanId id) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(id);
  return view.has_value() ? view.value().spec.targets.digest() : ContentDigest{};
}

[[nodiscard]] ContentDigest bound_manifest(const Coordinator& coordinator, PlanId id, OwnerDomain domain) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(id);
  if (!view.has_value()) {
    return ContentDigest{};
  }
  const std::optional<ContentDigest>& bound = view.value().spec.bindings.manifest_digest(domain);
  return bound.has_value() ? bound.value() : ContentDigest{};
}

[[nodiscard]] Result<RecordEnumerationOutcome> enumerate(Coordinator& coordinator, PlanId id, OwnerDomain domain,
                                                         EvidenceGeneration generation,
                                                         const std::vector<ConsumerRecord>& consumers) {
  RecordEnumerationRequest request;
  request.context = context_for(coordinator, id);
  request.domain = domain;
  request.coverage = CoverageState::kComplete;
  request.generation = generation;
  request.observed_at = context_for(coordinator, id).observation;
  request.scope_manifest_digest = scope_digest(coordinator, id);
  request.consumers = consumers;
  request.generations = complete_generations();
  request.source = std::string{to_token(domain)};
  request.annotation = "enumeration";
  return coordinator.record_enumeration(request);
}

[[nodiscard]] Result<IngestCompletionOutcome> complete(Coordinator& coordinator, PlanId id, OwnerDomain domain,
                                                       EvidenceGeneration generation, CompletionState state,
                                                       bool count_known, std::uint64_t count) {
  IngestCompletionRequest request;
  request.context = context_for(coordinator, id);
  request.domain = domain;
  request.state = state;
  request.generation = generation;
  request.observed_at = context_for(coordinator, id).observation;
  request.payload_digest = digest_text("completion-payload");
  request.manifest_digest = bound_manifest(coordinator, id, domain);
  request.scope_manifest_digest = scope_digest(coordinator, id);
  request.generations = complete_generations();
  request.residual_count_known = count_known;
  request.residual_count = count;
  request.source = std::string{to_token(domain)};
  request.annotation = "completion";
  return coordinator.ingest_completion(request);
}

[[nodiscard]] Result<IssueRequestsOutcome> issue(Coordinator& coordinator, PlanId id, DomainMask domains) {
  IssueRequestsRequest request;
  request.context = context_for(coordinator, id);
  request.domains = domains;
  return coordinator.issue_requests(request);
}

[[nodiscard]] std::string state_of(const Coordinator& coordinator, PlanId id) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(id);
  return view.has_value() ? std::string{to_token(view.value().state)} : std::string{"<missing>"};
}

/// A plan whose single required domain is proven drained, so the derivation can
/// reach Drained, SafeToRemove and ResidualsPresent from real recorded facts.
/// The result of the last step is returned so a failure names the exact code.
[[nodiscard]] Result<IngestCompletionOutcome> drain_one_domain(Coordinator& coordinator, PlanId id) {
  const Result<CreatePlanOutcome> created = coordinator.create_plan(
      plan_request(coordinator, id, std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001})}));
  if (!created.has_value()) {
    return Result<IngestCompletionOutcome>{created.error()};
  }
  const Result<RecordEnumerationOutcome> enumeration =
      enumerate(coordinator, id, OwnerDomain::kAsi, EvidenceGeneration{1},
                std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001})});
  if (!enumeration.has_value()) {
    return Result<IngestCompletionOutcome>{enumeration.error()};
  }
  return complete(coordinator, id, OwnerDomain::kAsi, EvidenceGeneration{2}, CompletionState::kDrained, true, 0);
}

constexpr std::array<DrainState, 9> kAllStates{
    DrainState::kProposed,      DrainState::kEnumerating, DrainState::kRequested,
    DrainState::kDraining,      DrainState::kResidualsPresent, DrainState::kDrained,
    DrainState::kSafeToRemove,  DrainState::kCancelled,  DrainState::kFailed};

constexpr std::array<PlanOperation, 13> kAllOperations{
    PlanOperation::kRevise,              PlanOperation::kRecordEnumeration, PlanOperation::kIssueRequests,
    PlanOperation::kRecordAcknowledgement, PlanOperation::kIngestCompletion, PlanOperation::kRecordResidual,
    PlanOperation::kResolveResidual,     PlanOperation::kEvaluate,         PlanOperation::kGrant,
    PlanOperation::kFence,               PlanOperation::kCancel,           PlanOperation::kFail,
    PlanOperation::kSupersedeRequest};

}  // namespace

FDC_TEST(lifecycle, proposed_is_the_state_of_a_fresh_plan) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"proposed"});
}

FDC_TEST(lifecycle, enumerating_follows_the_first_recorded_enumeration) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{}));
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"enumerating"});
}

FDC_TEST(lifecycle, requested_then_draining_follows_the_request_states) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(
      plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001})})));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001})}));

  Result<IssueRequestsOutcome> staged = issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi));
  FDC_REQUIRE_OK(staged);
  FDC_REQUIRE(!staged.value().to_deliver.empty());
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"requested"});

  const DrainRequestId request = staged.value().to_deliver.front().id;
  ConfirmDeliveryRequest delivery;
  delivery.context = context_for(coordinator, PlanId{1});
  delivery.request = request;
  delivery.delivery_reference = "handed to asi";
  FDC_REQUIRE_OK(coordinator.confirm_delivery(delivery));
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"requested"});

  RecordAcknowledgementRequest acknowledgement;
  acknowledgement.context = context_for(coordinator, PlanId{1});
  acknowledgement.request = request;
  acknowledgement.acknowledging_system = "asi";
  FDC_REQUIRE_OK(coordinator.record_acknowledgement(acknowledgement));
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"draining"});
}

FDC_TEST(lifecycle, an_open_residual_makes_the_plan_residuals_present) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  RecordResidualRequest residual;
  residual.context = context_for(coordinator, PlanId{1});
  residual.entry.obligation = ObligationId{1001};
  residual.entry.domain = OwnerDomain::kAsi;
  residual.entry.kind = ResidualKind::kObligationActive;
  residual.entry.detail = "the owner still reports the workload";
  FDC_REQUIRE_OK(coordinator.record_residual(residual));
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"residuals-present"});
}

FDC_TEST(lifecycle, drained_when_every_required_domain_is_proven) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(drain_one_domain(coordinator, PlanId{1}));
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"drained"});

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(!view.value().grant.has_value());
  FDC_CHECK(!view.value().grant_live);
  FDC_CHECK(!is_authority_drain_state(view.value().state));
}

FDC_TEST(lifecycle, safe_to_remove_follows_a_live_grant) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(drain_one_domain(coordinator, PlanId{1}));

  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, PlanId{1});
  grant.granted_by = "operator";
  Result<GrantSafeToRemoveOutcome> granted = coordinator.grant_safe_to_remove(grant);
  FDC_REQUIRE_OK(granted);
  FDC_CHECK_EQ(std::string{to_token(granted.value().evaluation.verdict)}, std::string{"granted"});
  FDC_REQUIRE(granted.value().grant.has_value());

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK_EQ(std::string{to_token(view.value().state)}, std::string{"safe-to-remove"});
  FDC_CHECK(view.value().grant_live);
  FDC_CHECK(is_authority_drain_state(view.value().state));
  FDC_CHECK_EQ(view.value().grant.value().revision, view.value().spec.revision);
  FDC_CHECK_EQ(view.value().grant.value().epoch, coordinator.control_epoch());
}

FDC_TEST(lifecycle, cancelling_is_idempotent_and_terminal) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  CancelPlanRequest cancel;
  cancel.context = context_for(coordinator, PlanId{1});
  cancel.reason = "operator stopped the drain";
  Result<CancelPlanOutcome> cancelled = coordinator.cancel_plan(cancel);
  FDC_REQUIRE_OK(cancelled);
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"cancelled"});
  FDC_CHECK(cancelled.value().plan.cancelled);
  FDC_CHECK(is_terminal_drain_state(cancelled.value().plan.state));

  CancelPlanRequest again;
  again.context = context_for(coordinator, PlanId{1});
  again.reason = "operator stopped the drain again";
  Result<CancelPlanOutcome> repeated = coordinator.cancel_plan(again);
  FDC_REQUIRE_OK(repeated);
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"cancelled"});
  FDC_CHECK_EQ(repeated.value().plan.cancellation_detail, std::string{"operator stopped the drain"});

  FDC_CHECK_CODE(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{}),
                 ErrorCode::kPlanCancelled);
  FDC_CHECK_CODE(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi)), ErrorCode::kPlanCancelled);
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"cancelled"});
}

FDC_TEST(lifecycle, a_failed_plan_is_terminal_and_refuses_work) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  FailPlanRequest fail;
  fail.context = context_for(coordinator, PlanId{1});
  fail.reason = "the owning system reported an unrecoverable fault";
  Result<FailPlanOutcome> failed = coordinator.fail_plan(fail);
  FDC_REQUIRE_OK(failed);
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"failed"});
  FDC_CHECK(failed.value().plan.failed);
  FDC_CHECK(is_terminal_drain_state(failed.value().plan.state));

  FailPlanRequest again;
  again.context = context_for(coordinator, PlanId{1});
  again.reason = "a second reason";
  Result<FailPlanOutcome> repeated = coordinator.fail_plan(again);
  FDC_REQUIRE_OK(repeated);
  FDC_CHECK_EQ(repeated.value().plan.failure_detail,
               std::string{"the owning system reported an unrecoverable fault"});

  FDC_CHECK_CODE(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{}),
                 ErrorCode::kPlanFailed);
  FDC_CHECK_CODE(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi)), ErrorCode::kPlanFailed);
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"failed"});
}

FDC_TEST(lifecycle, check_plan_operation_follows_the_documented_table) {
  for (const DrainState state : kAllStates) {
    FDC_CHECK(check_plan_operation(state, PlanOperation::kEvaluate).ok());
    for (const PlanOperation operation : kAllOperations) {
      bool expected_ok = true;
      ErrorCode expected_code = ErrorCode::kOk;
      if (operation == PlanOperation::kEvaluate) {
        expected_ok = true;
      } else if (state == DrainState::kCancelled) {
        expected_ok = operation == PlanOperation::kRevise;
        expected_code = ErrorCode::kPlanCancelled;
      } else if (state == DrainState::kFailed) {
        expected_ok = operation == PlanOperation::kRevise || operation == PlanOperation::kCancel;
        expected_code = ErrorCode::kPlanFailed;
      } else if (state == DrainState::kSafeToRemove) {
        expected_ok = operation != PlanOperation::kIssueRequests && operation != PlanOperation::kSupersedeRequest;
        expected_code = ErrorCode::kInvalidStateTransition;
      }
      const Status status = check_plan_operation(state, operation);
      FDC_CHECK_EQ(status.ok(), expected_ok);
      if (!expected_ok) {
        FDC_CHECK_EQ(status.code(), expected_code);
      }
    }
  }
}

FDC_TEST(lifecycle, terminal_and_authority_state_predicates) {
  for (const DrainState state : kAllStates) {
    const bool terminal = state == DrainState::kCancelled || state == DrainState::kFailed;
    const bool authority = state == DrainState::kSafeToRemove;
    FDC_CHECK_EQ(is_terminal_drain_state(state), terminal);
    FDC_CHECK_EQ(is_authority_drain_state(state), authority);
  }
}
