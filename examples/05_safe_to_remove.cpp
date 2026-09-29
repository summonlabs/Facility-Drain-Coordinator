// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 05: a granted safe to remove verdict.
//
// Walks the whole path in order and prints the deterministic explanation at each
// step. An acknowledgement is a received message and never completes a domain,
// so the verdict stays denied until completion evidence is ingested. Only then
// does the grant record authority, and the grant binds the revision, the epoch,
// the generation set and the exact evidence digest it was evaluated against.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/report.hpp"
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
  set.scope = ScopeGeneration{51};
  set.dependency = DependencyGeneration{52};
  set.reservation = ReservationGeneration{53};
  set.obligation = ObligationGeneration{54};
  set.policy = PolicyGeneration{55};
  set.topology = TopologyGeneration{56};
  set.maintenance = MaintenanceGeneration{57};
  set.capacity = CapacityGeneration{58};
  set.hardware = HardwareGeneration{59};
  set.firmware = FirmwareGeneration{60};
  return set;
}

ConsumerRecord consumer(ConsumerCategory category, std::uint64_t obligation, std::string label) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = category;
  record.generation = ObligationGeneration{8};
  record.strength = ObligationStrength::kMandatory;
  record.label = std::move(label);
  record.source = "fleet-inventory";
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
  context.principal = "example-05";
  return context;
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
  const DrainScope scope{ScopeKind::kRack, 21};
  const GenerationSet generations = complete_generations();
  const std::vector<ConsumerRecord> consumers{
      consumer(ConsumerCategory::kWorkload, 1001, "training-job"),
      consumer(ConsumerCategory::kNetworkPath, 2001, "fabric-path")};

  CreatePlanRequest create;
  create.context.plan = plan_id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "example-05";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  create.generations = generations;
  create.policy_id = PolicyId{8180};
  create.policy_digest = digest_text("policy-8180");
  create.consumers = consumers;
  create.label = "drain-rack-21";
  auto created = coordinator.create_plan(create);
  if (!created) {
    std::cout << "create failed: " << created.error().to_text() << '\n';
    return 1;
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();

  std::cout << "example 05: a granted safe to remove verdict\n";
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!create.declared_required_domains.contains(domain)) {
      continue;
    }
    RecordEnumerationRequest enumeration;
    enumeration.context = context_for(coordinator, plan_id);
    enumeration.domain = domain;
    enumeration.coverage = CoverageState::kComplete;
    enumeration.generation = EvidenceGeneration{1};
    enumeration.observed_at = enumeration.context.observation;
    enumeration.scope_manifest_digest = scope_digest;
    enumeration.generations = generations;
    enumeration.source = std::string{to_token(domain)} + "-controller";
    for (const ConsumerRecord& record : consumers) {
      if (record.domain() == domain) {
        enumeration.consumers.push_back(record);
      }
    }
    auto recorded = coordinator.record_enumeration(enumeration);
    if (!recorded) {
      std::cout << "enumerate failed: " << recorded.error().to_text() << '\n';
      return 1;
    }
  }

  IssueRequestsRequest issue;
  issue.context = context_for(coordinator, plan_id);
  issue.domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  auto issued = coordinator.issue_requests(issue);
  if (!issued) {
    std::cout << "issue failed: " << issued.error().to_text() << '\n';
    return 1;
  }
  for (const DrainRequest& staged : issued.value().to_deliver) {
    ConfirmDeliveryRequest delivery;
    delivery.context = context_for(coordinator, plan_id);
    delivery.request = staged.id;
    delivery.delivery_reference = std::string{"handoff-"} + std::string{to_token(staged.key.domain)};
    auto delivered = coordinator.confirm_delivery(delivery);
    if (!delivered) {
      std::cout << "deliver failed: " << delivered.error().to_text() << '\n';
      return 1;
    }
    RecordAcknowledgementRequest acknowledgement;
    acknowledgement.context = context_for(coordinator, plan_id);
    acknowledgement.request = staged.id;
    acknowledgement.acknowledging_system = std::string{to_token(staged.key.domain)} + "-controller";
    auto acknowledged = coordinator.record_acknowledgement(acknowledgement);
    if (!acknowledged) {
      std::cout << "acknowledge failed: " << acknowledged.error().to_text() << '\n';
      return 1;
    }
    std::cout << "request " << to_string(staged.id) << " " << to_token(staged.key.domain)
              << " acknowledged by " << acknowledged.value().request.acknowledgement_source << '\n';
  }
  print_verdict(coordinator, plan_id, "after-acknowledgement");

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!create.declared_required_domains.contains(domain)) {
      continue;
    }
    auto view = coordinator.plan(plan_id);
    IngestCompletionRequest completion;
    completion.context = context_for(coordinator, plan_id);
    completion.domain = domain;
    completion.state = CompletionState::kDrained;
    completion.generation = EvidenceGeneration{2};
    completion.observed_at = completion.context.observation;
    completion.payload_digest = digest_text(std::string{"drained-report-"} + std::string{to_token(domain)});
    completion.manifest_digest = view.value().spec.bindings.manifest_digest(domain).value();
    completion.scope_manifest_digest = scope_digest;
    completion.generations = generations;
    completion.residual_count_known = true;
    completion.residual_count = 0;
    completion.source = std::string{to_token(domain)} + "-controller";
    auto ingested = coordinator.ingest_completion(completion);
    if (!ingested) {
      std::cout << "ingest failed: " << ingested.error().to_text() << '\n';
      return 1;
    }
    std::cout << "completion " << to_token(domain) << " generation "
              << to_string(ingested.value().evidence.generation) << " compatible "
              << (ingested.value().compatible ? "yes" : "no") << '\n';
  }
  print_verdict(coordinator, plan_id, "after-completion");

  auto evaluation = coordinator.evaluate_safe_to_remove(plan_id);
  if (!evaluation) {
    std::cout << "evaluate failed: " << evaluation.error().to_text() << '\n';
    return 1;
  }
  std::cout << format_evaluation(evaluation.value());

  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, plan_id);
  grant.granted_by = "operator";
  auto granted = coordinator.grant_safe_to_remove(grant);
  if (!granted) {
    std::cout << "grant failed: " << granted.error().to_text() << '\n';
    return 1;
  }
  const SafeToRemoveGrant& record = granted.value().grant.value();
  std::cout << "grant plan " << to_string(record.plan) << " revision " << to_string(record.revision)
            << " epoch " << to_string(record.epoch) << " commit " << to_string(record.granted_commit)
            << " by " << record.granted_by << '\n';
  std::cout << "grant evidence " << record.evidence_digest.to_hex() << " manifest "
            << record.manifest_digest.to_hex() << '\n';
  std::cout << "grant generations " << record.generations.to_canonical() << '\n';
  auto final_view = coordinator.plan(plan_id);
  std::cout << "state " << to_token(final_view.value().state) << " grant-live "
            << (final_view.value().grant_live ? "yes" : "no") << '\n';
  print_verdict(coordinator, plan_id, "after-grant");
  return 0;
}
