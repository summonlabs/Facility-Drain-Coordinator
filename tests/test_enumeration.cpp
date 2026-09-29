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
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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
  context.principal = "enumeration-tests";
  context.requested_at_milliseconds = kClockMilliseconds;
  return context;
}

[[nodiscard]] ConsumerRecord asi_obligation(ObligationId obligation) {
  ConsumerRecord record;
  record.obligation = obligation;
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{100};
  record.reservation = ReservationGeneration{7};
  record.strength = ObligationStrength::kMandatory;
  record.label = "workload-alpha";
  record.source = "asi";
  return record;
}

[[nodiscard]] CreatePlanRequest plan_request(const Coordinator& coordinator, PlanId id,
                                             const std::vector<ConsumerRecord>& consumers,
                                             bool bind_manifests = true) {
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
  request.bind_consumer_manifests = bind_manifests;
  request.label = "enumeration";
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

[[nodiscard]] RecordEnumerationRequest enumeration_request(const Coordinator& coordinator, PlanId id,
                                                           OwnerDomain domain, CoverageState coverage,
                                                           EvidenceGeneration generation,
                                                           const std::vector<ConsumerRecord>& consumers) {
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
  return request;
}

/// Records one accepted complete enumeration of the domain's bound manifest.
[[nodiscard]] Result<RecordEnumerationOutcome> enumerate_bound(Coordinator& coordinator, PlanId id,
                                                               OwnerDomain domain, EvidenceGeneration generation,
                                                               const std::vector<ConsumerRecord>& consumers) {
  return coordinator.record_enumeration(
      enumeration_request(coordinator, id, domain, CoverageState::kComplete, generation, consumers));
}

}  // namespace

FDC_TEST(enumeration, a_complete_enumeration_is_recorded_and_accepted) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001}),
                                              asi_obligation(ObligationId{1002})};
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, consumers)));

  Result<RecordEnumerationOutcome> recorded =
      enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers);
  FDC_REQUIRE_OK(recorded);
  FDC_CHECK(recorded.value().accepted);
  FDC_CHECK(!recorded.value().bound_manifest);
  FDC_CHECK_EQ(recorded.value().consumers_recorded, 2U);
  FDC_CHECK_EQ(recorded.value().evidence.domain, OwnerDomain::kAsi);
  FDC_CHECK_EQ(std::string{to_token(recorded.value().evidence.coverage)}, std::string{"complete"});
  FDC_CHECK_EQ(recorded.value().evidence.generation, EvidenceGeneration{1});
  FDC_CHECK_EQ(recorded.value().evidence.manifest_digest, bound_manifest(coordinator, PlanId{1}, OwnerDomain::kAsi));

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  const DomainEnumerationSnapshot& enumeration = view.value().enumerations[0];
  FDC_CHECK(enumeration.present);
  FDC_CHECK(enumeration.accepted);
  FDC_CHECK_EQ(enumeration.rejection, ErrorCode::kOk);
  FDC_CHECK_EQ(enumeration.consumers.size(), std::size_t{2});
  FDC_CHECK_EQ(view.value().consumers.size(), std::size_t{2});
  FDC_CHECK_EQ(std::string{to_token(view.value().state)}, std::string{"enumerating"});
}

FDC_TEST(enumeration, a_partial_enumeration_is_recorded_but_does_not_bind) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  const ContentDigest bound_before = bound_manifest(coordinator, PlanId{1}, OwnerDomain::kAsi);
  FDC_CHECK(!bound_before.is_zero());

  Result<RecordEnumerationOutcome> recorded = coordinator.record_enumeration(enumeration_request(
      coordinator, PlanId{1}, OwnerDomain::kAsi, CoverageState::kPartial, EvidenceGeneration{1},
      std::vector<ConsumerRecord>{}));
  FDC_REQUIRE_OK(recorded);
  FDC_CHECK(recorded.value().accepted);
  FDC_CHECK(!recorded.value().bound_manifest);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(view.value().enumerations[0].present);
  FDC_CHECK(view.value().enumerations[0].accepted);
  FDC_CHECK_EQ(view.value().enumerations[0].evidence.generation, EvidenceGeneration{1});
  FDC_CHECK_EQ(bound_manifest(coordinator, PlanId{1}, OwnerDomain::kAsi), bound_before);
}

FDC_TEST(enumeration, only_a_complete_enumeration_binds_an_unbound_manifest) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001})};
  FDC_REQUIRE_OK(
      coordinator.create_plan(plan_request(coordinator, PlanId{1}, consumers, /*bind_manifests=*/false)));

  Result<DrainPlanSnapshot> unbound = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(unbound);
  FDC_CHECK(!unbound.value().spec.bindings.manifest_digest(OwnerDomain::kAsi).has_value());
  FDC_CHECK(unbound.value().spec.bindings.manifest_digest(OwnerDomain::kDfi).has_value() == false);

  const std::uint64_t commit_before = coordinator.commit_sequence().value();
  Result<RecordEnumerationOutcome> partial = coordinator.record_enumeration(enumeration_request(
      coordinator, PlanId{1}, OwnerDomain::kAsi, CoverageState::kPartial, EvidenceGeneration{1}, consumers));
  FDC_CHECK_CODE(partial, ErrorCode::kConsumerDigestMismatch);

  Result<DrainPlanSnapshot> unchanged = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(unchanged);
  FDC_CHECK(!unchanged.value().enumerations[0].present);
  FDC_CHECK(!unchanged.value().spec.bindings.manifest_digest(OwnerDomain::kAsi).has_value());
  FDC_CHECK_EQ(coordinator.commit_sequence().value(), commit_before);

  Result<RecordEnumerationOutcome> complete =
      enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers);
  FDC_REQUIRE_OK(complete);
  FDC_CHECK(complete.value().accepted);
  FDC_CHECK(complete.value().bound_manifest);
  FDC_CHECK_EQ(complete.value().evidence.manifest_digest,
               consumer_manifest_digest(OwnerDomain::kAsi, consumers));
  FDC_CHECK_EQ(bound_manifest(coordinator, PlanId{1}, OwnerDomain::kAsi),
               complete.value().evidence.manifest_digest);
}

FDC_TEST(enumeration, a_non_advancing_generation_is_rejected_as_stale) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{4},
                                 std::vector<ConsumerRecord>{}));

  FDC_CHECK_CODE(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{4},
                                 std::vector<ConsumerRecord>{}),
                 ErrorCode::kStaleEvidence);
  FDC_CHECK_CODE(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{3},
                                 std::vector<ConsumerRecord>{}),
                 ErrorCode::kStaleEvidence);
  FDC_CHECK_CODE(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{0},
                                 std::vector<ConsumerRecord>{}),
                 ErrorCode::kMissingRequiredField);
  FDC_REQUIRE_OK(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{5},
                                 std::vector<ConsumerRecord>{}));

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK_EQ(view.value().enumerations[0].evidence.generation, EvidenceGeneration{5});
}

FDC_TEST(enumeration, the_generation_counter_is_shared_with_completions) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                                 std::vector<ConsumerRecord>{}));

  IngestCompletionRequest completion;
  completion.context = context_for(coordinator, PlanId{1});
  completion.domain = OwnerDomain::kAsi;
  completion.state = CompletionState::kDraining;
  completion.generation = EvidenceGeneration{5};
  completion.observed_at = context_for(coordinator, PlanId{1}).observation;
  completion.payload_digest = digest_text("completion-payload");
  completion.manifest_digest = bound_manifest(coordinator, PlanId{1}, OwnerDomain::kAsi);
  completion.scope_manifest_digest = scope_digest(coordinator, PlanId{1});
  completion.generations = complete_generations();
  completion.source = "asi";
  FDC_REQUIRE_OK(coordinator.ingest_completion(completion));

  FDC_CHECK_CODE(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{5},
                                 std::vector<ConsumerRecord>{}),
                 ErrorCode::kStaleEvidence);
  FDC_CHECK_CODE(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{4},
                                 std::vector<ConsumerRecord>{}),
                 ErrorCode::kStaleEvidence);
  FDC_REQUIRE_OK(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{6},
                                 std::vector<ConsumerRecord>{}));
  // The counter is per (plan, domain): the completion for ASI does not consume
  // DFI's evidence generations.
  FDC_REQUIRE_OK(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kDfi, EvidenceGeneration{1},
                                 std::vector<ConsumerRecord>{}));
}

FDC_TEST(enumeration, a_wrong_scope_manifest_digest_is_rejected) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  RecordEnumerationRequest wrong_scope = enumeration_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                             CoverageState::kComplete, EvidenceGeneration{1},
                                                             std::vector<ConsumerRecord>{});
  wrong_scope.scope_manifest_digest = digest_text("the-wrong-physical-membership");
  FDC_CHECK_CODE(coordinator.record_enumeration(wrong_scope), ErrorCode::kScopeManifestMismatch);

  RecordEnumerationRequest unset_scope = enumeration_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                             CoverageState::kComplete, EvidenceGeneration{1},
                                                             std::vector<ConsumerRecord>{});
  unset_scope.scope_manifest_digest = ContentDigest{};
  FDC_CHECK_CODE(coordinator.record_enumeration(unset_scope), ErrorCode::kMissingRequiredField);

  RecordEnumerationRequest not_enumerated = enumeration_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                               CoverageState::kNotEnumerated, EvidenceGeneration{1},
                                                               std::vector<ConsumerRecord>{});
  FDC_CHECK_CODE(coordinator.record_enumeration(not_enumerated), ErrorCode::kInvalidEnumValue);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(!view.value().enumerations[0].present);
}

FDC_TEST(enumeration, a_consumer_manifest_that_disagrees_records_nothing) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(
      plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001})})));

  Result<DrainPlanSnapshot> before = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(before);
  const ObservationSequence observation_before = before.value().last_observation;
  const std::uint64_t commit_before = coordinator.commit_sequence().value();

  FDC_CHECK_CODE(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                                 std::vector<ConsumerRecord>{asi_obligation(ObligationId{9999})}),
                 ErrorCode::kConsumerDigestMismatch);

  Result<DrainPlanSnapshot> after = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(after);
  FDC_CHECK(!after.value().enumerations[0].present);
  FDC_CHECK_EQ(after.value().last_observation, observation_before);
  FDC_CHECK_EQ(after.value().consumers.size(), std::size_t{1});
  FDC_CHECK_EQ(after.value().consumers.front().obligation, ObligationId{1001});
  FDC_CHECK_EQ(coordinator.commit_sequence().value(), commit_before);
}

FDC_TEST(enumeration, a_foreign_domain_category_is_rejected) {
  Limits limits;
  const std::vector<ConsumerRecord> asi_records{asi_obligation(ObligationId{1001})};
  FDC_CHECK(validate_domain_manifest(OwnerDomain::kAsi, asi_records, limits).ok());
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, asi_records, limits),
                   ErrorCode::kConflictingField);

  ConsumerRecord dfi_record = asi_obligation(ObligationId{1002});
  dfi_record.category = ConsumerCategory::kMonitoringDependency;
  const std::vector<ConsumerRecord> dfi_records{dfi_record};
  FDC_CHECK(validate_domain_manifest(OwnerDomain::kMonitoring, dfi_records, limits).ok());
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kFacility, dfi_records, limits),
                   ErrorCode::kConflictingField);
}

FDC_TEST(enumeration, duplicate_obligation_ids_are_rejected) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  FDC_CHECK_CODE(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                                 std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001}),
                                                             asi_obligation(ObligationId{1001})}),
                 ErrorCode::kDuplicateIdentifier);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(!view.value().enumerations[0].present);
}

FDC_TEST(enumeration, the_retention_bound_trims_the_oldest_record) {
  Limits limits;
  limits.max_evidence_per_domain = 2;
  Result<Coordinator> opened = open_coordinator(limits);
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  // The bound is a retention bound, not a hard refusal: a record past the bound
  // is accepted and the oldest retained record is dropped, so the newest record
  // is always the one a reader sees.
  for (std::uint64_t generation = 1; generation <= 3U; ++generation) {
    FDC_REQUIRE_OK(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                   EvidenceGeneration{generation}, std::vector<ConsumerRecord>{}));
  }

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK_EQ(view.value().enumerations[0].evidence.generation, EvidenceGeneration{3});

  FDC_REQUIRE_OK(enumerate_bound(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{4},
                                 std::vector<ConsumerRecord>{}));
  Result<DrainPlanSnapshot> reread = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(reread);
  FDC_CHECK_EQ(reread.value().enumerations[0].evidence.generation, EvidenceGeneration{4});
}
