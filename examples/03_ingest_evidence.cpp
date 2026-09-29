// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 03: ingesting completion evidence.
//
// A completion report is stored whether or not it agrees with the plan
// binding, and the disagreement is reported rather than repaired. The verdict
// is recomputed from the newest record every time it is asked, so a report
// taken under a different generation set denies the verdict while it is the
// newest one, and a compatible report that supersedes it grants the verdict.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
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
  set.scope = ScopeGeneration{31};
  set.dependency = DependencyGeneration{32};
  set.reservation = ReservationGeneration{33};
  set.obligation = ObligationGeneration{34};
  set.policy = PolicyGeneration{35};
  set.topology = TopologyGeneration{36};
  set.maintenance = MaintenanceGeneration{37};
  set.capacity = CapacityGeneration{38};
  set.hardware = HardwareGeneration{39};
  set.firmware = FirmwareGeneration{40};
  return set;
}

ConsumerRecord workload(std::uint64_t obligation) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{5};
  record.strength = ObligationStrength::kMandatory;
  record.label = "training-job";
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
  context.principal = "example-03";
  return context;
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
            << to_token(view.value().state) << " evidence "
            << evaluation.value().evidence_digest.to_hex() << '\n';
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
  const DrainScope scope{ScopeKind::kRack, 9};
  const GenerationSet generations = complete_generations();
  const std::vector<ConsumerRecord> consumers{workload(1001)};

  CreatePlanRequest create;
  create.context.plan = plan_id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "example-03";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  create.generations = generations;
  create.policy_id = PolicyId{6160};
  create.policy_digest = digest_text("policy-6160");
  create.consumers = consumers;
  create.label = "drain-rack-9";
  auto created = coordinator.create_plan(create);
  if (!created) {
    std::cout << "create failed: " << created.error().to_text() << '\n';
    return 1;
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();
  const ContentDigest manifest_digest = created.value().plan.spec.bindings
                                            .manifest_digest(OwnerDomain::kAsi)
                                            .value();

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
  delivery.delivery_reference = "handoff-0007";
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

  std::cout << "example 03: ingesting completion evidence\n";
  std::cout << "acknowledged request " << to_string(request_id) << " state "
            << to_token(acknowledged.value().request.state) << '\n';
  print_verdict(coordinator, plan_id);

  // The owner drained under a different policy generation. The report is real
  // evidence about a different world, so it is stored and reported as
  // incompatible instead of being reconciled.
  GenerationSet drifted = generations;
  drifted.policy = PolicyGeneration{99};
  IngestCompletionRequest incompatible;
  incompatible.context = context_for(coordinator, plan_id);
  incompatible.domain = OwnerDomain::kAsi;
  incompatible.state = CompletionState::kDrained;
  incompatible.generation = EvidenceGeneration{2};
  incompatible.observed_at = incompatible.context.observation;
  incompatible.payload_digest = digest_text("asi-drained-report-drifted");
  incompatible.manifest_digest = manifest_digest;
  incompatible.scope_manifest_digest = scope_digest;
  incompatible.generations = drifted;
  incompatible.residual_count_known = true;
  incompatible.residual_count = 0;
  incompatible.source = "asi-controller";
  auto drifted_ingest = coordinator.ingest_completion(incompatible);
  if (!drifted_ingest) {
    std::cout << "ingest failed: " << drifted_ingest.error().to_text() << '\n';
    return 1;
  }
  std::cout << "ingest generation 2 compatible " << (drifted_ingest.value().compatible ? "yes" : "no")
            << " rejection " << to_token(drifted_ingest.value().rejection) << " stored "
            << (coordinator.plan(plan_id).value().completions[0].present ? "yes" : "no") << '\n';
  print_verdict(coordinator, plan_id);

  IngestCompletionRequest compatible;
  compatible.context = context_for(coordinator, plan_id);
  compatible.domain = OwnerDomain::kAsi;
  compatible.state = CompletionState::kDrained;
  compatible.generation = EvidenceGeneration{3};
  compatible.observed_at = compatible.context.observation;
  compatible.payload_digest = digest_text("asi-drained-report");
  compatible.manifest_digest = manifest_digest;
  compatible.scope_manifest_digest = scope_digest;
  compatible.generations = generations;
  compatible.residual_count_known = true;
  compatible.residual_count = 0;
  compatible.source = "asi-controller";
  auto ingested = coordinator.ingest_completion(compatible);
  if (!ingested) {
    std::cout << "ingest failed: " << ingested.error().to_text() << '\n';
    return 1;
  }
  std::cout << "ingest generation 3 compatible " << (ingested.value().compatible ? "yes" : "no")
            << " rejection " << to_token(ingested.value().rejection) << '\n';
  print_verdict(coordinator, plan_id);

  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, plan_id);
  grant.granted_by = "operator";
  auto granted = coordinator.grant_safe_to_remove(grant);
  if (!granted) {
    std::cout << "grant failed: " << granted.error().to_text() << '\n';
    return 1;
  }
  std::cout << "granted evidence " << granted.value().grant.value().evidence_digest.to_hex()
            << " state " << to_token(coordinator.plan(plan_id).value().state) << '\n';
  return 0;
}
