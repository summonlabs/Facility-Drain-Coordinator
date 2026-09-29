// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 02: enumeration and bounded request issuing.
//
// Shows what a request is and what it is not. Every named domain must already
// have an accepted complete enumeration, because a request is a statement about
// a known set of obligations. A domain with no obligations yields no request at
// all. Re-issuing a key that is still staged re-offers the same record under the
// same idempotency key, and re-issuing one that was delivered reports the
// duplicate without asking the owner twice.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
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
  set.scope = ScopeGeneration{21};
  set.dependency = DependencyGeneration{22};
  set.reservation = ReservationGeneration{23};
  set.obligation = ObligationGeneration{24};
  set.policy = PolicyGeneration{25};
  set.topology = TopologyGeneration{26};
  set.maintenance = MaintenanceGeneration{27};
  set.capacity = CapacityGeneration{28};
  set.hardware = HardwareGeneration{29};
  set.firmware = FirmwareGeneration{30};
  return set;
}

ConsumerRecord consumer(ConsumerCategory category, std::uint64_t obligation, std::string label) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = category;
  record.generation = ObligationGeneration{3};
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
  context.principal = "example-02";
  return context;
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
  const DrainScope scope{ScopeKind::kAsset, 17};
  const GenerationSet generations = complete_generations();
  const std::vector<ConsumerRecord> consumers{
      consumer(ConsumerCategory::kWorkload, 1001, "training-job"),
      consumer(ConsumerCategory::kWorkload, 1002, "inference-job"),
      consumer(ConsumerCategory::kNetworkPath, 2001, "fabric-path")};

  CreatePlanRequest create;
  create.context.plan = plan_id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "example-02";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::all();
  create.generations = generations;
  create.policy_id = PolicyId{5150};
  create.policy_digest = digest_text("policy-5150");
  create.consumers = consumers;
  create.label = "drain-asset-17";
  auto created = coordinator.create_plan(create);
  if (!created) {
    std::cout << "create failed: " << created.error().to_text() << '\n';
    return 1;
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();

  std::cout << "example 02: enumeration and bounded request issuing\n";
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
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
    std::cout << "enumerated " << to_token(domain) << " coverage "
              << to_token(recorded.value().evidence.coverage) << " consumers "
              << recorded.value().consumers_recorded << " accepted "
              << (recorded.value().accepted ? "yes" : "no") << '\n';
  }

  IssueRequestsRequest issue;
  issue.context = context_for(coordinator, plan_id);
  issue.domains = DomainMask::all();
  // One obligation at a time: a bound is a hard upper limit on what the owner
  // may act on, and it is recorded on the request rather than inferred later.
  issue.bound_operations = 1;
  auto issued = coordinator.issue_requests(issue);
  if (!issued) {
    std::cout << "issue failed: " << issued.error().to_text() << '\n';
    return 1;
  }
  std::cout << "issue staged " << issued.value().newly_staged << " duplicates "
            << issued.value().duplicates << '\n';
  DrainRequestId first_request{};
  for (std::size_t index = 0; index < issued.value().items.size(); ++index) {
    const IssueItem& item = issued.value().items[index];
    const std::string domain_text = std::string{to_token(owner_domain_at(static_cast<std::uint32_t>(index)))};
    if (item.request.id.value() == 0) {
      std::cout << "item " << domain_text << " request none deliver no detail " << item.detail << '\n';
      continue;
    }
    if (first_request.value() == 0) {
      first_request = item.request.id;
    }
    std::cout << "item " << domain_text << " request " << to_string(item.request.id) << " state "
              << to_token(item.request.state) << " attempt " << to_string(item.request.key.attempt)
              << " bound " << item.request.bound_operations << " key "
              << item.request.idempotency_key.to_hex() << '\n';
    std::cout << "instruction " << item.request.instruction << '\n';
  }

  IssueRequestsRequest again;
  again.context = context_for(coordinator, plan_id);
  again.domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  auto repeated = coordinator.issue_requests(again);
  if (!repeated) {
    std::cout << "repeat issue failed: " << repeated.error().to_text() << '\n';
    return 1;
  }
  for (const IssueItem& item : repeated.value().items) {
    std::cout << "re-issue " << to_token(item.request.key.domain) << " duplicate "
              << (item.duplicate ? "yes" : "no") << " deliver " << (item.deliver ? "yes" : "no")
              << " key " << item.request.idempotency_key.to_hex() << " detail " << item.detail << '\n';
  }

  ConfirmDeliveryRequest delivery;
  delivery.context = context_for(coordinator, plan_id);
  delivery.request = first_request;
  delivery.delivery_reference = "handoff-0001";
  auto delivered = coordinator.confirm_delivery(delivery);
  if (!delivered) {
    std::cout << "deliver failed: " << delivered.error().to_text() << '\n';
    return 1;
  }
  std::cout << "delivered request " << to_string(delivered.value().request.id) << " state "
            << to_token(delivered.value().request.state) << '\n';

  IssueRequestsRequest third;
  third.context = context_for(coordinator, plan_id);
  third.domains = DomainMask::of(OwnerDomain::kAsi);
  auto settled = coordinator.issue_requests(third);
  if (!settled) {
    std::cout << "third issue failed: " << settled.error().to_text() << '\n';
    return 1;
  }
  for (const IssueItem& item : settled.value().items) {
    std::cout << "re-issue " << to_token(item.request.key.domain) << " duplicate "
              << (item.duplicate ? "yes" : "no") << " deliver " << (item.deliver ? "yes" : "no")
              << " state " << to_token(item.request.state) << " detail " << item.detail << '\n';
  }
  auto final_view = coordinator.plan(plan_id);
  std::cout << "plan state " << to_token(final_view.value().state) << " requests "
            << final_view.value().requests.size() << '\n';
  return 0;
}
