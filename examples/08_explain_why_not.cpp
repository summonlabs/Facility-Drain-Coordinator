// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 08: why a verdict is denied.
//
// The explanation is not a message invented for operators: it is the same
// deterministic rendering the evaluation itself carries, one line per domain in
// canonical order plus the blocking summary, and it names the first code in the
// canonical walk. As the facts improve, the primary code walks forward with
// them, and the last line shows the same explanation after a grant.

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
#include "facilitydrain/residual.hpp"
#include "facilitydrain/snapshot.hpp"

#include <algorithm>
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
  set.scope = ScopeGeneration{81};
  set.dependency = DependencyGeneration{82};
  set.reservation = ReservationGeneration{83};
  set.obligation = ObligationGeneration{84};
  set.policy = PolicyGeneration{85};
  set.topology = TopologyGeneration{86};
  set.maintenance = MaintenanceGeneration{87};
  set.capacity = CapacityGeneration{88};
  set.hardware = HardwareGeneration{89};
  set.firmware = FirmwareGeneration{90};
  return set;
}

ConsumerRecord consumer(ConsumerCategory category, std::uint64_t obligation, std::string label) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = category;
  record.generation = ObligationGeneration{11};
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
  context.principal = "example-08";
  return context;
}

void print_summary(const Coordinator& coordinator, PlanId id, const std::string& stage) {
  auto evaluation = coordinator.evaluate_safe_to_remove(id);
  if (!evaluation) {
    std::cout << stage << " evaluate failed: " << evaluation.error().to_text() << '\n';
    return;
  }
  auto view = coordinator.plan(id);
  std::cout << stage << " verdict " << to_token(evaluation.value().verdict) << " primary "
            << to_token(evaluation.value().primary_blocking_code) << " state "
            << to_token(view.value().state) << " blocking";
  for (const ErrorCode code : evaluation.value().blocking_codes) {
    std::cout << ' ' << to_token(code);
  }
  std::cout << '\n';
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
  const DrainScope scope{ScopeKind::kRack, 51};
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
  create.context.principal = "example-08";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  create.generations = generations;
  create.policy_id = PolicyId{1212};
  create.policy_digest = digest_text("policy-1212");
  create.consumers = consumers;
  create.label = "drain-rack-51";
  auto created = coordinator.create_plan(create);
  if (!created) {
    std::cout << "create failed: " << created.error().to_text() << '\n';
    return 1;
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();

  std::cout << "example 08: why a verdict is denied\n";
  print_summary(coordinator, plan_id, "no-evidence");

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
  auto after_enumeration = coordinator.evaluate_safe_to_remove(plan_id);
  if (!after_enumeration) {
    std::cout << "evaluate failed: " << after_enumeration.error().to_text() << '\n';
    return 1;
  }
  print_summary(coordinator, plan_id, "enumerated");
  std::cout << format_evaluation(after_enumeration.value());

  IngestCompletionRequest asi;
  asi.context = context_for(coordinator, plan_id);
  asi.domain = OwnerDomain::kAsi;
  asi.state = CompletionState::kDrained;
  asi.generation = EvidenceGeneration{2};
  asi.observed_at = asi.context.observation;
  asi.payload_digest = digest_text("asi-drained-report");
  asi.manifest_digest = created.value().plan.spec.bindings.manifest_digest(OwnerDomain::kAsi).value();
  asi.scope_manifest_digest = scope_digest;
  asi.generations = generations;
  asi.residual_count_known = true;
  asi.residual_count = 0;
  asi.source = "asi-controller";
  auto asi_ingested = coordinator.ingest_completion(asi);
  if (!asi_ingested) {
    std::cout << "ingest failed: " << asi_ingested.error().to_text() << '\n';
    return 1;
  }

  // DFI reports the domain drained without saying how many obligations remain.
  // An absent count is not zero, so the verdict stays denied and names the
  // unknown rather than assuming the best case.
  IngestCompletionRequest dfi;
  dfi.context = context_for(coordinator, plan_id);
  dfi.domain = OwnerDomain::kDfi;
  dfi.state = CompletionState::kDrained;
  dfi.generation = EvidenceGeneration{2};
  dfi.observed_at = dfi.context.observation;
  dfi.payload_digest = digest_text("dfi-drained-report-without-count");
  dfi.manifest_digest = created.value().plan.spec.bindings.manifest_digest(OwnerDomain::kDfi).value();
  dfi.scope_manifest_digest = scope_digest;
  dfi.generations = generations;
  dfi.residual_count_known = false;
  dfi.residual_count = 0;
  dfi.source = "dfi-controller";
  auto dfi_ingested = coordinator.ingest_completion(dfi);
  if (!dfi_ingested) {
    std::cout << "ingest failed: " << dfi_ingested.error().to_text() << '\n';
    return 1;
  }
  print_summary(coordinator, plan_id, "dfi-unknown-count");
  auto explanation = coordinator.explain(plan_id);
  if (!explanation) {
    std::cout << "explain failed: " << explanation.error().to_text() << '\n';
    return 1;
  }
  std::cout << explanation.value();

  RecordResidualRequest refused;
  refused.context = context_for(coordinator, plan_id);
  refused.entry.domain = OwnerDomain::kDfi;
  refused.entry.obligation = ObligationId{2001};
  refused.entry.kind = ResidualKind::kOwnerRefused;
  refused.entry.detail = "the fabric owner declined to relinquish the path inside the window";
  auto refused_recorded = coordinator.record_residual(refused);
  if (!refused_recorded) {
    std::cout << "residual record failed: " << refused_recorded.error().to_text() << '\n';
    return 1;
  }
  IngestCompletionRequest with_residual;
  with_residual.context = context_for(coordinator, plan_id);
  with_residual.domain = OwnerDomain::kDfi;
  with_residual.state = CompletionState::kDrainedWithResiduals;
  with_residual.generation = EvidenceGeneration{3};
  with_residual.observed_at = with_residual.context.observation;
  with_residual.payload_digest = digest_text("dfi-drained-with-residuals");
  with_residual.manifest_digest = created.value().plan.spec.bindings.manifest_digest(OwnerDomain::kDfi).value();
  with_residual.scope_manifest_digest = scope_digest;
  with_residual.generations = generations;
  with_residual.residual_count_known = true;
  with_residual.residual_count = 1;
  with_residual.source = "dfi-controller";
  auto residual_ingested = coordinator.ingest_completion(with_residual);
  if (!residual_ingested) {
    std::cout << "ingest failed: " << residual_ingested.error().to_text() << '\n';
    return 1;
  }
  print_summary(coordinator, plan_id, "dfi-refused-with-count");
  auto still_denied = coordinator.evaluate_safe_to_remove(plan_id);
  if (!still_denied) {
    std::cout << "evaluate failed: " << still_denied.error().to_text() << '\n';
    return 1;
  }
  std::cout << format_evaluation(still_denied.value());

  ResolveResidualRequest resolve;
  resolve.context = context_for(coordinator, plan_id);
  resolve.domain = OwnerDomain::kDfi;
  resolve.obligation = ObligationId{2001};
  resolve.kind = ResidualKind::kOwnerRefused;
  resolve.resolution_evidence_generation = EvidenceGeneration{3};
  resolve.detail = "the owner relinquished the path at generation 3";
  auto resolved = coordinator.resolve_residual(resolve);
  if (!resolved) {
    std::cout << "residual resolve failed: " << resolved.error().to_text() << '\n';
    return 1;
  }
  std::cout << "resolved " << resolved.value().entry.to_canonical() << '\n';
  print_summary(coordinator, plan_id, "residual-relinquished");
  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, plan_id);
  grant.granted_by = "operator";
  auto granted = coordinator.grant_safe_to_remove(grant);
  if (!granted) {
    std::cout << "grant failed: " << granted.error().to_text() << '\n';
    return 1;
  }
  auto final_view = coordinator.plan(plan_id);
  std::cout << "state " << to_token(final_view.value().state) << " grant-live "
            << (final_view.value().grant_live ? "yes" : "no") << " explanation-lines "
            << std::count(granted.value().evaluation.explanation.begin(),
                          granted.value().evaluation.explanation.end(), '\n')
            << '\n';
  return 0;
}
