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
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"
#include "test_harness.hpp"

#include <cstddef>
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
  context.principal = "request-tests";
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

[[nodiscard]] ConsumerRecord dfi_obligation(ObligationId obligation) {
  ConsumerRecord record;
  record.obligation = obligation;
  record.category = ConsumerCategory::kNetworkPath;
  record.generation = ObligationGeneration{200};
  record.reservation = ReservationGeneration{8};
  record.strength = ObligationStrength::kMandatory;
  record.label = "path-beta";
  record.source = "dfi";
  return record;
}

[[nodiscard]] CreatePlanRequest plan_request(const Coordinator& coordinator, PlanId id,
                                             const std::vector<ConsumerRecord>& consumers,
                                             DomainMask required = DomainMask::all()) {
  CreatePlanRequest request;
  request.context = context_for(coordinator, id);
  request.id = id;
  request.scope = DrainScope{ScopeKind::kAsset, 1};
  request.targets = std::vector<DrainScope>{request.scope};
  request.declared_required_domains = required;
  request.generations = complete_generations();
  request.policy_id = PolicyId{11};
  request.policy_digest = digest_text("policy-document");
  request.consumers = consumers;
  request.label = "requests";
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
                                                         const std::vector<ConsumerRecord>& consumers,
                                                         CoverageState coverage = CoverageState::kComplete) {
  RecordEnumerationRequest request;
  request.context = context_for(coordinator, id);
  request.domain = domain;
  request.coverage = coverage;
  request.generation = generation;
  request.observed_at = context_for(coordinator, id).observation;
  request.scope_manifest_digest = scope_digest(coordinator, id);
  request.consumers = consumers;
  request.generations = complete_generations();
  request.source = std::string{to_token(domain)};
  request.annotation = "enumeration";
  return coordinator.record_enumeration(request);
}

[[nodiscard]] Result<IssueRequestsOutcome> issue(Coordinator& coordinator, PlanId id, DomainMask domains,
                                                 std::uint32_t bound_operations = 0) {
  IssueRequestsRequest request;
  request.context = context_for(coordinator, id);
  request.domains = domains;
  request.bound_operations = bound_operations;
  return coordinator.issue_requests(request);
}

[[nodiscard]] Result<ConfirmDeliveryOutcome> deliver(Coordinator& coordinator, PlanId id, DrainRequestId request,
                                                     std::string reference) {
  ConfirmDeliveryRequest delivery;
  delivery.context = context_for(coordinator, id);
  delivery.request = request;
  delivery.delivery_reference = std::move(reference);
  return coordinator.confirm_delivery(delivery);
}

/// Creates one plan with a single ASI obligation, enumerates it completely and
/// issues exactly one staged request, ready for the delivery and supersede
/// tests.
[[nodiscard]] Result<IssueRequestsOutcome> prepare_staged_request(Coordinator& coordinator, PlanId id) {
  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001})};
  const Result<CreatePlanOutcome> created = coordinator.create_plan(plan_request(coordinator, id, consumers));
  if (!created.has_value()) {
    return Result<IssueRequestsOutcome>{created.error()};
  }
  const Result<RecordEnumerationOutcome> enumerated =
      enumerate(coordinator, id, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers);
  if (!enumerated.has_value()) {
    return Result<IssueRequestsOutcome>{enumerated.error()};
  }
  return issue(coordinator, id, DomainMask::of(OwnerDomain::kAsi));
}

/// Creates a plan whose every named domain is proven drained, then grants it, so
/// the plan carries live removal authority.
[[nodiscard]] Result<GrantSafeToRemoveOutcome> prepare_granted_plan(Coordinator& coordinator, PlanId id) {
  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001})};
  const Result<CreatePlanOutcome> created = coordinator.create_plan(
      plan_request(coordinator, id, consumers, DomainMask::of(OwnerDomain::kAsi)));
  if (!created.has_value()) {
    return Result<GrantSafeToRemoveOutcome>{created.error()};
  }
  const Result<RecordEnumerationOutcome> enumerated =
      enumerate(coordinator, id, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers);
  if (!enumerated.has_value()) {
    return Result<GrantSafeToRemoveOutcome>{enumerated.error()};
  }
  IngestCompletionRequest completion;
  completion.context = context_for(coordinator, id);
  completion.domain = OwnerDomain::kAsi;
  completion.state = CompletionState::kDrained;
  completion.generation = EvidenceGeneration{2};
  completion.observed_at = context_for(coordinator, id).observation;
  completion.payload_digest = digest_text("completion-payload");
  completion.manifest_digest = bound_manifest(coordinator, id, OwnerDomain::kAsi);
  completion.scope_manifest_digest = scope_digest(coordinator, id);
  completion.generations = complete_generations();
  completion.residual_count_known = true;
  completion.residual_count = 0;
  completion.source = "asi";
  const Result<IngestCompletionOutcome> completed = coordinator.ingest_completion(completion);
  if (!completed.has_value()) {
    return Result<GrantSafeToRemoveOutcome>{completed.error()};
  }
  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, id);
  grant.granted_by = "operator";
  return coordinator.grant_safe_to_remove(grant);
}

}  // namespace

FDC_TEST(requests, issue_stages_one_request_per_named_domain) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> asi{asi_obligation(ObligationId{1001})};
  const std::vector<ConsumerRecord> dfi{dfi_obligation(ObligationId{2002})};
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{asi[0], dfi[0]})));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, asi));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kDfi, EvidenceGeneration{1}, dfi));

  Result<IssueRequestsOutcome> issued =
      issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi));
  FDC_REQUIRE_OK(issued);
  FDC_CHECK_EQ(issued.value().items.size(), std::size_t{2});
  FDC_CHECK_EQ(issued.value().to_deliver.size(), std::size_t{2});
  FDC_CHECK_EQ(issued.value().newly_staged, 2U);
  FDC_CHECK_EQ(issued.value().duplicates, 0U);

  FDC_CHECK(!issued.value().items[0].duplicate);
  FDC_CHECK(issued.value().items[0].deliver);
  FDC_CHECK(!issued.value().items[1].duplicate);
  FDC_CHECK(issued.value().items[1].deliver);

  const DrainRequest& asi_request = issued.value().to_deliver[0];
  const DrainRequest& dfi_request = issued.value().to_deliver[1];
  FDC_CHECK_EQ(asi_request.key.domain, OwnerDomain::kAsi);
  FDC_CHECK_EQ(dfi_request.key.domain, OwnerDomain::kDfi);
  FDC_CHECK_EQ(std::string{to_token(asi_request.state)}, std::string{"staged"});
  FDC_CHECK_EQ(std::string{to_token(dfi_request.state)}, std::string{"staged"});
  FDC_CHECK_EQ(asi_request.target_system, std::string{"asi"});
  FDC_CHECK_EQ(dfi_request.target_system, std::string{"dfi"});
  FDC_CHECK(!asi_request.id.is_default());
  FDC_CHECK(asi_request.id != dfi_request.id);

  Result<std::vector<DrainRequest>> recorded = coordinator.requests(PlanId{1});
  FDC_REQUIRE_OK(recorded);
  FDC_CHECK_EQ(recorded.value().size(), std::size_t{2});
  FDC_CHECK_EQ(std::string{to_token(recorded.value()[0].state)}, std::string{"staged"});
  FDC_CHECK_EQ(std::string{to_token(recorded.value()[1].state)}, std::string{"staged"});
}

FDC_TEST(requests, the_instruction_target_and_bound_are_reproducible) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001}),
                                              asi_obligation(ObligationId{1002})};
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, consumers)));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers));

  Result<IssueRequestsOutcome> issued = issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi));
  FDC_REQUIRE_OK(issued);
  FDC_REQUIRE(!issued.value().to_deliver.empty());
  const DrainRequest& request = issued.value().to_deliver.front();

  DrainRequestKey key;
  key.plan = PlanId{1};
  key.domain = OwnerDomain::kAsi;
  key.scope = DrainScope{ScopeKind::kAsset, 1};
  key.obligation_digest = consumer_manifest_digest(OwnerDomain::kAsi, consumers);
  key.policy_generation = complete_generations().policy;
  key.attempt = AttemptId{1};

  FDC_CHECK_EQ(request.key.obligation_digest, key.obligation_digest);
  FDC_CHECK_EQ(request.key.policy_generation, key.policy_generation);
  FDC_CHECK_EQ(request.key.attempt, AttemptId{1});
  FDC_CHECK_EQ(request.id, request_id_for(key));
  FDC_CHECK_EQ(request.idempotency_key, request_idempotency_key(key));
  FDC_CHECK_EQ(request.bound_operations, 2U);
  FDC_CHECK_EQ(request.instruction, std::string{"drain domain=asi scope=asset:1 obligations=2 bound=2"});
  FDC_CHECK_EQ(request.target_system, std::string{"asi"});
  FDC_CHECK_EQ(request.staged_at, coordinator.observation_sequence());
  FDC_CHECK_EQ(request.staged_at_milliseconds, kClockMilliseconds);

  DrainRequest bounded = request;
  bounded.bound_operations = 5U;
  FDC_CHECK_EQ(bounded.bound_operations, 5U);
  FDC_CHECK_EQ(std::string{to_token(bounded.state)}, std::string{"staged"});
}

FDC_TEST(requests, the_explicit_bound_is_respected_and_the_derived_bound_is_the_mandatory_count) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001}),
                                              asi_obligation(ObligationId{1002}, ObligationStrength::kAdvisory),
                                              asi_obligation(ObligationId{1003})};
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, consumers)));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers));

  Result<IssueRequestsOutcome> derived = issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi));
  FDC_REQUIRE_OK(derived);
  FDC_REQUIRE(!derived.value().to_deliver.empty());
  FDC_CHECK_EQ(derived.value().to_deliver.front().bound_operations, 2U);
  FDC_CHECK_EQ(derived.value().to_deliver.front().instruction,
               std::string{"drain domain=asi scope=asset:1 obligations=3 bound=2"});
}

FDC_TEST(requests, an_explicit_bound_beyond_the_consumer_limit_is_rejected) {
  Limits limits;
  limits.max_consumers_per_domain = 4;
  Result<Coordinator> opened = open_coordinator(limits);
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001})};
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, consumers)));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers));

  FDC_CHECK_CODE(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi), 5U),
                 ErrorCode::kLimitExceeded);
  FDC_REQUIRE_OK(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi), 4U));
}

FDC_TEST(requests, an_empty_manifest_yields_no_request) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{}));

  Result<IssueRequestsOutcome> issued = issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi));
  FDC_REQUIRE_OK(issued);
  FDC_CHECK_EQ(issued.value().items.size(), std::size_t{1});
  FDC_CHECK(!issued.value().items.front().duplicate);
  FDC_CHECK(!issued.value().items.front().deliver);
  FDC_CHECK(issued.value().to_deliver.empty());
  FDC_CHECK_EQ(issued.value().newly_staged, 0U);
}

FDC_TEST(requests, a_repeat_issue_reports_duplicate_and_re_offers_only_a_staged_request) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<IssueRequestsOutcome> first = prepare_staged_request(coordinator, PlanId{1});
  FDC_REQUIRE_OK(first);
  FDC_REQUIRE(!first.value().to_deliver.empty());
  const DrainRequest staged = first.value().to_deliver.front();

  Result<IssueRequestsOutcome> repeat = issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi));
  FDC_REQUIRE_OK(repeat);
  FDC_CHECK_EQ(repeat.value().duplicates, 1U);
  FDC_CHECK_EQ(repeat.value().newly_staged, 0U);
  FDC_REQUIRE(!repeat.value().items.empty());
  FDC_CHECK(repeat.value().items.front().duplicate);
  FDC_CHECK(repeat.value().items.front().deliver);
  FDC_CHECK_EQ(repeat.value().to_deliver.size(), std::size_t{1});
  FDC_CHECK_EQ(repeat.value().to_deliver.front().id, staged.id);
  FDC_CHECK_EQ(repeat.value().to_deliver.front().idempotency_key, staged.idempotency_key);

  Result<ConfirmDeliveryOutcome> delivered = deliver(coordinator, PlanId{1}, staged.id, "handed to asi");
  FDC_REQUIRE_OK(delivered);
  FDC_CHECK_EQ(std::string{to_token(delivered.value().request.state)}, std::string{"issued"});

  Result<IssueRequestsOutcome> after_delivery = issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi));
  FDC_REQUIRE_OK(after_delivery);
  FDC_CHECK_EQ(after_delivery.value().duplicates, 1U);
  FDC_CHECK_EQ(after_delivery.value().newly_staged, 0U);
  FDC_REQUIRE(!after_delivery.value().items.empty());
  FDC_CHECK(after_delivery.value().items.front().duplicate);
  FDC_CHECK(!after_delivery.value().items.front().deliver);
  FDC_CHECK(after_delivery.value().to_deliver.empty());
}

FDC_TEST(requests, issuing_without_an_accepted_enumeration_fails) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001})};
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, consumers)));
  FDC_CHECK_CODE(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi)),
                 ErrorCode::kIncompleteEnumeration);

  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers,
                           CoverageState::kPartial));
  FDC_CHECK_CODE(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi)),
                 ErrorCode::kIncompleteEnumeration);

  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{2}, consumers));
  FDC_REQUIRE_OK(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi)));
}

FDC_TEST(requests, issuing_with_an_empty_domain_mask_fails) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_CHECK_CODE(issue(coordinator, PlanId{1}, DomainMask::none()), ErrorCode::kMissingRequiredField);
}

FDC_TEST(requests, issuing_into_a_safe_to_remove_plan_fails) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(prepare_granted_plan(coordinator, PlanId{1}));
  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK_EQ(std::string{to_token(view.value().state)}, std::string{"safe-to-remove"});

  FDC_CHECK_CODE(issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi)),
                 ErrorCode::kInvalidStateTransition);
}

FDC_TEST(requests, confirm_delivery_is_idempotent) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<IssueRequestsOutcome> first = prepare_staged_request(coordinator, PlanId{1});
  FDC_REQUIRE_OK(first);
  FDC_REQUIRE(!first.value().to_deliver.empty());
  const DrainRequestId request = first.value().to_deliver.front().id;

  Result<ConfirmDeliveryOutcome> delivered = deliver(coordinator, PlanId{1}, request, "handed to asi");
  FDC_REQUIRE_OK(delivered);
  const ObservationSequence issued_at = delivered.value().request.issued_at;
  FDC_CHECK_EQ(std::string{to_token(delivered.value().request.state)}, std::string{"issued"});
  FDC_CHECK(!issued_at.is_default());

  const std::uint64_t commit_after_first = coordinator.commit_sequence().value();
  Result<ConfirmDeliveryOutcome> repeated = deliver(coordinator, PlanId{1}, request, "handed to asi again");
  FDC_REQUIRE_OK(repeated);
  FDC_CHECK_EQ(std::string{to_token(repeated.value().request.state)}, std::string{"issued"});
  FDC_CHECK_EQ(repeated.value().request.issued_at, issued_at);
  FDC_CHECK_EQ(repeated.value().request.id, request);
  FDC_CHECK_EQ(coordinator.commit_sequence().value(), commit_after_first);
}

FDC_TEST(requests, acknowledgement_of_a_staged_request_fails) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<IssueRequestsOutcome> staged = prepare_staged_request(coordinator, PlanId{1});
  FDC_REQUIRE_OK(staged);
  FDC_REQUIRE(!staged.value().to_deliver.empty());

  RecordAcknowledgementRequest acknowledgement;
  acknowledgement.context = context_for(coordinator, PlanId{1});
  acknowledgement.request = staged.value().to_deliver.front().id;
  acknowledgement.acknowledging_system = "asi";
  FDC_CHECK_CODE(coordinator.record_acknowledgement(acknowledgement), ErrorCode::kInvalidStateTransition);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_REQUIRE(view.value().requests.size() == std::size_t{1});
  FDC_CHECK_EQ(std::string{to_token(view.value().requests.front().state)}, std::string{"staged"});
}

FDC_TEST(requests, unknown_request_ids_fail) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(prepare_staged_request(coordinator, PlanId{1}));
  const DrainRequestId unknown{987654321ULL};

  ConfirmDeliveryRequest delivery;
  delivery.context = context_for(coordinator, PlanId{1});
  delivery.request = unknown;
  FDC_CHECK_CODE(coordinator.confirm_delivery(delivery), ErrorCode::kRequestNotFound);

  RecordAcknowledgementRequest acknowledgement;
  acknowledgement.context = context_for(coordinator, PlanId{1});
  acknowledgement.request = unknown;
  acknowledgement.acknowledging_system = "asi";
  FDC_CHECK_CODE(coordinator.record_acknowledgement(acknowledgement), ErrorCode::kRequestNotFound);

  SupersedeRequestRequest supersede;
  supersede.context = context_for(coordinator, PlanId{1});
  supersede.request = unknown;
  supersede.reason = "the owner lost the request";
  FDC_CHECK_CODE(coordinator.supersede_request(supersede), ErrorCode::kRequestNotFound);
}

FDC_TEST(requests, supersede_advances_the_attempt_and_the_key) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<IssueRequestsOutcome> staged = prepare_staged_request(coordinator, PlanId{1});
  FDC_REQUIRE_OK(staged);
  FDC_REQUIRE(!staged.value().to_deliver.empty());
  const DrainRequest original = staged.value().to_deliver.front();

  SupersedeRequestRequest supersede;
  supersede.context = context_for(coordinator, PlanId{1});
  supersede.request = original.id;
  supersede.reason = "the owner never received the bounded request";
  Result<SupersedeRequestOutcome> superseded = coordinator.supersede_request(supersede);
  FDC_REQUIRE_OK(superseded);

  FDC_CHECK_EQ(std::string{to_token(superseded.value().superseded.state)}, std::string{"superseded"});
  FDC_CHECK_EQ(superseded.value().superseded.id, original.id);
  FDC_CHECK_EQ(std::string{to_token(superseded.value().replacement.state)}, std::string{"staged"});
  FDC_CHECK_EQ(superseded.value().replacement.key.attempt, AttemptId{2});
  FDC_CHECK(superseded.value().replacement.id != original.id);
  FDC_CHECK(superseded.value().replacement.idempotency_key != original.idempotency_key);
  FDC_CHECK_EQ(superseded.value().replacement.key.obligation_digest, original.key.obligation_digest);
  FDC_CHECK_EQ(superseded.value().replacement.bound_operations, original.bound_operations);
  FDC_CHECK_EQ(superseded.value().replacement.instruction, original.instruction);

  Result<std::vector<DrainRequest>> recorded = coordinator.requests(PlanId{1});
  FDC_REQUIRE_OK(recorded);
  FDC_CHECK_EQ(recorded.value().size(), std::size_t{2});
  FDC_CHECK_EQ(recorded.value()[0].key.attempt, AttemptId{1});
  FDC_CHECK_EQ(recorded.value()[1].key.attempt, AttemptId{2});

  Result<IssueRequestsOutcome> reissued = issue(coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi));
  FDC_REQUIRE_OK(reissued);
  FDC_CHECK_EQ(reissued.value().duplicates, 1U);
  FDC_REQUIRE(!reissued.value().to_deliver.empty());
  FDC_CHECK_EQ(reissued.value().to_deliver.front().id, superseded.value().replacement.id);
  FDC_CHECK_EQ(reissued.value().to_deliver.front().key.attempt, AttemptId{2});
}

FDC_TEST(requests, supersede_requires_a_reason) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<IssueRequestsOutcome> staged = prepare_staged_request(coordinator, PlanId{1});
  FDC_REQUIRE_OK(staged);
  FDC_REQUIRE(!staged.value().to_deliver.empty());
  const DrainRequestId request = staged.value().to_deliver.front().id;

  SupersedeRequestRequest empty;
  empty.context = context_for(coordinator, PlanId{1});
  empty.request = request;
  FDC_CHECK_CODE(coordinator.supersede_request(empty), ErrorCode::kMissingRequiredField);

  SupersedeRequestRequest valid;
  valid.context = context_for(coordinator, PlanId{1});
  valid.request = request;
  valid.reason = "the owner never received the bounded request";
  FDC_REQUIRE_OK(coordinator.supersede_request(valid));

  SupersedeRequestRequest again;
  again.context = context_for(coordinator, PlanId{1});
  again.request = request;
  again.reason = "a second supersede";
  FDC_CHECK_CODE(coordinator.supersede_request(again), ErrorCode::kRequestSuperseded);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK_EQ(view.value().requests.size(), std::size_t{2});
}
