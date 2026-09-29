// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 01: plan creation and binding.
//
// Creates a plan against one exact generation set and prints what creation
// actually recorded: revision 1, the canonical target manifest, the domains the
// plan must have proven, and the consumer manifest digest bound for every
// domain -- including domains the plan holds no consumers for, because "no
// consumers" is a statement that later evidence is checked against rather than
// an absence of one.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/plan.hpp"
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
  set.scope = ScopeGeneration{11};
  set.dependency = DependencyGeneration{12};
  set.reservation = ReservationGeneration{13};
  set.obligation = ObligationGeneration{14};
  set.policy = PolicyGeneration{15};
  set.topology = TopologyGeneration{16};
  set.maintenance = MaintenanceGeneration{17};
  set.capacity = CapacityGeneration{18};
  set.hardware = HardwareGeneration{19};
  set.firmware = FirmwareGeneration{20};
  return set;
}

ConsumerRecord consumer(ConsumerCategory category, std::uint64_t obligation, ObligationStrength strength,
                        std::string label) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = category;
  record.generation = ObligationGeneration{7};
  record.strength = strength;
  record.label = std::move(label);
  record.source = "fleet-inventory";
  return record;
}

std::string digest_or_unbound(const std::optional<ContentDigest>& digest) {
  return digest.has_value() ? digest.value().to_hex() : std::string{"unbound"};
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

  const DrainScope scope{ScopeKind::kAsset, 17};
  const std::vector<DrainScope> targets{DrainScope{ScopeKind::kRack, 4}, scope,
                                       DrainScope{ScopeKind::kAsset, 18}};

  CreatePlanRequest request;
  request.context.plan = PlanId{1};
  request.context.expected_revision = Revision{1};
  request.context.incarnation = coordinator.incarnation();
  request.context.expected_epoch = coordinator.control_epoch();
  request.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  request.context.principal = "example-01";
  request.id = PlanId{1};
  request.scope = scope;
  request.targets = targets;
  // Only ASI is declared. The mandatory DFI obligation below widens the derived
  // requirement, because a declared requirement is a floor and never a ceiling.
  request.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  request.generations = complete_generations();
  request.policy_id = PolicyId{4242};
  request.policy_digest = digest_text("policy-4242-revision-a");
  request.consumers.push_back(
      consumer(ConsumerCategory::kWorkload, 1001, ObligationStrength::kMandatory, "training-job"));
  request.consumers.push_back(
      consumer(ConsumerCategory::kNetworkPath, 2001, ObligationStrength::kMandatory, "fabric-path"));
  request.consumers.push_back(consumer(ConsumerCategory::kMonitoringDependency, 3001,
                                       ObligationStrength::kAdvisory, "alert-route"));
  request.label = "drain-rack-4";
  request.requested_by = "operator";

  auto created = coordinator.create_plan(request);
  if (!created) {
    std::cout << "create failed: " << created.error().to_text() << '\n';
    return 1;
  }
  const DrainPlanSnapshot& plan = created.value().plan;

  std::cout << "example 01: plan creation and binding\n";
  std::cout << "plan " << to_string(plan.spec.id) << " revision " << to_string(plan.spec.revision)
            << " state " << to_token(plan.state) << '\n';
  std::cout << "scope " << format_scope(plan.spec.scope) << " targets "
            << plan.spec.targets.to_canonical() << '\n';
  std::cout << "required-declared " << plan.spec.declared_required_domains.to_canonical()
            << " required-derived " << plan.required_domains.to_canonical() << '\n';
  std::cout << "generations " << plan.spec.bindings.generations.to_canonical() << '\n';
  std::cout << "policy-id " << to_string(plan.spec.bindings.policy_id) << " policy-digest "
            << plan.spec.bindings.policy_digest.to_hex() << '\n';
  std::cout << "created-at " << plan.spec.created_at_milliseconds << " commit "
            << to_string(plan.last_commit) << '\n';
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    std::cout << "bound " << to_token(domain) << " "
              << digest_or_unbound(plan.spec.bindings.manifest_digest(domain)) << '\n';
  }
  const std::vector<ConsumerRecord> none;
  const ContentDigest empty_facility = consumer_manifest_digest(OwnerDomain::kFacility, none);
  std::cout << "facility-binding-is-the-empty-manifest "
            << (plan.spec.bindings.manifest_digest(OwnerDomain::kFacility).value() == empty_facility
                    ? "yes"
                    : "no")
            << '\n';

  // A second plan declines to bind consumer manifests at planning time. Its four
  // bindings stay unbound until a complete enumeration binds one, which is what
  // the coordinator refuses to guess in the meantime.
  CreatePlanRequest unbound;
  unbound.context.plan = PlanId{2};
  unbound.context.expected_revision = Revision{1};
  unbound.context.incarnation = coordinator.incarnation();
  unbound.context.expected_epoch = coordinator.control_epoch();
  unbound.context.observation = ObservationSequence{plan.last_observation.value() + 1U};
  unbound.context.principal = "example-01";
  unbound.id = PlanId{2};
  unbound.scope = DrainScope{ScopeKind::kZone, 3};
  unbound.targets.push_back(DrainScope{ScopeKind::kZone, 3});
  unbound.declared_required_domains = DomainMask::of(OwnerDomain::kMonitoring);
  unbound.generations = complete_generations();
  unbound.policy_id = PolicyId{4243};
  unbound.policy_digest = digest_text("policy-4243-revision-a");
  unbound.bind_consumer_manifests = false;
  unbound.label = "drain-zone-3";

  auto second = coordinator.create_plan(unbound);
  if (!second) {
    std::cout << "second create failed: " << second.error().to_text() << '\n';
    return 1;
  }
  std::cout << "plan " << to_string(second.value().plan.spec.id) << " bind-consumers no bound";
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    std::cout << ' ' << to_token(domain) << '='
              << digest_or_unbound(second.value().plan.spec.bindings.manifest_digest(domain));
  }
  std::cout << '\n';
  std::cout << "plans " << coordinator.snapshot().value().plans.size() << '\n';
  return 0;
}
