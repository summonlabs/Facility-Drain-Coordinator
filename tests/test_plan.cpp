// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace facilitydrain;

namespace {

/// One fixed instant, so every diagnostic timestamp is reproducible.
constexpr std::int64_t kClockMilliseconds = 1767225600000;

/// The incarnation every test coordinator is opened with, so an ephemeral
/// coordinator is reproducible rather than derived from the process and the
/// machine clock.
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

/// A context that names the live incarnation, the live epoch, the plan's live
/// revision and an observation strictly beyond the last accepted one.
[[nodiscard]] MutationContext context_for(const Coordinator& coordinator, PlanId plan) {
  MutationContext context;
  context.plan = plan;
  context.expected_revision = current_revision(coordinator, plan);
  context.incarnation = coordinator.incarnation();
  context.expected_epoch = coordinator.control_epoch();
  context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  context.principal = "plan-tests";
  context.requested_at_milliseconds = kClockMilliseconds;
  return context;
}

[[nodiscard]] ConsumerRecord asi_obligation(ObligationId obligation,
                                            ObligationStrength strength = ObligationStrength::kMandatory) {
  ConsumerRecord record;
  record.obligation = obligation;
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{100};
  record.reservation = ReservationGeneration{7};
  record.strength = strength;
  record.label = "workload-alpha";
  record.source = "asi";
  return record;
}

[[nodiscard]] ConsumerRecord dfi_obligation(ObligationId obligation,
                                            ObligationStrength strength = ObligationStrength::kMandatory) {
  ConsumerRecord record;
  record.obligation = obligation;
  record.category = ConsumerCategory::kNetworkPath;
  record.generation = ObligationGeneration{200};
  record.reservation = ReservationGeneration{8};
  record.strength = strength;
  record.label = "path-beta";
  record.source = "dfi";
  return record;
}

/// The valid creation request every rejection test perturbs in exactly one way.
[[nodiscard]] CreatePlanRequest create_request(const Coordinator& coordinator, PlanId id) {
  CreatePlanRequest request;
  request.context = context_for(coordinator, id);
  request.id = id;
  request.scope = DrainScope{ScopeKind::kAsset, 1};
  request.targets = std::vector<DrainScope>{request.scope};
  request.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  request.generations = complete_generations();
  request.policy_id = PolicyId{11};
  request.policy_digest = digest_text("policy-document");
  request.consumers = std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001})};
  request.label = "drain asset 1";
  request.requested_by = "operator";
  return request;
}

}  // namespace

FDC_TEST(plan, creates_a_plan_with_valid_input) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<CreatePlanOutcome> created = coordinator.create_plan(create_request(coordinator, PlanId{1}));
  FDC_REQUIRE_OK(created);
  const DrainPlanSnapshot& plan = created.value().plan;

  FDC_CHECK_EQ(plan.spec.id, PlanId{1});
  FDC_CHECK_EQ(format_scope(plan.spec.scope), std::string{"asset:1"});
  FDC_CHECK_EQ(plan.spec.targets.size(), std::size_t{1});
  FDC_CHECK_EQ(plan.spec.targets.to_canonical(), std::string{"asset:1"});
  FDC_CHECK(plan.spec.targets.contains(plan.spec.scope));
  FDC_CHECK_EQ(plan.spec.declared_required_domains.bits(), DomainMask::of(OwnerDomain::kAsi).bits());
  FDC_CHECK_EQ(plan.required_domains.bits(), DomainMask::of(OwnerDomain::kAsi).bits());
  FDC_CHECK_EQ(plan.spec.label, std::string{"drain asset 1"});
  FDC_CHECK_EQ(plan.spec.requested_by, std::string{"operator"});
  FDC_CHECK_EQ(plan.consumers.size(), std::size_t{1});
  FDC_CHECK_EQ(plan.spec.created_at_milliseconds, kClockMilliseconds);
  FDC_CHECK_EQ(plan.spec.bindings.policy_id, PolicyId{11});
  FDC_CHECK(!plan.spec.bindings.policy_digest.is_zero());
  FDC_CHECK_EQ(plan.spec.bindings.generations.to_canonical(), complete_generations().to_canonical());
  FDC_CHECK_EQ(plan.spec.bindings.facility_epoch, ControlEpoch{1});
  FDC_CHECK_EQ(std::string{to_token(plan.state)}, std::string{"proposed"});
  FDC_CHECK(!plan.grant_live);

  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  FDC_REQUIRE_OK(snapshot);
  FDC_CHECK_EQ(snapshot.value().plans.size(), std::size_t{1});
  FDC_CHECK_EQ(snapshot.value().control_epoch, coordinator.control_epoch());
  FDC_CHECK_EQ(snapshot.value().incarnation, coordinator.incarnation());
}

FDC_TEST(plan, revision_starts_at_one) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<CreatePlanOutcome> created = coordinator.create_plan(create_request(coordinator, PlanId{1}));
  FDC_REQUIRE_OK(created);
  FDC_CHECK_EQ(created.value().plan.spec.revision, Revision{1});
  FDC_CHECK_EQ(created.value().plan.revision, Revision{1});

  Result<DrainPlanSnapshot> reread = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(reread);
  FDC_CHECK_EQ(reread.value().spec.revision, Revision{1});
}

FDC_TEST(plan, plans_are_held_in_canonical_id_order) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{5})));
  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{2})));
  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{9})));

  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  FDC_REQUIRE_OK(snapshot);
  const std::vector<DrainPlanSnapshot>& plans = snapshot.value().plans;
  FDC_CHECK_EQ(plans.size(), std::size_t{3});
  FDC_CHECK_EQ(plans[0].spec.id, PlanId{2});
  FDC_CHECK_EQ(plans[1].spec.id, PlanId{5});
  FDC_CHECK_EQ(plans[2].spec.id, PlanId{9});
  FDC_CHECK(snapshot.value().find_plan(PlanId{5}) != nullptr);
  FDC_CHECK(snapshot.value().find_plan(PlanId{7}) == nullptr);
}

FDC_TEST(plan, creation_rejects_a_zero_plan_id) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_CHECK_CODE(coordinator.create_plan(create_request(coordinator, PlanId{0})),
                 ErrorCode::kMissingRequiredField);
}

FDC_TEST(plan, creation_rejects_a_zero_scope_id) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.scope = DrainScope{ScopeKind::kAsset, 0};
  request.targets = std::vector<DrainScope>{request.scope};
  FDC_CHECK_CODE(coordinator.create_plan(request), ErrorCode::kInvalidScope);
}

FDC_TEST(plan, creation_rejects_an_empty_required_mask) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.declared_required_domains = DomainMask::none();
  FDC_CHECK_CODE(coordinator.create_plan(request), ErrorCode::kMissingRequiredField);
}

FDC_TEST(plan, creation_rejects_an_incomplete_generation_set) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.generations.firmware = FirmwareGeneration{0};
  FDC_CHECK(!request.generations.is_complete());
  FDC_CHECK_CODE(coordinator.create_plan(request), ErrorCode::kMissingRequiredField);
}

FDC_TEST(plan, creation_rejects_a_zero_policy_digest) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.policy_digest = ContentDigest{};
  FDC_CHECK_CODE(coordinator.create_plan(request), ErrorCode::kMissingRequiredField);
}

FDC_TEST(plan, creation_rejects_a_zero_policy_id) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.policy_id = PolicyId{0};
  FDC_CHECK_CODE(coordinator.create_plan(request), ErrorCode::kMissingRequiredField);
}

FDC_TEST(plan, creation_rejects_a_manifest_without_the_scope) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.scope = DrainScope{ScopeKind::kAsset, 1};
  request.targets = std::vector<DrainScope>{DrainScope{ScopeKind::kAsset, 2}, DrainScope{ScopeKind::kRack, 3}};
  FDC_CHECK_CODE(coordinator.create_plan(request), ErrorCode::kInvalidScope);
}

FDC_TEST(plan, creation_rejects_a_duplicate_plan_id) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{1})));
  FDC_CHECK_CODE(coordinator.create_plan(create_request(coordinator, PlanId{1})),
                 ErrorCode::kDuplicateIdentifier);
}

FDC_TEST(plan, creation_rejects_a_context_that_names_another_plan) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.context = context_for(coordinator, PlanId{2});
  request.id = PlanId{1};
  FDC_CHECK_CODE(coordinator.create_plan(request), ErrorCode::kConflictingField);
}

FDC_TEST(plan, creation_rejects_the_plan_count_bound) {
  Limits limits;
  limits.max_plans = 1;
  Result<Coordinator> opened = open_coordinator(limits);
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{1})));
  FDC_CHECK_CODE(coordinator.create_plan(create_request(coordinator, PlanId{2})), ErrorCode::kTooManyEntries);

  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  FDC_REQUIRE_OK(snapshot);
  FDC_CHECK_EQ(snapshot.value().plans.size(), std::size_t{1});
}

FDC_TEST(plan, consumers_supplied_while_binding_is_declined_are_recorded_unbound) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.bind_consumer_manifests = false;
  request.consumers = std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001})};
  Result<CreatePlanOutcome> created = coordinator.create_plan(request);
  FDC_REQUIRE_OK(created);

  // Declining the binding is a statement about the digests, not about the
  // consumers: the records are kept as the planning manifest of record, and no
  // domain digest is bound until a complete enumeration binds one.
  const DrainPlanSnapshot& plan = created.value().plan;
  FDC_CHECK_EQ(plan.consumers.size(), std::size_t{1});
  FDC_CHECK_EQ(plan.consumers.front().obligation, ObligationId{1001});
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    FDC_CHECK(!plan.spec.bindings.manifest_digest(owner_domain_at(index)).has_value());
  }
  FDC_CHECK(plan.required_domains.contains(OwnerDomain::kAsi));
  FDC_CHECK_EQ(std::string{to_token(plan.state)}, std::string{"proposed"});
}

FDC_TEST(plan, creation_binds_an_empty_manifest_for_a_domain_without_consumers) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  Result<CreatePlanOutcome> created = coordinator.create_plan(create_request(coordinator, PlanId{1}));
  FDC_REQUIRE_OK(created);
  const DrainPlanBindings& bindings = created.value().plan.spec.bindings;

  const ContentDigest asi = consumer_manifest_digest(OwnerDomain::kAsi, created.value().plan.consumers);
  FDC_CHECK_EQ(bindings.manifest_digest(OwnerDomain::kAsi).value(), asi);
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    FDC_CHECK(bindings.manifest_digest(domain).has_value());
    FDC_CHECK(!bindings.manifest_digest(domain).value().is_zero());
    if (domain != OwnerDomain::kAsi) {
      FDC_CHECK_EQ(bindings.manifest_digest(domain).value(),
                   consumer_manifest_digest(domain, std::span<const ConsumerRecord>{}));
    }
  }
}

FDC_TEST(plan, creation_orders_the_manifest_canonically) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.targets = std::vector<DrainScope>{DrainScope{ScopeKind::kRack, 9}, DrainScope{ScopeKind::kAsset, 1},
                                            DrainScope{ScopeKind::kAsset, 3}};
  Result<CreatePlanOutcome> created = coordinator.create_plan(request);
  FDC_REQUIRE_OK(created);

  const std::vector<DrainScope>& targets = created.value().plan.spec.targets.targets();
  FDC_CHECK_EQ(targets.size(), std::size_t{3});
  FDC_CHECK_EQ(targets[0], (DrainScope{ScopeKind::kAsset, 1}));
  FDC_CHECK_EQ(targets[1], (DrainScope{ScopeKind::kAsset, 3}));
  FDC_CHECK_EQ(targets[2], (DrainScope{ScopeKind::kRack, 9}));
  FDC_CHECK_EQ(created.value().plan.spec.targets.to_canonical(), std::string{"asset:1|asset:3|rack:9"});
}

FDC_TEST(plan, the_declared_requirement_is_widened_by_mandatory_obligations) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  CreatePlanRequest request = create_request(coordinator, PlanId{1});
  request.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
  request.consumers = std::vector<ConsumerRecord>{asi_obligation(ObligationId{1001}),
                                                  dfi_obligation(ObligationId{2002}),
                                                  dfi_obligation(ObligationId{2003}, ObligationStrength::kAdvisory)};
  Result<CreatePlanOutcome> created = coordinator.create_plan(request);
  FDC_REQUIRE_OK(created);

  const DrainPlanSnapshot& plan = created.value().plan;
  FDC_CHECK_EQ(plan.spec.declared_required_domains.bits(), DomainMask::of(OwnerDomain::kAsi).bits());
  FDC_CHECK(plan.required_domains.contains(OwnerDomain::kAsi));
  FDC_CHECK(plan.required_domains.contains(OwnerDomain::kDfi));
  FDC_CHECK(!plan.required_domains.contains(OwnerDomain::kFacility));
  FDC_CHECK(!plan.required_domains.contains(OwnerDomain::kMonitoring));
  FDC_CHECK_EQ(plan.required_domains.bits(),
               DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi).bits());
  FDC_CHECK_EQ(plan.consumers.size(), std::size_t{3});
}

FDC_TEST(plan, a_revision_conflict_is_rejected) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{1})));

  CancelPlanRequest stale;
  stale.context = context_for(coordinator, PlanId{1});
  stale.context.expected_revision = Revision{2};
  stale.reason = "operator stopped the drain";
  FDC_CHECK_CODE(coordinator.cancel_plan(stale), ErrorCode::kRevisionConflict);

  Result<DrainPlanSnapshot> reread = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(reread);
  FDC_CHECK(!reread.value().cancelled);
  FDC_CHECK_EQ(reread.value().spec.revision, Revision{1});

  CancelPlanRequest current;
  current.context = context_for(coordinator, PlanId{1});
  current.reason = "operator stopped the drain";
  FDC_REQUIRE_OK(coordinator.cancel_plan(current));
}

FDC_TEST(plan, a_wrong_incarnation_is_rejected) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{1})));

  CancelPlanRequest request;
  request.context = context_for(coordinator, PlanId{1});
  request.context.incarnation = IncarnationId{kTestIncarnation + 1U};
  request.reason = "operator stopped the drain";
  FDC_CHECK_CODE(coordinator.cancel_plan(request), ErrorCode::kStaleAuthority);
}

FDC_TEST(plan, a_wrong_control_epoch_is_rejected) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{1})));

  CancelPlanRequest request;
  request.context = context_for(coordinator, PlanId{1});
  request.context.expected_epoch = ControlEpoch{coordinator.control_epoch().value() + 1U};
  request.reason = "operator stopped the drain";
  FDC_CHECK_CODE(coordinator.cancel_plan(request), ErrorCode::kEpochMismatch);
}

FDC_TEST(plan, a_plan_scoped_mutation_must_advance_that_plans_observation) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  FDC_REQUIRE_OK(coordinator.create_plan(create_request(coordinator, PlanId{1})));
  const DrainPlanSnapshot first = coordinator.plan(PlanId{1}).value();

  // Replaying the observation a plan has already accepted would apply the same
  // intent twice, so it is refused.
  CancelPlanRequest replay;
  replay.context = context_for(coordinator, PlanId{1});
  replay.context.observation = first.last_observation;
  replay.reason = "operator stopped the drain";
  FDC_CHECK_CODE(coordinator.cancel_plan(replay), ErrorCode::kInvalidGenerationOrder);

  // Creating a different plan is not ordered against this one: each plan has
  // its own observation order, and creation has no prior plan state to order
  // against. The coordinator's sequence is a high water mark, not a global lock.
  CreatePlanRequest second = create_request(coordinator, PlanId{2});
  second.context.observation = first.last_observation;
  FDC_REQUIRE_OK(coordinator.create_plan(second));

  CreatePlanRequest unset = create_request(coordinator, PlanId{3});
  unset.context.observation = ObservationSequence{0};
  FDC_CHECK_CODE(coordinator.create_plan(unset), ErrorCode::kMissingRequiredField);
}

FDC_TEST(plan, the_same_invalid_manifest_reports_the_same_code_in_every_order) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();

  ConsumerRecord broken_identity = asi_obligation(ObligationId{0});
  ConsumerRecord missing_generation = asi_obligation(ObligationId{5});
  missing_generation.generation = ObligationGeneration{0};

  CreatePlanRequest forward = create_request(coordinator, PlanId{1});
  forward.consumers = std::vector<ConsumerRecord>{broken_identity, missing_generation};
  FDC_CHECK_CODE(coordinator.create_plan(forward), ErrorCode::kInvalidIdentity);

  CreatePlanRequest backward = create_request(coordinator, PlanId{2});
  backward.consumers = std::vector<ConsumerRecord>{missing_generation, broken_identity};
  FDC_CHECK_CODE(coordinator.create_plan(backward), ErrorCode::kInvalidIdentity);

  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  FDC_REQUIRE_OK(snapshot);
  FDC_CHECK_EQ(snapshot.value().plans.size(), std::size_t{0});
}
