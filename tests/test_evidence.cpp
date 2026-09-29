// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/evaluation.hpp"
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

[[nodiscard]] Result<Coordinator> open_coordinator() {
  EphemeralOptions options;
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
  context.principal = "evidence-tests";
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

/// The valid completion record every validation test perturbs exactly once.
[[nodiscard]] CompletionEvidence valid_completion() {
  CompletionEvidence evidence;
  evidence.id = EvidenceId{101};
  evidence.domain = OwnerDomain::kAsi;
  evidence.state = CompletionState::kDrained;
  evidence.generation = EvidenceGeneration{2};
  evidence.observed_at = ObservationSequence{2};
  evidence.payload_digest = digest_text("completion-payload");
  evidence.manifest_digest = digest_text("consumer-manifest");
  evidence.scope_manifest_digest = digest_text("scope-manifest");
  evidence.generations = complete_generations();
  evidence.residual_count_known = true;
  evidence.residual_count = 0;
  evidence.source = "asi";
  evidence.annotation = "drained";
  return evidence;
}

[[nodiscard]] EnumerationEvidence valid_enumeration() {
  EnumerationEvidence evidence;
  evidence.domain = OwnerDomain::kAsi;
  evidence.coverage = CoverageState::kComplete;
  evidence.generation = EvidenceGeneration{1};
  evidence.observed_at = ObservationSequence{1};
  evidence.manifest_digest = digest_text("consumer-manifest");
  evidence.scope_manifest_digest = digest_text("scope-manifest");
  evidence.generations = complete_generations();
  evidence.source = "asi";
  evidence.annotation = "enumerated";
  return evidence;
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
  request.label = "evidence";
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

[[nodiscard]] IngestCompletionRequest completion_request(const Coordinator& coordinator, PlanId id,
                                                         OwnerDomain domain, EvidenceGeneration generation) {
  IngestCompletionRequest request;
  request.context = context_for(coordinator, id);
  request.domain = domain;
  request.state = CompletionState::kDrained;
  request.generation = generation;
  request.observed_at = context_for(coordinator, id).observation;
  request.payload_digest = digest_text("completion-payload");
  request.manifest_digest = bound_manifest(coordinator, id, domain);
  request.scope_manifest_digest = scope_digest(coordinator, id);
  request.generations = complete_generations();
  request.residual_count_known = true;
  request.residual_count = 0;
  request.source = std::string{to_token(domain)};
  request.annotation = "completion";
  return request;
}

/// A plan with one ASI obligation, fully enumerated, ready for completion
/// evidence.
[[nodiscard]] Result<RecordEnumerationOutcome> prepare_enumerated_plan(Coordinator& coordinator, PlanId id) {
  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001})};
  const Result<CreatePlanOutcome> created = coordinator.create_plan(plan_request(coordinator, id, consumers));
  if (!created.has_value()) {
    return Result<RecordEnumerationOutcome>{created.error()};
  }
  RecordEnumerationRequest enumeration;
  enumeration.context = context_for(coordinator, id);
  enumeration.domain = OwnerDomain::kAsi;
  enumeration.coverage = CoverageState::kComplete;
  enumeration.generation = EvidenceGeneration{1};
  enumeration.observed_at = context_for(coordinator, id).observation;
  enumeration.scope_manifest_digest = scope_digest(coordinator, id);
  enumeration.consumers = consumers;
  enumeration.generations = complete_generations();
  enumeration.source = "asi";
  return coordinator.record_enumeration(enumeration);
}

[[nodiscard]] std::string state_of(const Coordinator& coordinator, PlanId id) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(id);
  return view.has_value() ? std::string{to_token(view.value().state)} : std::string{"<missing>"};
}

}  // namespace

FDC_TEST(evidence, a_valid_completion_record_is_accepted) {
  Limits limits;
  const CompletionEvidence evidence = valid_completion();
  FDC_CHECK(evidence.validate(limits).ok());
  FDC_CHECK(is_terminal_completion(evidence.state));
  FDC_CHECK(is_terminal_completion(CompletionState::kDrainedWithResiduals));
  FDC_CHECK(!is_terminal_completion(CompletionState::kAcknowledged));

  CompletionEvidence with_residuals = evidence;
  with_residuals.state = CompletionState::kDrainedWithResiduals;
  with_residuals.residual_count = 3;
  FDC_CHECK(with_residuals.validate(limits).ok());
}

FDC_TEST(evidence, completion_validation_rejects_each_broken_field) {
  Limits limits;
  const CompletionEvidence valid = valid_completion();
  FDC_REQUIRE(valid.validate(limits).ok());

  CompletionEvidence zero_id = valid;
  zero_id.id = EvidenceId{0};
  FDC_CHECK_STATUS(zero_id.validate(limits), ErrorCode::kInvalidIdentity);

  CompletionEvidence unknown_state = valid;
  unknown_state.state = CompletionState::kUnknown;
  FDC_CHECK_STATUS(unknown_state.validate(limits), ErrorCode::kInvalidEnumValue);

  CompletionEvidence zero_generation = valid;
  zero_generation.generation = EvidenceGeneration{0};
  FDC_CHECK_STATUS(zero_generation.validate(limits), ErrorCode::kMissingRequiredField);

  CompletionEvidence zero_payload = valid;
  zero_payload.payload_digest = ContentDigest{};
  FDC_CHECK_STATUS(zero_payload.validate(limits), ErrorCode::kMissingRequiredField);

  CompletionEvidence zero_manifest = valid;
  zero_manifest.manifest_digest = ContentDigest{};
  FDC_CHECK_STATUS(zero_manifest.validate(limits), ErrorCode::kMissingRequiredField);

  CompletionEvidence zero_scope = valid;
  zero_scope.scope_manifest_digest = ContentDigest{};
  FDC_CHECK_STATUS(zero_scope.validate(limits), ErrorCode::kMissingRequiredField);

  CompletionEvidence incomplete_generations = valid;
  incomplete_generations.generations.topology = TopologyGeneration{0};
  FDC_CHECK_STATUS(incomplete_generations.validate(limits), ErrorCode::kMissingRequiredField);

  CompletionEvidence zero_observed_at = valid;
  zero_observed_at.observed_at = ObservationSequence{0};
  FDC_CHECK_STATUS(zero_observed_at.validate(limits), ErrorCode::kMissingRequiredField);

  CompletionEvidence unknown_count_with_value = valid;
  unknown_count_with_value.residual_count_known = false;
  unknown_count_with_value.residual_count = 4;
  FDC_CHECK_STATUS(unknown_count_with_value.validate(limits), ErrorCode::kConflictingField);

  CompletionEvidence residuals_known_zero = valid;
  residuals_known_zero.state = CompletionState::kDrainedWithResiduals;
  residuals_known_zero.residual_count_known = true;
  residuals_known_zero.residual_count = 0;
  FDC_CHECK_STATUS(residuals_known_zero.validate(limits), ErrorCode::kConflictingField);

  CompletionEvidence residuals_unknown = valid;
  residuals_unknown.state = CompletionState::kDrainedWithResiduals;
  residuals_unknown.residual_count_known = false;
  residuals_unknown.residual_count = 0;
  FDC_CHECK_STATUS(residuals_unknown.validate(limits), ErrorCode::kConflictingField);
}

FDC_TEST(evidence, an_unknown_residual_count_is_not_rendered_as_zero) {
  const CompletionEvidence known = valid_completion();
  CompletionEvidence unknown = known;
  unknown.residual_count_known = false;
  unknown.residual_count = 0;

  FDC_CHECK(known.to_canonical().find("residuals=0") != std::string::npos);
  FDC_CHECK(unknown.to_canonical().find("residuals=unknown") != std::string::npos);
  FDC_CHECK(known.to_canonical() != unknown.to_canonical());
}

FDC_TEST(evidence, enumeration_validation_rejects_each_broken_field) {
  Limits limits;
  const EnumerationEvidence valid = valid_enumeration();
  FDC_REQUIRE(valid.validate(limits).ok());

  EnumerationEvidence zero_generation = valid;
  zero_generation.generation = EvidenceGeneration{0};
  FDC_CHECK_STATUS(zero_generation.validate(limits), ErrorCode::kMissingRequiredField);

  EnumerationEvidence not_enumerated = valid;
  not_enumerated.coverage = CoverageState::kNotEnumerated;
  FDC_CHECK_STATUS(not_enumerated.validate(limits), ErrorCode::kInvalidEnumValue);

  EnumerationEvidence zero_manifest = valid;
  zero_manifest.manifest_digest = ContentDigest{};
  FDC_CHECK_STATUS(zero_manifest.validate(limits), ErrorCode::kMissingRequiredField);

  EnumerationEvidence zero_scope = valid;
  zero_scope.scope_manifest_digest = ContentDigest{};
  FDC_CHECK_STATUS(zero_scope.validate(limits), ErrorCode::kMissingRequiredField);

  EnumerationEvidence incomplete_generations = valid;
  incomplete_generations.generations.capacity = CapacityGeneration{0};
  FDC_CHECK_STATUS(incomplete_generations.validate(limits), ErrorCode::kMissingRequiredField);

  EnumerationEvidence zero_observed_at = valid;
  zero_observed_at.observed_at = ObservationSequence{0};
  FDC_CHECK_STATUS(zero_observed_at.validate(limits), ErrorCode::kMissingRequiredField);
}

FDC_TEST(evidence, a_non_advancing_evidence_generation_is_rejected_as_stale) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  FDC_REQUIRE_OK(coordinator.ingest_completion(completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                                  EvidenceGeneration{5})));
  FDC_CHECK_CODE(coordinator.ingest_completion(completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                                  EvidenceGeneration{5})),
                 ErrorCode::kStaleEvidence);
  FDC_CHECK_CODE(coordinator.ingest_completion(completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                                  EvidenceGeneration{4})),
                 ErrorCode::kStaleEvidence);
  FDC_CHECK_CODE(coordinator.ingest_completion(completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                                  EvidenceGeneration{0})),
                 ErrorCode::kMissingRequiredField);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(view.value().completions[0].present);
  FDC_CHECK_EQ(view.value().completions[0].evidence.generation, EvidenceGeneration{5});
}

FDC_TEST(evidence, incompatible_evidence_is_still_recorded) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  IngestCompletionRequest request = completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                       EvidenceGeneration{2});
  request.generations.dependency = DependencyGeneration{99};
  Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(request);
  FDC_REQUIRE_OK(ingested);
  FDC_CHECK(!ingested.value().compatible);
  FDC_CHECK_EQ(ingested.value().rejection, ErrorCode::kGenerationIncompatible);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(view.value().completions[0].present);
  FDC_CHECK(!view.value().completions[0].compatible);
  FDC_CHECK_EQ(view.value().completions[0].rejection, ErrorCode::kGenerationIncompatible);
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"enumerating"});

  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(evaluation);
  FDC_CHECK_EQ(evaluation.value().domains[domain_index(OwnerDomain::kAsi)].blocking_code,
               ErrorCode::kGenerationIncompatible);
}

FDC_TEST(evidence, a_manifest_disagreement_reports_evidence_mismatch) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  IngestCompletionRequest request = completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                       EvidenceGeneration{2});
  request.manifest_digest = digest_text("a-different-consumer-manifest");
  Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(request);
  FDC_REQUIRE_OK(ingested);
  FDC_CHECK(!ingested.value().compatible);
  FDC_CHECK_EQ(ingested.value().rejection, ErrorCode::kEvidenceMismatch);
}

FDC_TEST(evidence, a_scope_disagreement_reports_scope_manifest_mismatch) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  IngestCompletionRequest request = completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                       EvidenceGeneration{2});
  request.scope_manifest_digest = digest_text("a-different-physical-membership");
  Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(request);
  FDC_REQUIRE_OK(ingested);
  FDC_CHECK(!ingested.value().compatible);
  FDC_CHECK_EQ(ingested.value().rejection, ErrorCode::kScopeManifestMismatch);
}

FDC_TEST(evidence, evidence_at_or_before_the_fence_floor_is_stale) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  FenceSafeToRemoveRequest fence;
  fence.context = context_for(coordinator, PlanId{1});
  fence.reason = FenceReason::kOperatorFence;
  fence.detail = "the operator withdrew the previous answer";
  FDC_REQUIRE_OK(coordinator.fence_safe_to_remove(fence));

  Result<DrainPlanSnapshot> fenced = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(fenced);
  FDC_REQUIRE(fenced.value().fence.has_value());
  const ObservationSequence floor = fenced.value().fence.value().floor;
  FDC_CHECK(!floor.is_default());

  IngestCompletionRequest request = completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                       EvidenceGeneration{2});
  request.observed_at = floor;
  Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(request);
  FDC_REQUIRE_OK(ingested);
  FDC_CHECK(!ingested.value().compatible);
  FDC_CHECK_EQ(ingested.value().rejection, ErrorCode::kStaleEvidence);
  // No removal authority was live, so nothing had to be withdrawn.
  FDC_CHECK(!ingested.value().fenced);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(view.value().completions[0].present);
  FDC_CHECK(!view.value().completions[0].compatible);
  FDC_CHECK_EQ(view.value().completions[0].rejection, ErrorCode::kStaleEvidence);
}

FDC_TEST(evidence, a_compatible_drained_report_proves_the_domain_complete) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(
      completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{2}));
  FDC_REQUIRE_OK(ingested);
  FDC_CHECK(ingested.value().compatible);
  FDC_CHECK_EQ(ingested.value().rejection, ErrorCode::kOk);
  FDC_CHECK(ingested.value().evidence.residual_count_known);
  FDC_CHECK_EQ(ingested.value().evidence.residual_count, 0ULL);

  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(evaluation);
  const DomainAssessment& assessment = evaluation.value().domains[domain_index(OwnerDomain::kAsi)];
  FDC_CHECK_EQ(std::string{to_token(assessment.verdict)}, std::string{"proven-complete"});
  FDC_CHECK_EQ(assessment.blocking_code, ErrorCode::kOk);
  FDC_CHECK_EQ(assessment.residual_count, 0ULL);
  FDC_CHECK(assessment.enumeration_complete);
  FDC_CHECK(assessment.enumeration_manifest_matches);
  FDC_CHECK(assessment.completion_compatible);
  FDC_CHECK(evaluation.value().all_required_proven);
  FDC_CHECK_EQ(std::string{to_token(evaluation.value().verdict)}, std::string{"granted"});

  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"drained"});
}

FDC_TEST(evidence, a_drained_report_with_an_unknown_count_does_not_prove_the_domain) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  IngestCompletionRequest request = completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                       EvidenceGeneration{2});
  request.residual_count_known = false;
  FDC_REQUIRE_OK(coordinator.ingest_completion(request));

  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(evaluation);
  const DomainAssessment& assessment = evaluation.value().domains[domain_index(OwnerDomain::kAsi)];
  FDC_CHECK_EQ(std::string{to_token(assessment.verdict)}, std::string{"unknown"});
  FDC_CHECK_EQ(assessment.blocking_code, ErrorCode::kUnknownResidualCount);
  FDC_CHECK(!evaluation.value().all_required_proven);
  FDC_CHECK_EQ(std::string{to_token(evaluation.value().verdict)}, std::string{"denied"});
  FDC_CHECK_EQ(state_of(coordinator, PlanId{1}), std::string{"enumerating"});
}

FDC_TEST(evidence, an_acknowledgement_is_not_an_effect) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  IngestCompletionRequest request = completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                       EvidenceGeneration{2});
  request.state = CompletionState::kAcknowledged;
  Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(request);
  FDC_REQUIRE_OK(ingested);
  FDC_CHECK(ingested.value().compatible);

  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(evaluation);
  const DomainAssessment& assessment = evaluation.value().domains[domain_index(OwnerDomain::kAsi)];
  FDC_CHECK_EQ(std::string{to_token(assessment.verdict)}, std::string{"incomplete"});
  FDC_CHECK_EQ(assessment.blocking_code, ErrorCode::kAcknowledgementIsNotEffect);
  FDC_CHECK(!evaluation.value().all_required_proven);
  FDC_CHECK(state_of(coordinator, PlanId{1}) != std::string{"drained"});
}

FDC_TEST(evidence, a_draining_report_is_not_a_drain) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(prepare_enumerated_plan(coordinator, PlanId{1}));

  IngestCompletionRequest request = completion_request(coordinator, PlanId{1}, OwnerDomain::kAsi,
                                                       EvidenceGeneration{2});
  request.state = CompletionState::kDraining;
  Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(request);
  FDC_REQUIRE_OK(ingested);
  FDC_CHECK(ingested.value().compatible);

  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(evaluation);
  const DomainAssessment& assessment = evaluation.value().domains[domain_index(OwnerDomain::kAsi)];
  FDC_CHECK_EQ(std::string{to_token(assessment.verdict)}, std::string{"incomplete"});
  FDC_CHECK_EQ(assessment.blocking_code, ErrorCode::kNotDrained);
  FDC_CHECK(!evaluation.value().all_required_proven);
  FDC_CHECK_EQ(std::string{to_token(evaluation.value().verdict)}, std::string{"denied"});
}
