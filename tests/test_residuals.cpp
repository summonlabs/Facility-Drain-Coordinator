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
#include "facilitydrain/residual.hpp"
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"
#include "test_harness.hpp"

#include <array>
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
  context.principal = "residual-tests";
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
                                             DomainMask required = DomainMask::of(OwnerDomain::kAsi)) {
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
  request.label = "residuals";
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
  return coordinator.record_enumeration(request);
}

[[nodiscard]] Result<RecordResidualOutcome> record(Coordinator& coordinator, PlanId id, ObligationId obligation,
                                                   OwnerDomain domain, ResidualKind kind, std::string detail) {
  RecordResidualRequest request;
  request.context = context_for(coordinator, id);
  request.entry.obligation = obligation;
  request.entry.domain = domain;
  request.entry.kind = kind;
  request.entry.generation = ObligationGeneration{100};
  request.entry.detail = std::move(detail);
  return coordinator.record_residual(request);
}

[[nodiscard]] Result<ResolveResidualOutcome> resolve(Coordinator& coordinator, PlanId id,
                                                     ObligationId obligation, OwnerDomain domain,
                                                     ResidualKind kind, EvidenceGeneration generation,
                                                     std::string detail) {
  ResolveResidualRequest request;
  request.context = context_for(coordinator, id);
  request.obligation = obligation;
  request.domain = domain;
  request.kind = kind;
  request.resolution_evidence_generation = generation;
  request.detail = std::move(detail);
  return coordinator.resolve_residual(request);
}

[[nodiscard]] Result<IngestCompletionOutcome> drain_domain(Coordinator& coordinator, PlanId id,
                                                           OwnerDomain domain, EvidenceGeneration generation,
                                                           std::uint64_t residual_count, bool count_known) {
  IngestCompletionRequest request;
  request.context = context_for(coordinator, id);
  request.domain = domain;
  request.state = count_known && residual_count == 0 ? CompletionState::kDrained
                                                     : CompletionState::kDrainedWithResiduals;
  request.generation = generation;
  request.observed_at = context_for(coordinator, id).observation;
  request.payload_digest = digest_text("completion-payload");
  request.manifest_digest = bound_manifest(coordinator, id, domain);
  request.scope_manifest_digest = scope_digest(coordinator, id);
  request.generations = complete_generations();
  request.residual_count_known = count_known;
  request.residual_count = residual_count;
  request.source = std::string{to_token(domain)};
  return coordinator.ingest_completion(request);
}

[[nodiscard]] Result<GrantSafeToRemoveOutcome> grant(Coordinator& coordinator, PlanId id) {
  GrantSafeToRemoveRequest request;
  request.context = context_for(coordinator, id);
  request.granted_by = "operator";
  return coordinator.grant_safe_to_remove(request);
}

[[nodiscard]] Result<ResidualLedger> ledger_of(Coordinator& coordinator, PlanId id) {
  return coordinator.residuals(id);
}

constexpr std::array<ResidualKind, 10> kAllResidualKinds{
    ResidualKind::kObligationActive,      ResidualKind::kObligationUnknown,
    ResidualKind::kOwnerRefused,          ResidualKind::kEnumerationIncomplete,
    ResidualKind::kEvidenceMissing,       ResidualKind::kResidualCountUnknown,
    ResidualKind::kRequestUnacknowledged, ResidualKind::kDomainFailed,
    ResidualKind::kEvidenceStale,         ResidualKind::kProtectedObligation};

}  // namespace

FDC_TEST(residuals, recording_forces_the_open_state) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  RecordResidualRequest request;
  request.context = context_for(coordinator, PlanId{1});
  request.entry.obligation = ObligationId{1001};
  request.entry.domain = OwnerDomain::kAsi;
  request.entry.kind = ResidualKind::kObligationActive;
  request.entry.state = ResidualState::kRelinquished;
  request.entry.resolved_at = ObservationSequence{7};
  request.entry.resolution_evidence_generation = EvidenceGeneration{3};
  request.entry.detail = "the owner still reports the workload";
  Result<RecordResidualOutcome> recorded = coordinator.record_residual(request);
  FDC_REQUIRE_OK(recorded);
  FDC_CHECK_EQ(std::string{to_token(recorded.value().entry.state)}, std::string{"open"});
  FDC_CHECK(recorded.value().entry.resolved_at.is_default());
  FDC_CHECK(recorded.value().entry.resolution_evidence_generation.is_default());
  FDC_CHECK_EQ(recorded.value().entry.recorded_at, request.context.observation);
  FDC_CHECK(!recorded.value().fenced);

  Result<ResidualLedger> ledger = ledger_of(coordinator, PlanId{1});
  FDC_REQUIRE_OK(ledger);
  FDC_CHECK_EQ(ledger.value().entries.size(), std::size_t{1});
  FDC_CHECK_EQ(ledger.value().open_count(), 1U);
  FDC_CHECK(ledger.value().has_open(OwnerDomain::kAsi));
}

FDC_TEST(residuals, a_second_record_of_the_same_identity_replaces_the_entry) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "first statement"));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kOwnerRefused, "a different reason"));
  Result<ResidualLedger> two_kinds = ledger_of(coordinator, PlanId{1});
  FDC_REQUIRE_OK(two_kinds);
  FDC_CHECK_EQ(two_kinds.value().entries.size(), std::size_t{2});

  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "second statement"));
  Result<ResidualLedger> replaced = ledger_of(coordinator, PlanId{1});
  FDC_REQUIRE_OK(replaced);
  FDC_CHECK_EQ(replaced.value().entries.size(), std::size_t{2});
  bool found = false;
  for (const ResidualEntry& entry : replaced.value().entries) {
    if (entry.domain == OwnerDomain::kAsi && entry.obligation == ObligationId{1001} &&
        entry.kind == ResidualKind::kObligationActive) {
      found = true;
      FDC_CHECK_EQ(entry.detail, std::string{"second statement"});
      FDC_CHECK_EQ(std::string{to_token(entry.state)}, std::string{"open"});
    }
  }
  FDC_CHECK(found);
}

FDC_TEST(residuals, ledger_counts_are_documented) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{}));

  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "active"));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1002}, OwnerDomain::kAsi,
                        ResidualKind::kObligationUnknown, "unknown"));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1002}, OwnerDomain::kAsi,
                        ResidualKind::kOwnerRefused, "refused"));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{0}, OwnerDomain::kAsi,
                        ResidualKind::kEvidenceMissing, "no completion evidence"));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{2001}, OwnerDomain::kDfi,
                        ResidualKind::kObligationActive, "dfi active"));

  Result<ResidualLedger> ledger = ledger_of(coordinator, PlanId{1});
  FDC_REQUIRE_OK(ledger);
  FDC_CHECK_EQ(ledger.value().entries.size(), std::size_t{5});
  FDC_CHECK_EQ(ledger.value().open_count(), 5U);
  FDC_CHECK_EQ(ledger.value().open_count(OwnerDomain::kAsi), 4U);
  FDC_CHECK_EQ(ledger.value().unknown_count(), 2U);
  FDC_CHECK_EQ(ledger.value().unknown_count(OwnerDomain::kAsi), 2U);
  FDC_CHECK_EQ(ledger.value().unknown_count(OwnerDomain::kDfi), 0U);
  FDC_CHECK_EQ(ledger.value().known_obligation_count(OwnerDomain::kAsi), 2U);
  FDC_CHECK_EQ(ledger.value().known_obligation_count(OwnerDomain::kDfi), 1U);
  FDC_CHECK_EQ(ledger.value().relinquished_obligation_count(OwnerDomain::kAsi), 0U);
  FDC_CHECK(!ledger.value().has_open(OwnerDomain::kFacility));

  FDC_REQUIRE_OK(resolve(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                         ResidualKind::kObligationActive, EvidenceGeneration{1}, "the workload is gone"));
  Result<ResidualLedger> after = ledger_of(coordinator, PlanId{1});
  FDC_REQUIRE_OK(after);
  FDC_CHECK_EQ(after.value().open_count(OwnerDomain::kAsi), 3U);
  FDC_CHECK_EQ(after.value().unknown_count(), 2U);
  FDC_CHECK_EQ(after.value().known_obligation_count(OwnerDomain::kAsi), 2U);
  FDC_CHECK_EQ(after.value().relinquished_obligation_count(OwnerDomain::kAsi), 1U);
  FDC_CHECK_EQ(after.value().relinquished_obligation_count(OwnerDomain::kDfi), 0U);
}

FDC_TEST(residuals, resolving_a_residual_that_does_not_exist_fails) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "active"));

  FDC_CHECK_CODE(resolve(coordinator, PlanId{1}, ObligationId{1002}, OwnerDomain::kAsi,
                         ResidualKind::kObligationActive, EvidenceGeneration{1}, "never recorded"),
                 ErrorCode::kObligationNotDeclared);
  FDC_CHECK_CODE(resolve(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kDfi,
                         ResidualKind::kObligationActive, EvidenceGeneration{1}, "wrong domain"),
                 ErrorCode::kObligationNotDeclared);
  FDC_CHECK_CODE(resolve(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                         ResidualKind::kOwnerRefused, EvidenceGeneration{1}, "wrong kind"),
                 ErrorCode::kObligationNotDeclared);
  FDC_CHECK_CODE(resolve(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                         ResidualKind::kObligationActive, EvidenceGeneration{0}, "no evidence named"),
                 ErrorCode::kMissingRequiredField);
}

FDC_TEST(residuals, resolving_is_idempotent) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{}));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "active"));

  Result<ResolveResidualOutcome> first = resolve(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                                                 ResidualKind::kObligationActive, EvidenceGeneration{1},
                                                 "the workload is gone");
  FDC_REQUIRE_OK(first);
  FDC_CHECK_EQ(std::string{to_token(first.value().entry.state)}, std::string{"relinquished"});
  FDC_CHECK_EQ(first.value().entry.resolution_evidence_generation, EvidenceGeneration{1});
  FDC_CHECK_EQ(first.value().entry.detail, std::string{"the workload is gone"});
  const ObservationSequence resolved_at = first.value().entry.resolved_at;
  FDC_CHECK(!resolved_at.is_default());

  const std::uint64_t commit_after_first = coordinator.commit_sequence().value();
  Result<ResolveResidualOutcome> repeated = resolve(coordinator, PlanId{1}, ObligationId{1001},
                                                    OwnerDomain::kAsi, ResidualKind::kObligationActive,
                                                    EvidenceGeneration{1}, "a second statement");
  FDC_REQUIRE_OK(repeated);
  FDC_CHECK_EQ(std::string{to_token(repeated.value().entry.state)}, std::string{"relinquished"});
  FDC_CHECK_EQ(repeated.value().entry.resolved_at, resolved_at);
  FDC_CHECK_EQ(repeated.value().entry.detail, std::string{"the workload is gone"});
  FDC_CHECK_EQ(coordinator.commit_sequence().value(), commit_after_first);
  FDC_CHECK_EQ(coordinator.residuals(PlanId{1}).value().relinquished_obligation_count(OwnerDomain::kAsi), 1U);
}

FDC_TEST(residuals, resolution_cannot_cite_evidence_that_was_never_recorded) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1},
                           std::vector<ConsumerRecord>{}));
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "active"));

  FDC_CHECK_CODE(resolve(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                         ResidualKind::kObligationActive, EvidenceGeneration{5}, "evidence nobody produced"),
                 ErrorCode::kInvalidGenerationOrder);

  Result<ResidualLedger> ledger = ledger_of(coordinator, PlanId{1});
  FDC_REQUIRE_OK(ledger);
  FDC_CHECK_EQ(ledger.value().open_count(), 1U);
  FDC_CHECK_EQ(ledger.value().relinquished_obligation_count(OwnerDomain::kAsi), 0U);
}

FDC_TEST(residuals, the_bound_refuses_a_new_entry_but_allows_an_update) {
  Limits limits;
  limits.max_residuals_per_plan = 1;
  Result<Coordinator> opened = open_coordinator(limits);
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "first"));
  FDC_CHECK_CODE(record(coordinator, PlanId{1}, ObligationId{1002}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "a new obligation"),
                 ErrorCode::kTooManyEntries);
  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                        ResidualKind::kObligationActive, "an update of the existing entry"));

  Result<ResidualLedger> ledger = ledger_of(coordinator, PlanId{1});
  FDC_REQUIRE_OK(ledger);
  FDC_CHECK_EQ(ledger.value().entries.size(), std::size_t{1});
  FDC_CHECK_EQ(ledger.value().entries.front().detail, std::string{"an update of the existing entry"});
}

FDC_TEST(residuals, reopening_a_relinquished_residual_is_a_new_obligation) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  const std::vector<ConsumerRecord> consumers{asi_obligation(ObligationId{1001})};
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, consumers)));
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, consumers));
  FDC_REQUIRE_OK(drain_domain(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{2}, 0, true));
  FDC_REQUIRE_OK(grant(coordinator, PlanId{1}));
  FDC_CHECK_EQ(std::string{to_token(coordinator.plan(PlanId{1}).value().state)},
               std::string{"safe-to-remove"});

  // A new residual recorded while authority is live withdraws it.
  Result<RecordResidualOutcome> opened_entry =
      record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi, ResidualKind::kObligationActive,
             "the workload is present again");
  FDC_REQUIRE_OK(opened_entry);
  FDC_CHECK(opened_entry.value().fenced);
  FDC_REQUIRE(opened_entry.value().fence.has_value());
  FDC_CHECK_EQ(std::string{to_token(opened_entry.value().fence.value().reason)},
               std::string{"evidence-superseded"});
  FDC_CHECK(!coordinator.plan(PlanId{1}).value().grant.has_value());

  FDC_REQUIRE_OK(resolve(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi,
                         ResidualKind::kObligationActive, EvidenceGeneration{2}, "the workload is gone"));

  // The fence moved the floor past the evidence the first answer was computed
  // from, so a fresh grant needs fresh evidence rather than the old records.
  FDC_REQUIRE_OK(enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{3}, consumers));
  FDC_REQUIRE_OK(drain_domain(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{4}, 0, true));
  FDC_REQUIRE_OK(grant(coordinator, PlanId{1}));
  FDC_CHECK_EQ(std::string{to_token(coordinator.plan(PlanId{1}).value().state)},
               std::string{"safe-to-remove"});

  Result<RecordResidualOutcome> reopened =
      record(coordinator, PlanId{1}, ObligationId{1001}, OwnerDomain::kAsi, ResidualKind::kObligationActive,
             "the workload is present again");
  FDC_REQUIRE_OK(reopened);
  FDC_CHECK(reopened.value().fenced);
  FDC_REQUIRE(reopened.value().fence.has_value());
  FDC_CHECK_EQ(std::string{to_token(reopened.value().fence.value().reason)}, std::string{"new-obligation"});
  FDC_CHECK_EQ(std::string{to_token(reopened.value().entry.state)}, std::string{"open"});
  FDC_CHECK_EQ(coordinator.residuals(PlanId{1}).value().open_count(OwnerDomain::kAsi), 1U);

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(!view.value().grant.has_value());
  FDC_CHECK(!view.value().grant_live);
  FDC_CHECK_EQ(std::string{to_token(view.value().state)}, std::string{"residuals-present"});
}

FDC_TEST(residuals, unknown_residual_kinds_are_classified) {
  const std::array<bool, 10> expected{false, true,  false, true,  true,
                                      true,  false, false, false, false};
  for (std::size_t index = 0; index < kAllResidualKinds.size(); ++index) {
    FDC_CHECK_EQ(is_unknown_residual_kind(kAllResidualKinds[index]), expected[index]);
  }
  FDC_CHECK_EQ(residual_kind_from_token(to_token(ResidualKind::kProtectedObligation)).value(),
               ResidualKind::kProtectedObligation);
}

FDC_TEST(residuals, an_unknown_kind_in_a_domain_that_is_not_required_still_blocks) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(plan_request(coordinator, PlanId{1}, std::vector<ConsumerRecord>{})));

  FDC_REQUIRE_OK(record(coordinator, PlanId{1}, ObligationId{2001}, OwnerDomain::kDfi,
                        ResidualKind::kEnumerationIncomplete, "dfi never enumerated the scope"));

  Result<DrainPlanSnapshot> view = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(view);
  FDC_CHECK(!view.value().required_domains.contains(OwnerDomain::kDfi));
  FDC_CHECK_EQ(ledger_of(coordinator, PlanId{1}).value().unknown_count(OwnerDomain::kDfi), 1U);
  FDC_CHECK_EQ(std::string{to_token(view.value().state)}, std::string{"residuals-present"});

  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(evaluation);
  FDC_CHECK_EQ(std::string{to_token(evaluation.value().verdict)}, std::string{"denied"});
  bool unknown_obligation_blocks = false;
  for (const ErrorCode code : evaluation.value().blocking_codes) {
    if (code == ErrorCode::kUnknownObligation) {
      unknown_obligation_blocks = true;
    }
  }
  FDC_CHECK(unknown_obligation_blocks);
}
