// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_TESTS_SUPPORT_FDC_TEST_SUPPORT_HPP
#define FACILITYDRAIN_TESTS_SUPPORT_FDC_TEST_SUPPORT_HPP

#include "facilitydrain/facility_drain_coordinator.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fdc_test {

using namespace facilitydrain;  // NOLINT(google-build-using-namespace) - deliberate in test support

/// Fails the current test by throwing; the harness reports it as a failed test
/// with the message attached.
[[noreturn]] inline void fail_now(std::string message) { throw std::runtime_error{std::move(message)}; }

inline void require_ok(const Status& status, const char* what) {
  if (!status.ok()) {
    fail_now(std::string{what} + " failed with " + std::string{to_token(status.code())} + ": " +
             status.error().detail());
  }
}

template <class T>
T& require(Result<T>& result, const char* what) {
  if (!result) {
    fail_now(std::string{what} + " failed with " + std::string{to_token(result.error().code())} + ": " +
             result.error().detail());
  }
  return result.value();
}

/// A complete generation set with every field distinct and non default, so a
/// test that mixes up two generations is caught rather than accidentally equal.
inline GenerationSet generations(std::uint64_t base) {
  GenerationSet set;
  set.scope = ScopeGeneration{base};
  set.dependency = DependencyGeneration{base + 1U};
  set.reservation = ReservationGeneration{base + 2U};
  set.obligation = ObligationGeneration{base + 3U};
  set.policy = PolicyGeneration{base + 4U};
  set.topology = TopologyGeneration{base + 5U};
  set.maintenance = MaintenanceGeneration{base + 6U};
  set.capacity = CapacityGeneration{base + 7U};
  set.hardware = HardwareGeneration{base + 8U};
  set.firmware = FirmwareGeneration{base + 9U};
  return set;
}

inline ConsumerRecord consumer(ConsumerCategory category, std::uint64_t obligation, std::uint64_t generation,
                               ObligationStrength strength = ObligationStrength::kMandatory,
                               std::string label = {}) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = category;
  record.generation = ObligationGeneration{generation};
  record.reservation = ReservationGeneration{0};
  record.strength = strength;
  record.label = label.empty() ? ("obligation-" + std::to_string(obligation)) : std::move(label);
  record.source = "test-registry";
  return record;
}

inline ConsumerRecord workload(std::uint64_t obligation, std::uint64_t generation = 1) {
  return consumer(ConsumerCategory::kWorkload, obligation, generation);
}

inline ConsumerRecord network_path(std::uint64_t obligation, std::uint64_t generation = 1) {
  return consumer(ConsumerCategory::kNetworkPath, obligation, generation);
}

inline ResidualEntry residual(OwnerDomain domain, std::uint64_t obligation, ResidualKind kind,
                             ResidualState state = ResidualState::kOpen, std::string detail = {}) {
  ResidualEntry entry;
  entry.obligation = ObligationId{obligation};
  entry.domain = domain;
  entry.kind = kind;
  entry.state = state;
  entry.generation = ObligationGeneration{1};
  entry.detail = detail.empty() ? "test residual" : std::move(detail);
  return entry;
}

/// A coordinator plus the bookkeeping a test would otherwise repeat: a strictly
/// increasing observation counter, the current incarnation and epoch, and the
/// plan revision. Every helper either succeeds or fails the test loudly, so a
/// broken precondition can never be mistaken for a passing scenario.
class Scaffold {
 public:
  explicit Scaffold(Coordinator coordinator) : coordinator_(std::move(coordinator)) {
    // A restarted writer continues where the recovered state left off. Starting
    // a fresh counter would look like a replay of observations the store has
    // already accepted, which the coordinator rightly refuses.
    observation_ = coordinator_.observation_sequence().value();
    const auto current = coordinator_.snapshot();
    if (current) {
      for (const DrainPlanSnapshot& plan : current.value().plans) {
        observation_ = observation_ > plan.last_observation.value() ? observation_
                                                                   : plan.last_observation.value();
      }
    }
  }

  static Scaffold ephemeral(Limits limits = {}) {
    EphemeralOptions options;
    options.limits = limits;
    options.clock = std::make_shared<const FixedClock>(1700000000000);
    auto coordinator = Coordinator::open_ephemeral(options);
    return Scaffold{std::move(require(coordinator, "open_ephemeral"))};
  }

  static Scaffold durable(const std::filesystem::path& root, Limits limits = {},
                          PublishFaultHooks faults = {}) {
    CoordinatorOpenRequest request;
    request.root = root;
    request.limits = limits;
    request.faults = faults;
    request.writer_label = "fdc-tests";
    request.clock = std::make_shared<const FixedClock>(1700000000000);
    auto coordinator = Coordinator::open(request);
    return Scaffold{std::move(require(coordinator, "Coordinator::open"))};
  }

  [[nodiscard]] Coordinator& coordinator() noexcept { return coordinator_; }
  [[nodiscard]] const Coordinator& coordinator() const noexcept { return coordinator_; }
  [[nodiscard]] std::uint64_t observation() const noexcept { return observation_; }
  void set_observation(std::uint64_t value) noexcept { observation_ = value; }

  /// Advances the observation counter and builds a context for a mutation.
  MutationContext next(PlanId plan, Revision revision) {
    ++observation_;
    MutationContext context;
    context.plan = plan;
    context.expected_revision = revision;
    context.incarnation = coordinator_.incarnation();
    context.expected_epoch = coordinator_.control_epoch();
    context.observation = ObservationSequence{observation_};
    context.principal = "fdc-test";
    context.requested_at_milliseconds = 1700000000000;
    return context;
  }

  DrainPlanSnapshot plan(PlanId id) {
    auto view = coordinator_.plan(id);
    return require(view, "Coordinator::plan");
  }

  Revision revision_of(PlanId id) { return plan(id).spec.revision; }

  DrainPlanSnapshot create_plan(PlanId id, std::vector<ConsumerRecord> consumers = {},
                                              std::vector<DrainScope> targets = {},
                                              DomainMask required = DomainMask::of(OwnerDomain::kAsi)
                                                                        .with(OwnerDomain::kDfi),
                                              GenerationSet set = generations(1),
                                              bool bind_manifests = true) {
    CreatePlanRequest request;
    request.context = next(id, Revision{1});
    request.id = id;
    if (targets.empty()) {
      targets.push_back(DrainScope{ScopeKind::kRack, 9});
    }
    request.scope = targets.front();
    request.targets = std::move(targets);
    request.declared_required_domains = required;
    request.generations = set;
    request.policy_id = PolicyId{7};
    request.policy_digest = digest_text("test-policy-v1");
    request.consumers = std::move(consumers);
    request.bind_consumer_manifests = bind_manifests;
    request.label = "test-plan";
    request.requested_by = "fdc-tests";
    auto outcome = coordinator_.create_plan(request);
    return require(outcome, "create_plan").plan;
  }

  RecordEnumerationOutcome enumerate(PlanId id, OwnerDomain domain, CoverageState coverage,
                                                   std::uint64_t generation,
                                                   std::vector<ConsumerRecord> consumers,
                                                   GenerationSet set = generations(1)) {
    RecordEnumerationRequest request;
    request.context = next(id, revision_of(id));
    request.domain = domain;
    request.coverage = coverage;
    request.generation = EvidenceGeneration{generation};
    request.observed_at = ObservationSequence{observation_};
    request.scope_manifest_digest = plan(id).spec.targets.digest();
    request.consumers = std::move(consumers);
    request.generations = set;
    request.source = "test-owner";
    request.annotation = "test enumeration";
    auto outcome = coordinator_.record_enumeration(request);
    return require(outcome, "record_enumeration");
  }

  IssueRequestsOutcome issue(PlanId id, DomainMask domains, std::uint32_t bound = 0) {
    IssueRequestsRequest request;
    request.context = next(id, revision_of(id));
    request.domains = domains;
    request.bound_operations = bound;
    auto outcome = coordinator_.issue_requests(request);
    return require(outcome, "issue_requests");
  }

  void deliver(PlanId id, DrainRequestId request_id) {
    ConfirmDeliveryRequest request;
    request.context = next(id, revision_of(id));
    request.request = request_id;
    request.delivery_reference = "test-adapter";
    auto outcome = coordinator_.confirm_delivery(request);
    require(outcome, "confirm_delivery");
  }

  void acknowledge(PlanId id, DrainRequestId request_id) {
    RecordAcknowledgementRequest request;
    request.context = next(id, revision_of(id));
    request.request = request_id;
    request.acknowledging_system = "test-owner";
    auto outcome = coordinator_.record_acknowledgement(request);
    require(outcome, "record_acknowledgement");
  }

  /// Delivers and acknowledges every outstanding request of a plan, which is
  /// the ordinary path from Requested to Draining.
  void deliver_and_acknowledge_all(PlanId id) {
    auto requests = coordinator_.requests(id);
    const std::vector<DrainRequest> recorded = require(requests, "Coordinator::requests");
    for (const auto& request : recorded) {
      if (request.state == RequestState::kStaged) {
        deliver(id, request.id);
      }
    }
    for (const auto& request : recorded) {
      if (request.state == RequestState::kStaged || request.state == RequestState::kIssued) {
        acknowledge(id, request.id);
      }
    }
  }

  IngestCompletionOutcome ingest(PlanId id, OwnerDomain domain, CompletionState state,
                                               std::uint64_t generation, bool count_known,
                                               std::uint64_t count,
                                               std::vector<ResidualEntry> residuals = {},
                                               GenerationSet set = generations(1)) {
    IngestCompletionRequest request;
    request.context = next(id, revision_of(id));
    request.domain = domain;
    request.state = state;
    request.generation = EvidenceGeneration{generation};
    request.observed_at = ObservationSequence{observation_};
    request.payload_digest = digest_text("report-" + std::to_string(generation) + "-" +
                                         std::string{to_token(domain)});
    // The snapshot is materialised first: binding a reference to a member of a
    // temporary that a function returned does not extend that temporary's
    // lifetime, and the reference would dangle before it is read.
    const DrainPlanSnapshot current = plan(id);
    request.scope_manifest_digest = current.spec.targets.digest();
    const auto& bound = current.spec.bindings.manifest_digest(domain);
    request.manifest_digest = bound.has_value()
                                  ? bound.value()
                                  : consumer_manifest_digest(domain, {});
    request.generations = set;
    request.residual_count_known = count_known;
    request.residual_count = count;
    request.residuals = std::move(residuals);
    request.source = "test-owner";
    request.annotation = "test completion";
    auto outcome = coordinator_.ingest_completion(request);
    return require(outcome, "ingest_completion");
  }

  RecordResidualOutcome record_residual(PlanId id, ResidualEntry entry) {
    RecordResidualRequest request;
    request.context = next(id, revision_of(id));
    request.entry = std::move(entry);
    auto outcome = coordinator_.record_residual(request);
    return require(outcome, "record_residual");
  }

  ResolveResidualOutcome resolve_residual(PlanId id, OwnerDomain domain, std::uint64_t obligation,
                                                        ResidualKind kind, std::uint64_t evidence_generation) {
    ResolveResidualRequest request;
    request.context = next(id, revision_of(id));
    request.domain = domain;
    request.obligation = ObligationId{obligation};
    request.kind = kind;
    request.resolution_evidence_generation = EvidenceGeneration{evidence_generation};
    request.detail = "relinquished by the owning system";
    auto outcome = coordinator_.resolve_residual(request);
    return require(outcome, "resolve_residual");
  }

  SafeToRemoveEvaluation evaluate(PlanId id) {
    auto evaluation = coordinator_.evaluate_safe_to_remove(id);
    return require(evaluation, "evaluate_safe_to_remove");
  }

  SafeToRemoveGrant grant(PlanId id) {
    GrantSafeToRemoveRequest request;
    request.context = next(id, revision_of(id));
    request.granted_by = "fdc-tests";
    auto outcome = coordinator_.grant_safe_to_remove(request);
    if (!outcome) {
      fail_now("grant_safe_to_remove was denied with " + std::string{to_token(outcome.error().code())} + ": " +
               outcome.error().detail());
    }
    if (!outcome.value().grant.has_value()) {
      fail_now("grant_safe_to_remove succeeded without recording a grant");
    }
    return outcome.value().grant.value();
  }

  /// Brings one domain all the way from enumeration to a proven drained state.
  void prove_drained(PlanId id, OwnerDomain domain, std::vector<ConsumerRecord> consumers,
                     std::uint64_t enumeration_generation, std::uint64_t completion_generation,
                     GenerationSet set = generations(1)) {
    enumerate(id, domain, CoverageState::kComplete, enumeration_generation, std::move(consumers), set);
    ingest(id, domain, CompletionState::kDrained, completion_generation, true, 0, {}, set);
  }

  /// The full ordinary flow for a plan with two required domains and no
  /// consumers: a proven empty manifest on both sides.
  void prove_empty_scope(PlanId id, DomainMask domains) {
    std::uint64_t generation = 1;
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      const OwnerDomain domain = owner_domain_at(index);
      if (!domains.contains(domain)) {
        continue;
      }
      enumerate(id, domain, CoverageState::kComplete, generation, {});
      ++generation;
    }
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      const OwnerDomain domain = owner_domain_at(index);
      if (!domains.contains(domain)) {
        continue;
      }
      ingest(id, domain, CompletionState::kDrained, generation, true, 0);
      ++generation;
    }
  }

 private:
  Coordinator coordinator_;
  std::uint64_t observation_ = 1000;
};

/// A temporary directory that removes itself, including when a test fails by
/// throwing. Tests that leave directories behind make the next run's evidence
/// unreliable, so cleanup must not depend on reaching the end of the test.
class TempDir {
 public:
  explicit TempDir(std::string_view label) : path_(make_temp_directory(label)) {}
  ~TempDir() { remove_tree(path_); }
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::filesystem::path sub(std::string_view name) const {
    return path_ / std::string{name};
  }

 private:
  std::filesystem::path path_;
};

/// Counts the files a directory holds whose name starts with a prefix.
inline std::size_t count_files_with_prefix(const std::filesystem::path& root, std::string_view prefix) {
  std::size_t total = 0;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator{root, error}) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(std::string{prefix}, 0) == 0) {
      ++total;
    }
  }
  return total;
}

/// A deterministic pseudo random generator, so every randomized test is
/// reproducible from the seed it prints.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  [[nodiscard]] std::uint64_t next() {
    // SplitMix64: small, fast and identical on every platform.
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::uint32_t below(std::uint32_t bound) {
    return bound == 0 ? 0U : static_cast<std::uint32_t>(next() % bound);
  }

  [[nodiscard]] bool chance(std::uint32_t numerator, std::uint32_t denominator) {
    return below(denominator) < numerator;
  }

 private:
  std::uint64_t state_;
};

}  // namespace fdc_test

#endif  // FACILITYDRAIN_TESTS_SUPPORT_FDC_TEST_SUPPORT_HPP