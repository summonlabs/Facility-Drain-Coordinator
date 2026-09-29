// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// benchmark_drain: completed operation throughput.
//
// Every number below counts operations that RETURNED SUCCESS, and the wall time
// covers the whole call including the durable publish where one happens. The
// inputs are synthetic: there is no fleet, no owner and no network, only
// generated plans and generated evidence. The durable phases therefore measure
// the coordinator's own cost -- staging a whole new generation, flushing it,
// reading it back and verifying it, and replacing the CURRENT pointer -- and
// nothing else.
//
// --quick is the small configuration the CTest entry fdc.benchmark.smoke runs.
// No argument runs the full configuration. The tool prints no comparison against
// any earlier run and claims no improvement: it reports what it measured, once.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/persistence.hpp"
#include "facilitydrain/snapshot.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace facilitydrain;

struct Configuration {
  std::string_view name{};
  std::uint32_t ephemeral_plans = 0;
  std::uint32_t durable_plans = 0;
  std::uint32_t enumerations = 0;
  std::uint32_t completions = 0;
  std::uint32_t recoveries = 0;
};

constexpr Configuration kQuick{"quick", 64, 16, 32, 32, 8};
constexpr Configuration kFull{"full", 2048, 256, 256, 256, 32};

struct Measurement {
  std::uint64_t completed = 0;
  double milliseconds = 0.0;
};

[[nodiscard]] std::string format_fixed(double value, int precision) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

void report(std::string_view label, const Measurement& measurement, std::uint64_t extra = 0,
            std::string_view extra_label = {}) {
  const double seconds = measurement.milliseconds / 1000.0;
  const double per_second = seconds > 0.0 ? static_cast<double>(measurement.completed) / seconds : 0.0;
  std::cout << label << " completed " << measurement.completed << " wall-ms "
            << format_fixed(measurement.milliseconds, 3) << " ops-per-second "
            << format_fixed(per_second, 1);
  if (!extra_label.empty()) {
    std::cout << ' ' << extra_label << ' ' << extra;
  }
  std::cout << '\n';
}

[[nodiscard]] GenerationSet benchmark_generations() {
  GenerationSet set;
  set.scope = ScopeGeneration{101};
  set.dependency = DependencyGeneration{102};
  set.reservation = ReservationGeneration{103};
  set.obligation = ObligationGeneration{104};
  set.policy = PolicyGeneration{105};
  set.topology = TopologyGeneration{106};
  set.maintenance = MaintenanceGeneration{107};
  set.capacity = CapacityGeneration{108};
  set.hardware = HardwareGeneration{109};
  set.firmware = FirmwareGeneration{110};
  return set;
}

[[nodiscard]] std::vector<ConsumerRecord> benchmark_consumers() {
  ConsumerRecord record;
  record.obligation = ObligationId{1001};
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{1};
  record.reservation = ReservationGeneration{0};
  record.strength = ObligationStrength::kMandatory;
  record.label = "synthetic-workload";
  record.source = "benchmark";
  return std::vector<ConsumerRecord>{record};
}

[[nodiscard]] DrainScope benchmark_scope() { return DrainScope{ScopeKind::kRack, 77}; }

/// One synthetic plan: one rack, two declared required domains, one mandatory
/// ASI obligation and the same ten generation values every time.
Result<CreatePlanOutcome> create_plan(Coordinator& coordinator, PlanId id) {
  CreatePlanRequest request;
  request.context.plan = id;
  request.context.expected_revision = Revision{1};
  request.context.incarnation = coordinator.incarnation();
  request.context.expected_epoch = coordinator.control_epoch();
  request.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  request.context.principal = "benchmark";
  request.id = id;
  request.scope = benchmark_scope();
  request.targets.push_back(benchmark_scope());
  request.declared_required_domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  request.generations = benchmark_generations();
  request.policy_id = PolicyId{1313};
  request.policy_digest = digest_text("benchmark-policy");
  request.consumers = benchmark_consumers();
  request.label = "synthetic";
  request.requested_by = "benchmark";
  return coordinator.create_plan(request);
}

/// The observation and revision a mutation on this plan must name.
bool next_context(Coordinator& coordinator, PlanId id, MutationContext& context, std::string& failure) {
  Result<DrainPlanSnapshot> view = coordinator.plan(id);
  if (!view) {
    failure = view.error().to_text();
    return false;
  }
  context.plan = id;
  context.expected_revision = view.value().spec.revision;
  context.incarnation = coordinator.incarnation();
  context.expected_epoch = coordinator.control_epoch();
  context.observation = ObservationSequence{view.value().last_observation.value() + 1U};
  context.principal = "benchmark";
  return true;
}

Measurement measure_ephemeral_plans(const Configuration& configuration) {
  Measurement measurement;
  EphemeralOptions options;
  options.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(0)};
  options.limits.max_plans = configuration.ephemeral_plans + 16U;
  auto opened = Coordinator::open_ephemeral(options);
  if (!opened) {
    std::cout << "benchmark failed: ephemeral open: " << opened.error().to_text() << '\n';
    return measurement;
  }
  Coordinator coordinator = std::move(opened).value();
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t index = 0; index < configuration.ephemeral_plans; ++index) {
    auto created = create_plan(coordinator, PlanId{static_cast<std::uint64_t>(index) + 1U});
    if (!created) {
      std::cout << "benchmark failed: ephemeral plan creation: " << created.error().to_text() << '\n';
      return Measurement{};
    }
    ++measurement.completed;
  }
  const auto finish = std::chrono::steady_clock::now();
  measurement.milliseconds =
      std::chrono::duration<double, std::milli>(finish - start).count();
  return measurement;
}

Measurement measure_durable_plans(const Configuration& configuration, const std::filesystem::path& root) {
  Measurement measurement;
  CoordinatorOpenRequest request;
  request.root = root;
  request.writer_label = "benchmark";
  request.limits.max_plans = configuration.durable_plans + 16U;
  request.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(0)};
  auto opened = Coordinator::open(request);
  if (!opened) {
    std::cout << "benchmark failed: durable open: " << opened.error().to_text() << '\n';
    return measurement;
  }
  Coordinator coordinator = std::move(opened).value();
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t index = 0; index < configuration.durable_plans; ++index) {
    auto created = create_plan(coordinator, PlanId{static_cast<std::uint64_t>(index) + 1U});
    if (!created) {
      std::cout << "benchmark failed: durable plan creation: " << created.error().to_text() << '\n';
      return Measurement{};
    }
    ++measurement.completed;
  }
  const auto finish = std::chrono::steady_clock::now();
  measurement.milliseconds =
      std::chrono::duration<double, std::milli>(finish - start).count();
  return measurement;
}

Measurement measure_enumerations(const Configuration& configuration, const std::filesystem::path& root) {
  Measurement measurement;
  CoordinatorOpenRequest request;
  request.root = root;
  request.writer_label = "benchmark";
  request.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(0)};
  auto opened = Coordinator::open(request);
  if (!opened) {
    std::cout << "benchmark failed: enumeration store open: " << opened.error().to_text() << '\n';
    return measurement;
  }
  Coordinator coordinator = std::move(opened).value();
  auto created = create_plan(coordinator, PlanId{1});
  if (!created) {
    std::cout << "benchmark failed: enumeration plan: " << created.error().to_text() << '\n';
    return Measurement{};
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t index = 0; index < configuration.enumerations; ++index) {
    MutationContext context;
    std::string failure;
    if (!next_context(coordinator, PlanId{1}, context, failure)) {
      std::cout << "benchmark failed: enumeration context: " << failure << '\n';
      return Measurement{};
    }
    RecordEnumerationRequest enumeration;
    enumeration.context = context;
    enumeration.domain = OwnerDomain::kAsi;
    enumeration.coverage = CoverageState::kComplete;
    enumeration.generation = EvidenceGeneration{static_cast<std::uint64_t>(index) + 1U};
    enumeration.observed_at = context.observation;
    enumeration.scope_manifest_digest = scope_digest;
    enumeration.generations = benchmark_generations();
    enumeration.source = "benchmark-asi";
    enumeration.consumers = benchmark_consumers();
    auto recorded = coordinator.record_enumeration(enumeration);
    if (!recorded || !recorded.value().accepted) {
      std::cout << "benchmark failed: enumeration: "
                << (recorded ? std::string{"not accepted"} : recorded.error().to_text()) << '\n';
      return Measurement{};
    }
    ++measurement.completed;
  }
  const auto finish = std::chrono::steady_clock::now();
  measurement.milliseconds =
      std::chrono::duration<double, std::milli>(finish - start).count();
  return measurement;
}

Measurement measure_completions(const Configuration& configuration, const std::filesystem::path& root) {
  Measurement measurement;
  CoordinatorOpenRequest request;
  request.root = root;
  request.writer_label = "benchmark";
  request.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(0)};
  auto opened = Coordinator::open(request);
  if (!opened) {
    std::cout << "benchmark failed: completion store open: " << opened.error().to_text() << '\n';
    return measurement;
  }
  Coordinator coordinator = std::move(opened).value();
  auto created = create_plan(coordinator, PlanId{1});
  if (!created) {
    std::cout << "benchmark failed: completion plan: " << created.error().to_text() << '\n';
    return Measurement{};
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();
  const ContentDigest manifest_digest =
      created.value().plan.spec.bindings.manifest_digest(OwnerDomain::kAsi).value();

  MutationContext enumeration_context;
  std::string failure;
  if (!next_context(coordinator, PlanId{1}, enumeration_context, failure)) {
    std::cout << "benchmark failed: completion context: " << failure << '\n';
    return Measurement{};
  }
  RecordEnumerationRequest enumeration;
  enumeration.context = enumeration_context;
  enumeration.domain = OwnerDomain::kAsi;
  enumeration.coverage = CoverageState::kComplete;
  enumeration.generation = EvidenceGeneration{1};
  enumeration.observed_at = enumeration_context.observation;
  enumeration.scope_manifest_digest = scope_digest;
  enumeration.generations = benchmark_generations();
  enumeration.source = "benchmark-asi";
  enumeration.consumers = benchmark_consumers();
  auto enumerated = coordinator.record_enumeration(enumeration);
  if (!enumerated) {
    std::cout << "benchmark failed: completion enumeration: " << enumerated.error().to_text() << '\n';
    return Measurement{};
  }

  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t index = 0; index < configuration.completions; ++index) {
    MutationContext context;
    if (!next_context(coordinator, PlanId{1}, context, failure)) {
      std::cout << "benchmark failed: completion context: " << failure << '\n';
      return Measurement{};
    }
    IngestCompletionRequest completion;
    completion.context = context;
    completion.domain = OwnerDomain::kAsi;
    completion.state = CompletionState::kDrained;
    completion.generation = EvidenceGeneration{static_cast<std::uint64_t>(index) + 2U};
    completion.observed_at = context.observation;
    completion.payload_digest = digest_text("benchmark-drained-report");
    completion.manifest_digest = manifest_digest;
    completion.scope_manifest_digest = scope_digest;
    completion.generations = benchmark_generations();
    completion.residual_count_known = true;
    completion.residual_count = 0;
    completion.source = "benchmark-asi";
    auto recorded = coordinator.ingest_completion(completion);
    if (!recorded || !recorded.value().compatible) {
      std::cout << "benchmark failed: completion: "
                << (recorded ? std::string{to_token(recorded.value().rejection)}
                             : recorded.error().to_text())
                << '\n';
      return Measurement{};
    }
    ++measurement.completed;
  }
  const auto finish = std::chrono::steady_clock::now();
  measurement.milliseconds =
      std::chrono::duration<double, std::milli>(finish - start).count();
  return measurement;
}

/// One completed recovery is one whole open over the published generation of a
/// store holding many plans: CURRENT is read, the container it names is
/// verified, the payload is decoded and the epoch advances.
Measurement measure_recoveries(const Configuration& configuration, const std::filesystem::path& root) {
  Measurement measurement;
  for (std::uint32_t index = 0; index < configuration.recoveries; ++index) {
    CoordinatorOpenRequest request;
    request.root = root;
    request.writer_label = "benchmark";
    request.limits.max_plans = configuration.durable_plans + 16U;
    request.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(0)};
    const auto start = std::chrono::steady_clock::now();
    auto opened = Coordinator::open(request);
    if (!opened) {
      std::cout << "benchmark failed: recovery: " << opened.error().to_text() << '\n';
      return Measurement{};
    }
    Coordinator coordinator = std::move(opened).value();
    if (!coordinator.recovery().recovered) {
      std::cout << "benchmark failed: recovery reported no recovery\n";
      return Measurement{};
    }
    const auto finish = std::chrono::steady_clock::now();
    measurement.milliseconds += std::chrono::duration<double, std::milli>(finish - start).count();
    ++measurement.completed;
  }
  return measurement;
}

}  // namespace

int main(int argc, char** argv) {
  Configuration configuration = kFull;
  if (argc > 2) {
    std::cout << "usage: fdc_benchmark [--quick]\n";
    return 2;
  }
  if (argc == 2) {
    const std::string_view word{argv[1]};
    if (word != "--quick") {
      std::cout << "usage: fdc_benchmark [--quick]\n";
      return 2;
    }
    configuration = kQuick;
  }

  std::filesystem::path root;
  try {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() / ("fdc-benchmark-" + std::to_string(unique));
  } catch (const std::filesystem::filesystem_error& error) {
    std::cout << "benchmark failed: temporary directory unavailable: " << error.what() << '\n';
    return 1;
  }

  std::cout << "facility-drain-coordinator benchmark\n";
  std::cout << "inputs synthetic yes (generated plans and generated evidence, no fleet and no "
               "network)\n";
  std::cout << "durable numbers include the full publish cost: staging, flush, read back "
               "verification, pointer replacement\n";
  std::cout << "configuration " << configuration.name << " ephemeral-plans "
            << configuration.ephemeral_plans << " durable-plans " << configuration.durable_plans
            << " enumerations " << configuration.enumerations << " completions "
            << configuration.completions << " recoveries " << configuration.recoveries << '\n';

  const Measurement ephemeral = measure_ephemeral_plans(configuration);
  report("(a) ephemeral plan creation", ephemeral);

  const Measurement durable = measure_durable_plans(configuration, root / "plans");
  report("(b) durable plan creation", durable);

  const Measurement enumerations = measure_enumerations(configuration, root / "enumerations");
  report("(c) durable enumeration ingestion", enumerations);

  const Measurement completions = measure_completions(configuration, root / "completions");
  report("(d) durable completion ingestion", completions);

  const Measurement recoveries = measure_recoveries(configuration, root / "plans");
  report("(e) durable restart and recovery", recoveries, configuration.durable_plans,
         "plans-in-store");

  if (ephemeral.completed == 0 || durable.completed == 0 || enumerations.completed == 0 ||
      completions.completed == 0 || recoveries.completed == 0) {
    std::cout << "benchmark incomplete: at least one phase completed no operation\n";
    return 1;
  }

  std::error_code ignored;
  const std::uintmax_t removed = std::filesystem::remove_all(root, ignored);
  std::cout << "store removed " << (ignored ? "no" : "yes") << " entries " << removed << '\n';
  return ignored ? 1 : 0;
}
