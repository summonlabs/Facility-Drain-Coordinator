// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Example 07: restart, recovery and replay of a staged request.
//
// Runs against a real durable store in a temporary directory that is removed at
// the end. The coordinator is closed and reopened between the three phases, so
// each phase is a real restart: the control epoch advances, the incarnation is
// replaced, the plan survives, the still staged request is re-offered under the
// same idempotency key, and the recorded grant persists but stops being live.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/persistence.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/snapshot.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace facilitydrain;

constexpr std::int64_t kFixedMilliseconds = 1700000000000;

struct PhaseResult {
  bool ok = false;
  std::string text{};
};

GenerationSet complete_generations() {
  GenerationSet set;
  set.scope = ScopeGeneration{71};
  set.dependency = DependencyGeneration{72};
  set.reservation = ReservationGeneration{73};
  set.obligation = ObligationGeneration{74};
  set.policy = PolicyGeneration{75};
  set.topology = TopologyGeneration{76};
  set.maintenance = MaintenanceGeneration{77};
  set.capacity = CapacityGeneration{78};
  set.hardware = HardwareGeneration{79};
  set.firmware = FirmwareGeneration{80};
  return set;
}

ConsumerRecord workload(std::uint64_t obligation, std::string label) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{10};
  record.strength = ObligationStrength::kMandatory;
  record.label = std::move(label);
  record.source = "asi-controller";
  return record;
}

Result<Coordinator> open(const std::filesystem::path& root) {
  CoordinatorOpenRequest request;
  request.root = root;
  request.writer_label = "example-07";
  request.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(kFixedMilliseconds)};
  return Coordinator::open(request);
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
  context.principal = "example-07";
  return context;
}

/// Phase one: create the plan, enumerate it and stage one bounded request. The
/// session ends when this function returns, which is what makes the next phase
/// a restart rather than a continuation.
PhaseResult phase_one(const std::filesystem::path& root) {
  PhaseResult result;
  auto opened = open(root);
  if (!opened) {
    result.text = "open failed: " + opened.error().to_text();
    return result;
  }
  Coordinator coordinator = std::move(opened).value();
  const PlanId plan_id{1};
  const DrainScope scope{ScopeKind::kRack, 41};
  const GenerationSet generations = complete_generations();
  const std::vector<ConsumerRecord> consumers{workload(1001, "training-job")};

  CreatePlanRequest create;
  create.context.plan = plan_id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "example-07";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  create.generations = generations;
  create.policy_id = PolicyId{1010};
  create.policy_digest = digest_text("policy-1010");
  create.consumers = consumers;
  create.label = "drain-rack-41";
  auto created = coordinator.create_plan(create);
  if (!created) {
    result.text = "create failed: " + created.error().to_text();
    return result;
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();

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
    result.text = "enumerate failed: " + recorded.error().to_text();
    return result;
  }

  IssueRequestsRequest issue;
  issue.context = context_for(coordinator, plan_id);
  issue.domains = DomainMask::of(OwnerDomain::kAsi);
  auto issued = coordinator.issue_requests(issue);
  if (!issued) {
    result.text = "issue failed: " + issued.error().to_text();
    return result;
  }
  const DrainRequest& staged = issued.value().to_deliver.front();
  auto view = coordinator.plan(plan_id);
  result.ok = true;
  result.text = "phase1 store-created " +
                std::string{coordinator.recovery().created_new_store ? "yes" : "no"} + " epoch " +
                to_string(coordinator.control_epoch()) + " revision " +
                to_string(view.value().spec.revision) + " state " + std::string{to_token(view.value().state)} +
                " requests " + std::to_string(view.value().requests.size()) + "\n" + "phase1 staged " +
                to_string(staged.id) + " attempt " + to_string(staged.key.attempt) + " bound " +
                std::to_string(staged.bound_operations) + " key " + staged.idempotency_key.to_hex();
  return result;
}

/// Phase two: a new process over the same store. The request was never
/// delivered, so it is re-offered rather than forgotten, and the idempotency key
/// is byte identical, so the owner can recognise the replay.
PhaseResult phase_two(const std::filesystem::path& root, const std::string& staged_key) {
  PhaseResult result;
  auto opened = open(root);
  if (!opened) {
    result.text = "open failed: " + opened.error().to_text();
    return result;
  }
  Coordinator coordinator = std::move(opened).value();
  const PlanId plan_id{1};
  const RecoveryReport& recovery = coordinator.recovery();

  IssueRequestsRequest issue;
  issue.context = context_for(coordinator, plan_id);
  issue.domains = DomainMask::of(OwnerDomain::kAsi);
  auto issued = coordinator.issue_requests(issue);
  if (!issued) {
    result.text = "re-issue failed: " + issued.error().to_text();
    return result;
  }
  const IssueItem& item = issued.value().items.front();
  const std::string replay_key = item.request.idempotency_key.to_hex();

  ConfirmDeliveryRequest delivery;
  delivery.context = context_for(coordinator, plan_id);
  delivery.request = item.request.id;
  delivery.delivery_reference = "handoff-after-restart";
  auto delivered = coordinator.confirm_delivery(delivery);
  if (!delivered) {
    result.text = "deliver failed: " + delivered.error().to_text();
    return result;
  }
  RecordAcknowledgementRequest acknowledgement;
  acknowledgement.context = context_for(coordinator, plan_id);
  acknowledgement.request = item.request.id;
  acknowledgement.acknowledging_system = "asi-controller";
  auto acknowledged = coordinator.record_acknowledgement(acknowledgement);
  if (!acknowledged) {
    result.text = "acknowledge failed: " + acknowledged.error().to_text();
    return result;
  }

  auto view = coordinator.plan(plan_id);
  IngestCompletionRequest completion;
  completion.context = context_for(coordinator, plan_id);
  completion.domain = OwnerDomain::kAsi;
  completion.state = CompletionState::kDrained;
  completion.generation = EvidenceGeneration{2};
  completion.observed_at = completion.context.observation;
  completion.payload_digest = digest_text("asi-drained-report");
  completion.manifest_digest = view.value().spec.bindings.manifest_digest(OwnerDomain::kAsi).value();
  completion.scope_manifest_digest = view.value().spec.targets.digest();
  completion.generations = view.value().spec.bindings.generations;
  completion.residual_count_known = true;
  completion.residual_count = 0;
  completion.source = "asi-controller";
  auto ingested = coordinator.ingest_completion(completion);
  if (!ingested) {
    result.text = "ingest failed: " + ingested.error().to_text();
    return result;
  }

  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, plan_id);
  grant.granted_by = "operator";
  auto granted = coordinator.grant_safe_to_remove(grant);
  if (!granted) {
    result.text = "grant failed: " + granted.error().to_text();
    return result;
  }
  auto final_view = coordinator.plan(plan_id);
  result.ok = true;
  result.text = "phase2 recovered " + std::string{recovery.recovered ? "yes" : "no"} +
                " previous-epoch " + to_string(recovery.previous_epoch) + " current-epoch " +
                to_string(recovery.current_epoch) + " sequence " +
                to_string(recovery.recovered_sequence) + " transient-removed " +
                std::to_string(recovery.transient_files_removed) + " unpublished " +
                std::to_string(recovery.unpublished_generations) + "\n" + "phase2 replay duplicate " +
                std::string{item.duplicate ? "yes" : "no"} + " deliver " +
                std::string{item.deliver ? "yes" : "no"} + " same-key " +
                std::string{replay_key == staged_key ? "yes" : "no"} + " state " +
                std::string{to_token(item.request.state)} + "\n" + "phase2 delivered " +
                to_string(delivered.value().request.id) + " " +
                std::string{to_token(delivered.value().request.state)} + " acknowledged-by " +
                acknowledged.value().request.acknowledgement_source + "\n" + "phase2 completion compatible " +
                std::string{ingested.value().compatible ? "yes" : "no"} + "\n" + "phase2 state " +
                std::string{to_token(final_view.value().state)} + " grant-live " +
                std::string{final_view.value().grant_live ? "yes" : "no"};
  return result;
}

/// Phase three: the grant recorded in phase two persisted, and it stopped being
/// live the moment the control epoch moved. The evidence still supports removal,
/// so the answer is asked again and recorded again.
PhaseResult phase_three(const std::filesystem::path& root) {
  PhaseResult result;
  auto opened = open(root);
  if (!opened) {
    result.text = "open failed: " + opened.error().to_text();
    return result;
  }
  Coordinator coordinator = std::move(opened).value();
  const PlanId plan_id{1};
  const RecoveryReport& recovery = coordinator.recovery();
  auto view = coordinator.plan(plan_id);
  const bool recorded_before = view.value().grant.has_value();
  const bool live_before = view.value().grant_live;
  const std::string state_before{to_token(view.value().state)};

  auto evaluation = coordinator.evaluate_safe_to_remove(plan_id);
  if (!evaluation) {
    result.text = "evaluate failed: " + evaluation.error().to_text();
    return result;
  }
  if (evaluation.value().verdict != SafeToRemoveVerdict::kGranted) {
    result.text = "phase3 the recovered evidence no longer grants removal: " +
                  std::string{to_token(evaluation.value().primary_blocking_code)};
    return result;
  }
  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, plan_id);
  grant.granted_by = "operator";
  auto granted = coordinator.grant_safe_to_remove(grant);
  if (!granted) {
    result.text = "grant failed: " + granted.error().to_text();
    return result;
  }
  auto final_view = coordinator.plan(plan_id);
  result.ok = true;
  result.text = "phase3 recovered " + std::string{recovery.recovered ? "yes" : "no"} +
                " previous-epoch " + to_string(recovery.previous_epoch) + " current-epoch " +
                to_string(recovery.current_epoch) + " grants-fenced " +
                std::to_string(recovery.grants_fenced) + " plans-reopened " +
                std::to_string(recovery.plans_reopened) + "\n" + "phase3 grant-recorded " +
                std::string{recorded_before ? "yes" : "no"} + " grant-live " +
                std::string{live_before ? "yes" : "no"} + " state " + state_before + "\n" +
                "phase3 re-granted state " + std::string{to_token(final_view.value().state)} +
                " grant-live " + std::string{final_view.value().grant_live ? "yes" : "no"} +
                " evidence " + granted.value().grant.value().evidence_digest.to_hex();
  return result;
}

}  // namespace

int main() {
  std::cout << "example 07: restart, recovery and replay of a staged request\n";
  std::filesystem::path directory;
  try {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = std::filesystem::temp_directory_path() /
                ("fdc-example-07-" + std::to_string(unique));
  } catch (const std::filesystem::filesystem_error& error) {
    std::cout << "temporary directory unavailable: " << error.what() << '\n';
    return 1;
  }

  const PhaseResult first = phase_one(directory);
  std::cout << first.text << '\n';
  if (!first.ok) {
    return 1;
  }
  const std::size_t key_position = first.text.find("key ");
  const std::string staged_key =
      key_position == std::string::npos ? std::string{} : first.text.substr(key_position + 4U);

  const PhaseResult second = phase_two(directory, staged_key);
  std::cout << second.text << '\n';
  if (!second.ok) {
    return 1;
  }

  const PhaseResult third = phase_three(directory);
  std::cout << third.text << '\n';
  if (!third.ok) {
    return 1;
  }

  std::error_code ignored;
  const std::uintmax_t removed = std::filesystem::remove_all(directory, ignored);
  std::cout << "store removed " << (ignored ? "no" : "yes") << " entries " << removed << '\n';
  return ignored ? 1 : 0;
}
