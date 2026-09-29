// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

// An independent, out-of-tree consumer of the installed Facility Drain
// Coordinator package. It is configured with find_package against an installed
// prefix and links only the exported
// FacilityDrainCoordinator::facility_drain_coordinator target, so it exercises
// exactly what the package publishes: the umbrella header and the library.
//
// The scenario is one drain plan for rack 9 that covers assets 41 and 42. ASI
// and DFI must both prove the scope drained before removal authority exists, so
// the consumer walks the whole documented path: plan, enumerate, issue bounded
// requests, confirm delivery, record acknowledgements, ingest completions, and
// read the verdict after each domain reports. Every input, including the clock,
// is fixed, so two runs behave identically.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <facilitydrain/facility_drain_coordinator.hpp>

namespace {

namespace fd = facilitydrain;

// Fixed inputs for the whole run.
constexpr std::int64_t kFixedClockMilliseconds = 1'700'000'000'000;
constexpr std::uint64_t kIncarnation = 7;
constexpr std::uint64_t kPlanId = 11;
constexpr std::uint64_t kRackId = 9;
constexpr std::uint64_t kFirstAssetId = 41;
constexpr std::uint64_t kSecondAssetId = 42;
constexpr std::uint64_t kPolicyId = 3;
constexpr std::uint64_t kEnumerationEvidenceGeneration = 1;
constexpr std::uint64_t kCompletionEvidenceGeneration = 2;
constexpr std::uint32_t kBoundOperations = 4;
constexpr std::uint64_t kAsiObligationId = 1001;
constexpr std::uint64_t kDfiObligationId = 2002;
constexpr std::uint64_t kObligationGeneration = 1;
constexpr char kPrincipal[] = "package-consumer";
constexpr char kAsiOwner[] = "asi-owner";
constexpr char kDfiOwner[] = "dfi-owner";

int fail(const std::string& message) {
  std::cerr << "package consumer failure: " << message << "\n";
  return 1;
}

// Bounded limits for the whole run. The scenario stays far below every one of
// them, and the coordinator is asked to accept them explicitly rather than
// inheriting whatever the default happens to be.
fd::Limits fixed_limits() {
  fd::Limits limits;
  limits.max_plans = 4;
  limits.max_targets_per_plan = 16;
  limits.max_consumers_per_domain = 64;
  limits.max_evidence_per_domain = 8;
  limits.max_requests_per_plan = 16;
  limits.max_residuals_per_plan = 64;
  limits.max_history_per_plan = 64;
  limits.max_text_bytes = 128;
  limits.max_annotation_bytes = 512;
  limits.max_state_bytes = 4ULL * 1024ULL * 1024ULL;
  limits.max_document_bytes = 4ULL * 1024ULL * 1024ULL;
  limits.max_attempts_per_key = 4;
  return limits;
}

// The generation set the plan is bound to. Every participating generation is
// non default, because a default generation means "never observed" and is never
// compatible with a plan that names a real one.
fd::GenerationSet bound_generations() {
  fd::GenerationSet generations;
  generations.scope = fd::ScopeGeneration{1};
  generations.dependency = fd::DependencyGeneration{2};
  generations.reservation = fd::ReservationGeneration{3};
  generations.obligation = fd::ObligationGeneration{4};
  generations.policy = fd::PolicyGeneration{5};
  generations.topology = fd::TopologyGeneration{6};
  generations.maintenance = fd::MaintenanceGeneration{7};
  generations.capacity = fd::CapacityGeneration{8};
  generations.hardware = fd::HardwareGeneration{9};
  generations.firmware = fd::FirmwareGeneration{10};
  return generations;
}

// The consumer's driver for one plan. It owns the coordinator, remembers the
// generation set the plan was bound to, and hands out the mutation contexts and
// observation sequences every request must carry.
//
// A mutation context states the revision the caller believes is current. This
// session re-reads that revision from the coordinator immediately before it
// mutates, which is what a caller that has been talking to other systems has to
// do; it never assumes a revision it read earlier is still current.
class ConsumerSession {
 public:
  ConsumerSession(fd::Coordinator coordinator, fd::PlanId plan, const fd::GenerationSet& generations)
      : coordinator_(std::move(coordinator)),
        plan_(plan),
        generations_(generations),
        observation_(coordinator_.observation_sequence()) {}

  [[nodiscard]] fd::Coordinator& coordinator() noexcept { return coordinator_; }
  [[nodiscard]] fd::PlanId plan() const noexcept { return plan_; }
  [[nodiscard]] const fd::GenerationSet& generations() const noexcept { return generations_; }

  // The context for the plan's own creation. A mutation context always names a
  // concrete revision and the coordinator rejects a default one, so this names
  // the revision the plan is created at: a plan's revisions start at one.
  [[nodiscard]] fd::MutationContext creation_context() { return build_context(fd::Revision{1}); }

  // The context for the next mutation of a plan that already exists.
  [[nodiscard]] fd::Result<fd::MutationContext> next_context() {
    const fd::Result<fd::DrainPlanSnapshot> snapshot = coordinator_.plan(plan_);
    if (!snapshot) {
      return snapshot.error();
    }
    return build_context(snapshot.value().revision);
  }

  // The observation sequence the next request or piece of evidence is stamped
  // with. It only ever moves forward, so nothing this consumer does can be a
  // replay of something it did earlier.
  [[nodiscard]] fd::ObservationSequence next_observation() noexcept {
    observation_ = fd::ObservationSequence{observation_.value() + 1U};
    return observation_;
  }

 private:
  [[nodiscard]] fd::MutationContext build_context(fd::Revision revision) {
    fd::MutationContext context;
    context.plan = plan_;
    context.expected_revision = revision;
    context.incarnation = coordinator_.incarnation();
    context.expected_epoch = coordinator_.control_epoch();
    context.observation = next_observation();
    context.principal = kPrincipal;
    // Zero selects the coordinator's clock, which is the fixed clock here.
    context.requested_at_milliseconds = 0;
    return context;
  }

  fd::Coordinator coordinator_;
  fd::PlanId plan_{};
  fd::GenerationSet generations_{};
  fd::ObservationSequence observation_{};
};

// Fills in the mutation context of a request, or reports why it could not be
// built.
[[nodiscard]] fd::Status prepare_context(fd::MutationContext& context, ConsumerSession& session) {
  const fd::Result<fd::MutationContext> next = session.next_context();
  if (!next) {
    return fd::Status::failure(next.error());
  }
  context = next.value();
  return fd::Status::success();
}

}  // namespace

int main() {
  const fd::Limits limits = fixed_limits();
  const fd::Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return fail("the fixed limits are not valid: " + limits_status.to_text());
  }

  fd::EphemeralOptions options;
  options.limits = limits;
  options.clock = std::make_shared<fd::FixedClock>(kFixedClockMilliseconds);
  options.incarnation = fd::IncarnationId{kIncarnation};

  fd::Result<fd::Coordinator> opened = fd::Coordinator::open_ephemeral(options);
  if (!opened) {
    return fail("open an ephemeral coordinator: " + opened.error().to_text());
  }
  if (opened.value().durable()) {
    return fail("an ephemeral coordinator reported itself durable");
  }

  const fd::PlanId plan{kPlanId};
  ConsumerSession session{std::move(opened).value(), plan, bound_generations()};

  // -- the physical scope and the membership the plan covers -----------------
  const fd::DrainScope scope{fd::ScopeKind::kRack, kRackId};
  const std::vector<fd::DrainScope> targets{
      fd::DrainScope{fd::ScopeKind::kAsset, kFirstAssetId},
      fd::DrainScope{fd::ScopeKind::kAsset, kSecondAssetId},
      scope};

  const fd::Result<fd::DrainTargetManifest> manifest = fd::DrainTargetManifest::create(targets, limits);
  if (!manifest) {
    return fail("build the target manifest: " + manifest.error().to_text());
  }
  const fd::ContentDigest scope_manifest_digest = manifest.value().digest();

  // Two real obligations depend on this scope: one accelerator workload owned
  // by ASI and one fabric path owned by DFI. The plan binds the two manifests
  // when it is created, and every later record has to agree with the binding,
  // which is what makes the two drain requests meaningful.
  fd::ConsumerRecord asi_workload;
  asi_workload.obligation = fd::ObligationId{kAsiObligationId};
  asi_workload.category = fd::ConsumerCategory::kWorkload;
  asi_workload.generation = fd::ObligationGeneration{kObligationGeneration};
  asi_workload.strength = fd::ObligationStrength::kMandatory;
  asi_workload.label = "training-job-7";
  asi_workload.source = kAsiOwner;

  fd::ConsumerRecord dfi_path;
  dfi_path.obligation = fd::ObligationId{kDfiObligationId};
  dfi_path.category = fd::ConsumerCategory::kNetworkPath;
  dfi_path.generation = fd::ObligationGeneration{kObligationGeneration};
  dfi_path.strength = fd::ObligationStrength::kMandatory;
  dfi_path.label = "fabric-path-9";
  dfi_path.source = kDfiOwner;

  const std::vector<fd::ConsumerRecord> asi_consumers{asi_workload};
  const std::vector<fd::ConsumerRecord> dfi_consumers{dfi_path};
  const std::vector<fd::ConsumerRecord> all_consumers{asi_workload, dfi_path};
  const fd::ContentDigest asi_manifest_digest = fd::consumer_manifest_digest(fd::OwnerDomain::kAsi, asi_consumers);
  const fd::ContentDigest dfi_manifest_digest = fd::consumer_manifest_digest(fd::OwnerDomain::kDfi, dfi_consumers);

  // -- create the plan ------------------------------------------------------
  fd::CreatePlanRequest create;
  create.context = session.creation_context();
  create.id = plan;
  create.scope = scope;
  create.targets = targets;
  create.declared_required_domains = fd::DomainMask::of(fd::OwnerDomain::kAsi).with(fd::OwnerDomain::kDfi);
  create.generations = session.generations();
  create.policy_id = fd::PolicyId{kPolicyId};
  create.policy_digest = fd::digest_text("consumer-policy/1");
  create.consumers = all_consumers;
  create.bind_consumer_manifests = true;
  create.label = "rack 9 drain";
  create.requested_by = kPrincipal;

  const fd::Result<fd::CreatePlanOutcome> created = session.coordinator().create_plan(create);
  if (!created) {
    return fail("create the drain plan: " + created.error().to_text());
  }
  if (created.value().plan.spec.id != plan) {
    return fail("the created plan does not carry the requested plan id");
  }
  if (created.value().plan.spec.revision.is_default()) {
    return fail("the created plan has no revision");
  }
  if (created.value().plan.state != fd::DrainState::kProposed) {
    return fail("a plan with no recorded evidence is not Proposed");
  }
  if (created.value().plan.required_domains != create.declared_required_domains) {
    return fail("the plan's required domains are not the domains it was created with");
  }
  if (created.value().plan.spec.targets.size() != targets.size() ||
      !created.value().plan.spec.targets.contains(scope)) {
    return fail("the plan's target manifest does not hold the requested membership");
  }
  if (created.value().plan.spec.targets.digest() != scope_manifest_digest) {
    return fail("the plan's target manifest digest does not match the requested membership");
  }

  // -- ASI enumerates the scope ---------------------------------------------
  fd::RecordEnumerationRequest asi_enumeration;
  if (const fd::Status ready = prepare_context(asi_enumeration.context, session); !ready.ok()) {
    return fail("read the plan before the ASI enumeration: " + ready.to_text());
  }
  asi_enumeration.domain = fd::OwnerDomain::kAsi;
  asi_enumeration.coverage = fd::CoverageState::kComplete;
  asi_enumeration.generation = fd::EvidenceGeneration{kEnumerationEvidenceGeneration};
  asi_enumeration.observed_at = session.next_observation();
  asi_enumeration.scope_manifest_digest = scope_manifest_digest;
  asi_enumeration.consumers = asi_consumers;
  asi_enumeration.generations = session.generations();
  asi_enumeration.source = kAsiOwner;
  asi_enumeration.annotation = "complete enumeration of rack 9";

  const fd::Result<fd::RecordEnumerationOutcome> asi_recorded =
      session.coordinator().record_enumeration(asi_enumeration);
  if (!asi_recorded) {
    return fail("record the ASI enumeration: " + asi_recorded.error().to_text());
  }
  if (!asi_recorded.value().accepted && !asi_recorded.value().bound_manifest) {
    return fail("the ASI enumeration was neither accepted nor bound as the manifest of record");
  }
  if (asi_recorded.value().evidence.manifest_digest != asi_manifest_digest) {
    return fail("the ASI enumeration does not carry the manifest digest of the records it was given");
  }
  if (asi_recorded.value().evidence.coverage != fd::CoverageState::kComplete) {
    return fail("the ASI enumeration was not recorded as complete");
  }

  // -- DFI enumerates the scope ---------------------------------------------
  fd::RecordEnumerationRequest dfi_enumeration;
  if (const fd::Status ready = prepare_context(dfi_enumeration.context, session); !ready.ok()) {
    return fail("read the plan before the DFI enumeration: " + ready.to_text());
  }
  dfi_enumeration.domain = fd::OwnerDomain::kDfi;
  dfi_enumeration.coverage = fd::CoverageState::kComplete;
  dfi_enumeration.generation = fd::EvidenceGeneration{kEnumerationEvidenceGeneration};
  dfi_enumeration.observed_at = session.next_observation();
  dfi_enumeration.scope_manifest_digest = scope_manifest_digest;
  dfi_enumeration.consumers = dfi_consumers;
  dfi_enumeration.generations = session.generations();
  dfi_enumeration.source = kDfiOwner;
  dfi_enumeration.annotation = "complete enumeration of rack 9";

  const fd::Result<fd::RecordEnumerationOutcome> dfi_recorded =
      session.coordinator().record_enumeration(dfi_enumeration);
  if (!dfi_recorded) {
    return fail("record the DFI enumeration: " + dfi_recorded.error().to_text());
  }
  if (!dfi_recorded.value().accepted && !dfi_recorded.value().bound_manifest) {
    return fail("the DFI enumeration was neither accepted nor bound as the manifest of record");
  }

  // -- bounded requests to both owning systems ------------------------------
  fd::IssueRequestsRequest issue;
  if (const fd::Status ready = prepare_context(issue.context, session); !ready.ok()) {
    return fail("read the plan before issuing the drain requests: " + ready.to_text());
  }
  issue.domains = fd::DomainMask::of(fd::OwnerDomain::kAsi).with(fd::OwnerDomain::kDfi);
  issue.bound_operations = kBoundOperations;

  const fd::Result<fd::IssueRequestsOutcome> issued = session.coordinator().issue_requests(issue);
  if (!issued) {
    return fail("issue the bounded drain requests: " + issued.error().to_text());
  }
  if (issued.value().to_deliver.empty()) {
    return fail("issuing requests for the two required domains produced nothing to deliver");
  }
  if (issued.value().newly_staged != issued.value().to_deliver.size()) {
    return fail("the staged request count does not match the canonical delivery list");
  }

  bool requested_asi = false;
  bool requested_dfi = false;
  for (const fd::IssueItem& item : issued.value().items) {
    if (!item.deliver) {
      continue;
    }
    if (item.request.id.is_default()) {
      return fail("a drain request was staged without an id");
    }
    if (item.request.idempotency_key.is_zero()) {
      return fail("a drain request was staged without an idempotency key");
    }
    if (item.request.state != fd::RequestState::kStaged) {
      return fail("a request offered for delivery is not staged");
    }
    if (item.request.bound_operations == 0) {
      return fail("a drain request was staged without a bound");
    }
    if (item.request.key.domain == fd::OwnerDomain::kAsi) {
      requested_asi = true;
    } else if (item.request.key.domain == fd::OwnerDomain::kDfi) {
      requested_dfi = true;
    }

    fd::ConfirmDeliveryRequest delivery;
    if (const fd::Status ready = prepare_context(delivery.context, session); !ready.ok()) {
      return fail("read the plan before confirming delivery: " + ready.to_text());
    }
    delivery.request = item.request.id;
    delivery.delivery_reference = "consumer-delivery-" + fd::to_string(item.request.id);

    const fd::Result<fd::ConfirmDeliveryOutcome> confirmed = session.coordinator().confirm_delivery(delivery);
    if (!confirmed) {
      return fail("confirm delivery of request " + fd::to_string(item.request.id) + ": " +
                  confirmed.error().to_text());
    }
    if (confirmed.value().request.state != fd::RequestState::kIssued) {
      return fail("a confirmed delivery is not in the issued state");
    }

    fd::RecordAcknowledgementRequest acknowledgement;
    if (const fd::Status ready = prepare_context(acknowledgement.context, session); !ready.ok()) {
      return fail("read the plan before recording the acknowledgement: " + ready.to_text());
    }
    acknowledgement.request = item.request.id;
    acknowledgement.acknowledging_system = "consumer-owner";

    const fd::Result<fd::RecordAcknowledgementOutcome> acknowledged =
        session.coordinator().record_acknowledgement(acknowledgement);
    if (!acknowledged) {
      return fail("record the acknowledgement of request " + fd::to_string(item.request.id) + ": " +
                  acknowledged.error().to_text());
    }
    if (acknowledged.value().request.state != fd::RequestState::kAcknowledged) {
      return fail("an acknowledged request is not in the acknowledged state");
    }
  }

  if (!requested_asi || !requested_dfi) {
    return fail("the staged requests do not cover both required domains");
  }

  // -- ASI reports the scope drained ----------------------------------------
  fd::IngestCompletionRequest asi_completion;
  if (const fd::Status ready = prepare_context(asi_completion.context, session); !ready.ok()) {
    return fail("read the plan before the ASI completion: " + ready.to_text());
  }
  asi_completion.domain = fd::OwnerDomain::kAsi;
  asi_completion.state = fd::CompletionState::kDrained;
  asi_completion.generation = fd::EvidenceGeneration{kCompletionEvidenceGeneration};
  asi_completion.observed_at = session.next_observation();
  asi_completion.payload_digest = fd::digest_text("consumer-asi-drain-report");
  asi_completion.manifest_digest = asi_manifest_digest;
  asi_completion.scope_manifest_digest = scope_manifest_digest;
  asi_completion.generations = session.generations();
  asi_completion.residual_count_known = true;
  asi_completion.residual_count = 0;
  asi_completion.source = kAsiOwner;
  asi_completion.annotation = "ASI drained the scope and names no residual";

  const fd::Result<fd::IngestCompletionOutcome> asi_ingested =
      session.coordinator().ingest_completion(asi_completion);
  if (!asi_ingested) {
    return fail("ingest the ASI completion: " + asi_ingested.error().to_text());
  }
  if (!asi_ingested.value().compatible) {
    return fail("the ASI completion was rejected: " + std::string(fd::to_token(asi_ingested.value().rejection)));
  }

  // -- the verdict is still denied, and DFI is what blocks it ---------------
  const fd::Result<fd::SafeToRemoveEvaluation> after_asi = session.coordinator().evaluate_safe_to_remove(plan);
  if (!after_asi) {
    return fail("evaluate the verdict after the ASI completion: " + after_asi.error().to_text());
  }
  const fd::DomainAssessment& asi_assessment = after_asi.value().domains[fd::domain_index(fd::OwnerDomain::kAsi)];
  const fd::DomainAssessment& dfi_assessment = after_asi.value().domains[fd::domain_index(fd::OwnerDomain::kDfi)];

  if (after_asi.value().verdict != fd::SafeToRemoveVerdict::kDenied) {
    return fail("the verdict was granted while DFI had reported nothing");
  }
  if (asi_assessment.verdict != fd::DomainVerdict::kProvenComplete) {
    return fail("ASI was not proven complete after a complete enumeration and a drained completion with a known "
                "zero residual count: " + asi_assessment.reason);
  }
  if (dfi_assessment.verdict == fd::DomainVerdict::kProvenComplete) {
    return fail("DFI was proven complete without any DFI completion evidence");
  }
  if (dfi_assessment.blocking_code == fd::ErrorCode::kOk) {
    return fail("the DFI assessment carries no blocking code");
  }
  if (after_asi.value().primary_blocking_code != dfi_assessment.blocking_code) {
    return fail("the primary blocking code does not name DFI: " +
                std::string(fd::to_token(after_asi.value().primary_blocking_code)));
  }

  // -- DFI reports the scope drained ----------------------------------------
  fd::IngestCompletionRequest dfi_completion;
  if (const fd::Status ready = prepare_context(dfi_completion.context, session); !ready.ok()) {
    return fail("read the plan before the DFI completion: " + ready.to_text());
  }
  dfi_completion.domain = fd::OwnerDomain::kDfi;
  dfi_completion.state = fd::CompletionState::kDrained;
  dfi_completion.generation = fd::EvidenceGeneration{kCompletionEvidenceGeneration};
  dfi_completion.observed_at = session.next_observation();
  dfi_completion.payload_digest = fd::digest_text("consumer-dfi-drain-report");
  dfi_completion.manifest_digest = dfi_manifest_digest;
  dfi_completion.scope_manifest_digest = scope_manifest_digest;
  dfi_completion.generations = session.generations();
  dfi_completion.residual_count_known = true;
  dfi_completion.residual_count = 0;
  dfi_completion.source = kDfiOwner;
  dfi_completion.annotation = "DFI drained the scope and names no residual";

  const fd::Result<fd::IngestCompletionOutcome> dfi_ingested =
      session.coordinator().ingest_completion(dfi_completion);
  if (!dfi_ingested) {
    return fail("ingest the DFI completion: " + dfi_ingested.error().to_text());
  }
  if (!dfi_ingested.value().compatible) {
    return fail("the DFI completion was rejected: " + std::string(fd::to_token(dfi_ingested.value().rejection)));
  }

  // -- every required domain is proven, so the verdict is granted -----------
  const fd::Result<fd::SafeToRemoveEvaluation> final_evaluation =
      session.coordinator().evaluate_safe_to_remove(plan);
  if (!final_evaluation) {
    return fail("evaluate the verdict after the DFI completion: " + final_evaluation.error().to_text());
  }
  if (final_evaluation.value().verdict != fd::SafeToRemoveVerdict::kGranted) {
    return fail("the verdict is still denied after both required domains reported drained: " +
                std::string(fd::to_token(final_evaluation.value().primary_blocking_code)));
  }
  if (final_evaluation.value().primary_blocking_code != fd::ErrorCode::kOk) {
    return fail("a granted verdict still names a blocking code");
  }
  for (std::uint32_t index = 0; index < fd::kOwnerDomainCount; ++index) {
    if (!final_evaluation.value().domains[index].required) {
      continue;
    }
    if (final_evaluation.value().domains[index].verdict != fd::DomainVerdict::kProvenComplete) {
      return fail("a required domain was not proven complete in a granted verdict");
    }
  }

  std::cout << "package consumer: " << fd::to_token(final_evaluation.value().verdict)
            << " plan=" << fd::to_string(plan)
            << " blocking=" << fd::to_token(final_evaluation.value().primary_blocking_code) << "\n";
  return 0;
}
