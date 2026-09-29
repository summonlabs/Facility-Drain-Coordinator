// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 04: the residual ledger preserves unknowns.
//
// A compatible drain report is not the end of the story: anything unresolved
// stays on the ledger and keeps withholding authority. Unknown entries are
// counted separately from known ones, and an entry is only closed by naming the
// evidence generation that resolved it. The verdict is recomputed after every
// change, so the ledger emptying is what re-grants the answer.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"
#include "facilitydrain/snapshot.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace facilitydrain;

constexpr std::int64_t kFixedMilliseconds = 1700000000000;

GenerationSet complete_generations() {
  GenerationSet set;
  set.scope = ScopeGeneration{41};
  set.dependency = DependencyGeneration{42};
  set.reservation = ReservationGeneration{43};
  set.obligation = ObligationGeneration{44};
  set.policy = PolicyGeneration{45};
  set.topology = TopologyGeneration{46};
  set.maintenance = MaintenanceGeneration{47};
  set.capacity = CapacityGeneration{48};
  set.hardware = HardwareGeneration{49};
  set.firmware = FirmwareGeneration{50};
  return set;
}

ConsumerRecord workload(std::uint64_t obligation, std::string label) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{6};
  record.strength = ObligationStrength::kMandatory;
  record.label = std::move(label);
  record.source = "asi-controller";
  return record;
}

std::uint64_t next_observation(const Coordinator& coordinator, PlanId id) {
  auto view = coordinator.plan(id);
  if (!view) {
    return coordinator.observation_sequence().value() + 1U;
  }
  return view.value().last_observation.value() + 1U;
}

MutationContext context_for(const Coordinator& coordinator, PlanId id) {
  auto view = coordinator.plan(id);
  MutationContext context;
  context.plan = id;
  context.expected_revision = view.value().spec.revision;
  context.incarnation = coordinator.incarnation();
  context.expected_epoch = coordinator.control_epoch();
  context.observation = ObservationSequence{next_observation(coordinator, id)};
  context.principal = "example-04";
  return context;
}

void print_ledger(const Coordinator& coordinator, PlanId id) {
  auto ledger = coordinator.residuals(id);
  if (!ledger) {
    std::cout << "residuals failed: " << ledger.error().to_text() << '\n';
    return;
  }
  std::cout << "ledger total " << ledger.value().entries.size() << " open "
            << ledger.value().open_count() << " unknown " << ledger.value().unknown_count() << '\n';
  for (const ResidualEntry& entry : ledger.value().entries) {
    std::cout << "  entry " << entry.to_canonical() << '\n';
  }
}

void print_verdict(const Coordinator& coordinator, PlanId id) {
  auto evaluation = coordinator.evaluate_safe_to_remove(id);
  if (!evaluation) {
    std::cout << "evaluate failed: " << evaluation.error().to_text() << '\n';
    return;
  }
  auto view = coordinator.plan(id);
  std::cout << "verdict " << to_token(evaluation.value().verdict) << " primary "
            << to_token(evaluation.value().primary_blocking_code) << " state "
            << to_token(view.value().state) << '\n';
}

}  // namespace

int main() {
  EphemeralOptions options;
  options.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(kFixedMilliseconds)};
  auto opened = Coordinator::open_ephemeral(options);
  if (!opened) {
    std::cout << "open failed: " << opened.error().to_text() << '\n';
    return 1;
  }
  Coordinator coordinator = std::move(opened).value();

  const PlanId plan_id{1};
  const DrainScope scope{ScopeKind::kRack, 12};
  const GenerationSet generations = complete_generations();
  const std::vector<ConsumerRecord> consumers{workload(1001, "training-job"),
                                             workload(1002, "inference-job")};

  CreatePlanRequest create;
  create.context.plan = plan_id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "example-04";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  create.generations = generations;
  create.policy_id = PolicyId{7170};
  create.policy_digest = digest_text("policy-7170");
  create.consumers = consumers;
  create.label = "drain-rack-12";
  auto created = coordinator.create_plan(create);
  if (!created) {
    std::cout << "create failed: " << created.error().to_text() << '\n';
    return 1;
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();
  const ContentDigest manifest_digest =
      created.value().plan.spec.bindings.manifest_digest(OwnerDomain::kAsi).value();

  RecordEnumerationRequest enumeration;
  enumeration.context = context_for(coordinator, plan_id);
  enumeration.domain = OwnerDomain::kAsi;
  enumeration.coverage = CoverageState::kComplete;
  enumeration.generation = EvidenceGeneration{1};
  enumeration.observed_at = enumeration.context.observation;
  enumeration.scope_manifest_digest = scope_digest;
  enumeration.generations = generations;
  enumeration.source = "asi-controller";
  enumeration.consumers = consumers;
  auto recorded = coordinator.record_enumeration(enumeration);
  if (!recorded) {
    std::cout << "enumerate failed: " << recorded.error().to_text() << '\n';
    return 1;
  }

  IssueRequestsRequest issue;
  issue.context = context_for(coordinator, plan_id);
  issue.domains = DomainMask::of(OwnerDomain::kAsi);
  auto issued = coordinator.issue_requests(issue);
  if (!issued) {
    std::cout << "issue failed: " << issued.error().to_text() << '\n';
    return 1;
  }
  const DrainRequestId request_id = issued.value().to_deliver.front().id;

  ConfirmDeliveryRequest delivery;
  delivery.context = context_for(coordinator, plan_id);
  delivery.request = request_id;
  delivery.delivery_reference = "handoff-0011";
  auto delivered = coordinator.confirm_delivery(delivery);
  if (!delivered) {
    std::cout << "deliver failed: " << delivered.error().to_text() << '\n';
    return 1;
  }
  RecordAcknowledgementRequest acknowledgement;
  acknowledgement.context = context_for(coordinator, plan_id);
  acknowledgement.request = request_id;
  acknowledgement.acknowledging_system = "asi-controller";
  auto acknowledged = coordinator.record_acknowledgement(acknowledgement);
  if (!acknowledged) {
    std::cout << "acknowledge failed: " << acknowledged.error().to_text() << '\n';
    return 1;
  }

  IngestCompletionRequest completion;
  completion.context = context_for(coordinator, plan_id);
  completion.domain = OwnerDomain::kAsi;
  completion.state = CompletionState::kDrained;
  completion.generation = EvidenceGeneration{2};
  completion.observed_at = completion.context.observation;
  completion.payload_digest = digest_text("asi-drained-report");
  completion.manifest_digest = manifest_digest;
  completion.scope_manifest_digest = scope_digest;
  completion.generations = generations;
  completion.residual_count_known = true;
  completion.residual_count = 0;
  completion.source = "asi-controller";
  auto ingested = coordinator.ingest_completion(completion);
  if (!ingested) {
    std::cout << "ingest failed: " << ingested.error().to_text() << '\n';
    return 1;
  }

  std::cout << "example 04: the residual ledger preserves unknowns\n";
  std::cout << "completion compatible " << (ingested.value().compatible ? "yes" : "no") << '\n';
  print_ledger(coordinator, plan_id);
  print_verdict(coordinator, plan_id);

  // Enumeration coverage turned out to be partial after all. The owner said so,
  // and the ledger records it as an open unknown-shaped fact about the domain
  // rather than as an absence of a complaint.
  RecordResidualRequest scope_residual;
  scope_residual.context = context_for(coordinator, plan_id);
  scope_residual.entry.domain = OwnerDomain::kAsi;
  scope_residual.entry.kind = ResidualKind::kEnumerationIncomplete;
  scope_residual.entry.detail = "the owner narrowed coverage after the report";
  auto scope_recorded = coordinator.record_residual(scope_residual);
  if (!scope_recorded) {
    std::cout << "residual record failed: " << scope_recorded.error().to_text() << '\n';
    return 1;
  }
  std::cout << "recorded " << scope_recorded.value().entry.to_canonical() << '\n';
  print_ledger(coordinator, plan_id);
  print_verdict(coordinator, plan_id);

  // One obligation nobody can identify. It is an unknown, and unknowns are
  // counted separately because they can never be resolved by assuming zero.
  RecordResidualRequest unknown_residual;
  unknown_residual.context = context_for(coordinator, plan_id);
  unknown_residual.entry.domain = OwnerDomain::kAsi;
  unknown_residual.entry.obligation = ObligationId{1002};
  unknown_residual.entry.kind = ResidualKind::kObligationUnknown;
  unknown_residual.entry.detail = "the owner cannot determine whether 1002 is still present";
  auto unknown_recorded = coordinator.record_residual(unknown_residual);
  if (!unknown_recorded) {
    std::cout << "residual record failed: " << unknown_recorded.error().to_text() << '\n';
    return 1;
  }
  std::cout << "recorded " << unknown_recorded.value().entry.to_canonical() << '\n';
  print_ledger(coordinator, plan_id);
  print_verdict(coordinator, plan_id);

  ResolveResidualRequest resolve_scope;
  resolve_scope.context = context_for(coordinator, plan_id);
  resolve_scope.domain = OwnerDomain::kAsi;
  resolve_scope.kind = ResidualKind::kEnumerationIncomplete;
  resolve_scope.resolution_evidence_generation = EvidenceGeneration{2};
  resolve_scope.detail = "coverage re-confirmed complete at generation 2";
  auto scope_resolved = coordinator.resolve_residual(resolve_scope);
  if (!scope_resolved) {
    std::cout << "residual resolve failed: " << scope_resolved.error().to_text() << '\n';
    return 1;
  }
  std::cout << "resolved " << scope_resolved.value().entry.to_canonical() << '\n';
  print_ledger(coordinator, plan_id);
  print_verdict(coordinator, plan_id);

  ResolveResidualRequest resolve_unknown;
  resolve_unknown.context = context_for(coordinator, plan_id);
  resolve_unknown.domain = OwnerDomain::kAsi;
  resolve_unknown.obligation = ObligationId{1002};
  resolve_unknown.kind = ResidualKind::kObligationUnknown;
  resolve_unknown.resolution_evidence_generation = EvidenceGeneration{2};
  resolve_unknown.detail = "the owner proved 1002 relinquished at generation 2";
  auto unknown_resolved = coordinator.resolve_residual(resolve_unknown);
  if (!unknown_resolved) {
    std::cout << "residual resolve failed: " << unknown_resolved.error().to_text() << '\n';
    return 1;
  }
  std::cout << "resolved " << unknown_resolved.value().entry.to_canonical() << '\n';
  print_ledger(coordinator, plan_id);
  print_verdict(coordinator, plan_id);
  return 0;
}
