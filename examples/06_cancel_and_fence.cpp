// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 06: cancellation and fencing.
//
// Two ways an answer stops being true. A fence records a floor: evidence
// observed at or before it cannot support a new verdict, and the recorded grant
// is cleared. A cancellation is terminal for the attempt: the outstanding
// request is cancelled with it, every operation except a revision and an
// evaluation is refused, and the refusal names the plan rather than a field.

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
  set.scope = ScopeGeneration{61};
  set.dependency = DependencyGeneration{62};
  set.reservation = ReservationGeneration{63};
  set.obligation = ObligationGeneration{64};
  set.policy = PolicyGeneration{65};
  set.topology = TopologyGeneration{66};
  set.maintenance = MaintenanceGeneration{67};
  set.capacity = CapacityGeneration{68};
  set.hardware = HardwareGeneration{69};
  set.firmware = FirmwareGeneration{70};
  return set;
}

ConsumerRecord workload(std::uint64_t obligation, std::string label) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{9};
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
  context.principal = "example-06";
  return context;
}

bool create_plan(Coordinator& coordinator, PlanId id, const DrainScope& scope, PolicyId policy,
                 const std::vector<ConsumerRecord>& consumers, const GenerationSet& generations,
                 ContentDigest& scope_digest, ContentDigest& manifest_digest) {
  CreatePlanRequest create;
  create.context.plan = id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "example-06";
  create.id = id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  create.generations = generations;
  create.policy_id = policy;
  create.policy_digest = digest_text(std::string{"policy-"} + to_string(policy));
  create.consumers = consumers;
  create.label = "drain-" + format_scope(scope);
  auto created = coordinator.create_plan(create);
  if (!created) {
    std::cout << "create failed: " << created.error().to_text() << '\n';
    return false;
  }
  scope_digest = created.value().plan.spec.targets.digest();
  manifest_digest = created.value().plan.spec.bindings.manifest_digest(OwnerDomain::kAsi).value();
  return true;
}

bool complete_asi(Coordinator& coordinator, PlanId id, const std::vector<ConsumerRecord>& consumers,
                  const GenerationSet& generations, const ContentDigest& scope_digest,
                  const ContentDigest& manifest_digest) {
  RecordEnumerationRequest enumeration;
  enumeration.context = context_for(coordinator, id);
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
    return false;
  }
  IssueRequestsRequest issue;
  issue.context = context_for(coordinator, id);
  issue.domains = DomainMask::of(OwnerDomain::kAsi);
  auto issued = coordinator.issue_requests(issue);
  if (!issued) {
    std::cout << "issue failed: " << issued.error().to_text() << '\n';
    return false;
  }
  for (const DrainRequest& staged : issued.value().to_deliver) {
    ConfirmDeliveryRequest delivery;
    delivery.context = context_for(coordinator, id);
    delivery.request = staged.id;
    delivery.delivery_reference = "handoff-06";
    auto delivered = coordinator.confirm_delivery(delivery);
    if (!delivered) {
      std::cout << "deliver failed: " << delivered.error().to_text() << '\n';
      return false;
    }
    RecordAcknowledgementRequest acknowledgement;
    acknowledgement.context = context_for(coordinator, id);
    acknowledgement.request = staged.id;
    acknowledgement.acknowledging_system = "asi-controller";
    auto acknowledged = coordinator.record_acknowledgement(acknowledgement);
    if (!acknowledged) {
      std::cout << "acknowledge failed: " << acknowledged.error().to_text() << '\n';
      return false;
    }
  }
  IngestCompletionRequest completion;
  completion.context = context_for(coordinator, id);
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
    return false;
  }
  return true;
}

void print_verdict(const Coordinator& coordinator, PlanId id, const std::string& stage) {
  auto evaluation = coordinator.evaluate_safe_to_remove(id);
  if (!evaluation) {
    std::cout << stage << " evaluate failed: " << evaluation.error().to_text() << '\n';
    return;
  }
  auto view = coordinator.plan(id);
  std::cout << stage << " verdict " << to_token(evaluation.value().verdict) << " primary "
            << to_token(evaluation.value().primary_blocking_code) << " state "
            << to_token(view.value().state) << " floor " << to_string(evaluation.value().fence_floor)
            << '\n';
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

  const PlanId fenced_plan{1};
  const PlanId cancelled_plan{2};
  const GenerationSet generations = complete_generations();
  const std::vector<ConsumerRecord> consumers{workload(1001, "training-job")};
  ContentDigest scope_digest{};
  ContentDigest manifest_digest{};

  std::cout << "example 06: cancellation and fencing\n";
  if (!create_plan(coordinator, fenced_plan, DrainScope{ScopeKind::kRack, 31}, PolicyId{9190}, consumers,
                   generations, scope_digest, manifest_digest)) {
    return 1;
  }
  if (!complete_asi(coordinator, fenced_plan, consumers, generations, scope_digest, manifest_digest)) {
    return 1;
  }
  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, fenced_plan);
  grant.granted_by = "operator";
  auto granted = coordinator.grant_safe_to_remove(grant);
  if (!granted) {
    std::cout << "grant failed: " << granted.error().to_text() << '\n';
    return 1;
  }
  print_verdict(coordinator, fenced_plan, "granted");

  FenceSafeToRemoveRequest fence;
  fence.context = context_for(coordinator, fenced_plan);
  fence.reason = FenceReason::kOperatorFence;
  fence.detail = "the operator withdrew removal authority";
  auto fenced = coordinator.fence_safe_to_remove(fence);
  if (!fenced) {
    std::cout << "fence failed: " << fenced.error().to_text() << '\n';
    return 1;
  }
  std::cout << "fence reason " << to_token(fenced.value().fence.reason) << " floor "
            << to_string(fenced.value().fence.floor) << " had-live-grant "
            << (fenced.value().had_live_grant ? "yes" : "no") << " commit "
            << to_string(fenced.value().fence.commit) << '\n';
  auto after_fence = coordinator.plan(fenced_plan);
  std::cout << "grant-recorded " << (after_fence.value().grant.has_value() ? "yes" : "no")
            << " grant-live " << (after_fence.value().grant_live ? "yes" : "no") << '\n';
  print_verdict(coordinator, fenced_plan, "fenced");

  // Evidence observed strictly after the floor is admissible again. The
  // generation must still advance, so the fence costs a new observation rather
  // than a reinterpretation of the old one.
  RecordEnumerationRequest after;
  after.context = context_for(coordinator, fenced_plan);
  after.domain = OwnerDomain::kAsi;
  after.coverage = CoverageState::kComplete;
  after.generation = EvidenceGeneration{3};
  after.observed_at = after.context.observation;
  after.scope_manifest_digest = scope_digest;
  after.generations = generations;
  after.source = "asi-controller";
  after.consumers = consumers;
  auto re_enumerated = coordinator.record_enumeration(after);
  if (!re_enumerated) {
    std::cout << "re-enumerate failed: " << re_enumerated.error().to_text() << '\n';
    return 1;
  }
  std::cout << "re-enumerated generation " << to_string(re_enumerated.value().evidence.generation)
            << " observed " << to_string(re_enumerated.value().evidence.observed_at) << " accepted "
            << (re_enumerated.value().accepted ? "yes" : "no") << '\n';
  print_verdict(coordinator, fenced_plan, "after-floor-evidence");

  if (!create_plan(coordinator, cancelled_plan, DrainScope{ScopeKind::kRack, 32}, PolicyId{9191},
                   consumers, generations, scope_digest, manifest_digest)) {
    return 1;
  }
  RecordEnumerationRequest enumeration;
  enumeration.context = context_for(coordinator, cancelled_plan);
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
  issue.context = context_for(coordinator, cancelled_plan);
  issue.domains = DomainMask::of(OwnerDomain::kAsi);
  auto issued = coordinator.issue_requests(issue);
  if (!issued) {
    std::cout << "issue failed: " << issued.error().to_text() << '\n';
    return 1;
  }

  CancelPlanRequest cancel;
  cancel.context = context_for(coordinator, cancelled_plan);
  cancel.reason = "the maintenance window was withdrawn";
  auto cancelled = coordinator.cancel_plan(cancel);
  if (!cancelled) {
    std::cout << "cancel failed: " << cancelled.error().to_text() << '\n';
    return 1;
  }
  std::cout << "cancelled state " << to_token(cancelled.value().plan.state) << " requests-cancelled "
            << cancelled.value().requests_cancelled << " requests "
            << cancelled.value().plan.requests.size() << '\n';
  for (const DrainRequest& request : cancelled.value().plan.requests) {
    std::cout << "  request " << to_string(request.id) << " state " << to_token(request.state)
              << " detail " << request.settlement_detail << '\n';
  }

  RecordEnumerationRequest refused;
  refused.context = context_for(coordinator, cancelled_plan);
  refused.domain = OwnerDomain::kAsi;
  refused.coverage = CoverageState::kComplete;
  refused.generation = EvidenceGeneration{2};
  refused.observed_at = refused.context.observation;
  refused.scope_manifest_digest = scope_digest;
  refused.generations = generations;
  refused.source = "asi-controller";
  auto refusal = coordinator.record_enumeration(refused);
  if (refusal) {
    std::cout << "the cancelled plan accepted an enumeration, which it must not\n";
    return 1;
  }
  std::cout << "refused " << to_token(refusal.error().code()) << ": " << refusal.error().detail()
            << '\n';
  print_verdict(coordinator, cancelled_plan, "cancelled");
  return 0;
}
