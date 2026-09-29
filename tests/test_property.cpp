// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Property testing with a deterministic seed. The state machine below drives
// the real API with valid, invalid, stale and replayed intents and checks the
// documented invariants after every accepted step. Every run is reproducible
// from the seed printed with a failure, so a counterexample is a bug report.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace facilitydrain;
using fdc_test::Rng;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr std::uint64_t kSeed = 0xFDCD1234ULL;

/// One step of the machine: which operation ran, and what came back.
struct Step {
  std::string operation;
  ErrorCode code = ErrorCode::kOk;
  std::string canonical;
};

void check_plan_invariants(const DrainPlanSnapshot& plan) {
  FDC_CHECK(plan.spec.revision.value() >= 1U);
  FDC_CHECK(plan.revision == plan.spec.revision);
  FDC_CHECK(!plan.required_domains.empty());
  FDC_CHECK(plan.spec.targets.contains(plan.spec.scope));
  FDC_CHECK(plan.grant_live == plan.grant.has_value());
  if (plan.state == DrainState::kSafeToRemove) {
    FDC_CHECK(plan.grant_live);
  }
  if (plan.state == DrainState::kCancelled) {
    FDC_CHECK(plan.cancelled);
  }
  for (std::size_t index = 1; index < plan.requests.size(); ++index) {
    FDC_CHECK(plan.requests[index - 1] < plan.requests[index]);
  }
  for (std::size_t index = 1; index < plan.residuals.entries.size(); ++index) {
    FDC_CHECK(plan.residuals.entries[index - 1] < plan.residuals.entries[index]);
  }
  if (!plan.requests.empty()) {
    FDC_CHECK(plan.state != DrainState::kProposed);
  }
}

/// Drives the coordinator through a fixed number of random operations. Every
/// accepted operation is followed by an invariant check on the snapshot the
/// coordinator actually returns, and the trace is returned so two runs of the
/// same seed can be compared.
std::vector<Step> run_machine(Scaffold& scaffold, std::uint64_t seed, std::uint32_t steps,
                              std::vector<PlanId>& plans) {
  Rng rng{seed};
  std::vector<Step> trace;
  trace.reserve(steps);
  std::uint64_t generation = 1;

  for (std::uint32_t step = 0; step < steps; ++step) {
    const std::uint32_t choice = rng.below(100U);
    const PlanId target = plans.empty() ? PlanId{0} : plans[rng.below(static_cast<std::uint32_t>(plans.size()))];
    Step record;
    record.operation = std::to_string(choice);

    if (choice < 18U || target.value() == 0) {
      const PlanId id{5000U + step};
      std::vector<ConsumerRecord> consumers;
      if (rng.chance(1U, 2U)) {
        consumers.push_back(workload(1000U + step, generation));
      }
      const DomainMask required = rng.chance(1U, 2U)
                                      ? DomainMask::of(OwnerDomain::kAsi)
                                      : DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
      record.operation = "create";
      CreatePlanRequest request;
      request.context = scaffold.next(id, Revision{1});
      request.id = id;
      request.scope = DrainScope{ScopeKind::kRack, 9};
      request.targets = {DrainScope{ScopeKind::kRack, 9}};
      request.declared_required_domains = required;
      request.generations = fdc_test::generations(1);
      request.policy_id = PolicyId{7};
      request.policy_digest = digest_text("property-policy");
      request.consumers = consumers;
      request.bind_consumer_manifests = rng.chance(3U, 4U);
      auto outcome = scaffold.coordinator().create_plan(request);
      if (outcome) {
        plans.push_back(id);
      } else {
        record.code = outcome.error().code();
      }
    } else if (choice < 42U) {
      record.operation = "enumerate";
      const OwnerDomain domain = owner_domain_at(rng.below(kOwnerDomainCount));
      const auto coverage = static_cast<CoverageState>(1U + rng.below(3U));
      std::vector<ConsumerRecord> consumers;
      const std::uint32_t count = rng.below(3U);
      for (std::uint32_t index = 0; index < count; ++index) {
        consumers.push_back(workload(2000U + index, 1));
      }
      RecordEnumerationRequest request;
      request.context = scaffold.next(target, scaffold.revision_of(target));
      request.domain = domain;
      request.coverage = coverage;
      request.generation = EvidenceGeneration{generation++};
      request.observed_at = request.context.observation;
      request.scope_manifest_digest = scaffold.plan(target).spec.targets.digest();
      request.consumers = consumers;
      request.generations = fdc_test::generations(1);
      request.source = "property-owner";
      auto outcome = scaffold.coordinator().record_enumeration(request);
      if (!outcome) {
        record.code = outcome.error().code();
      }
    } else if (choice < 58U) {
      record.operation = "issue";
      IssueRequestsRequest request;
      request.context = scaffold.next(target, scaffold.revision_of(target));
      request.domains = rng.chance(1U, 2U)
                            ? DomainMask::of(OwnerDomain::kAsi)
                            : DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
      auto outcome = scaffold.coordinator().issue_requests(request);
      if (!outcome) {
        record.code = outcome.error().code();
      } else {
        for (const DrainRequest& staged : outcome.value().to_deliver) {
          ConfirmDeliveryRequest deliver;
          deliver.context = scaffold.next(target, scaffold.revision_of(target));
          deliver.request = staged.id;
          auto delivered = scaffold.coordinator().confirm_delivery(deliver);
          if (delivered && rng.chance(1U, 2U)) {
            RecordAcknowledgementRequest acknowledge;
            acknowledge.context = scaffold.next(target, scaffold.revision_of(target));
            acknowledge.request = staged.id;
            acknowledge.acknowledging_system = "property-owner";
            auto acknowledged = scaffold.coordinator().record_acknowledgement(acknowledge);
            if (!acknowledged) {
              record.code = acknowledged.error().code();
            }
          }
        }
      }
    } else if (choice < 76U) {
      record.operation = "ingest";
      const OwnerDomain domain = owner_domain_at(rng.below(kOwnerDomainCount));
      const auto state = static_cast<CompletionState>(1U + rng.below(8U));
      IngestCompletionRequest request;
      request.context = scaffold.next(target, scaffold.revision_of(target));
      request.domain = domain;
      request.state = state;
      request.generation = EvidenceGeneration{generation++};
      request.observed_at = request.context.observation;
      request.payload_digest = digest_text("property-report-" + std::to_string(step));
      const DrainPlanSnapshot plan = scaffold.plan(target);
      const auto& bound = plan.spec.bindings.manifest_digest(domain);
      request.manifest_digest = bound.has_value() ? bound.value() : consumer_manifest_digest(domain, {});
      request.scope_manifest_digest = plan.spec.targets.digest();
      request.generations = fdc_test::generations(1);
      request.residual_count_known = rng.chance(3U, 4U);
      request.residual_count = request.residual_count_known ? rng.below(3U) : 0U;
      request.source = "property-owner";
      auto outcome = scaffold.coordinator().ingest_completion(request);
      if (!outcome) {
        record.code = outcome.error().code();
      }
    } else if (choice < 86U) {
      record.operation = "evaluate";
      auto evaluation = scaffold.coordinator().evaluate_safe_to_remove(target);
      if (!evaluation) {
        record.code = evaluation.error().code();
      } else {
        record.canonical = evaluation.value().to_canonical();
        const auto again = scaffold.coordinator().evaluate_safe_to_remove(target);
        FDC_REQUIRE_OK(again);
        FDC_CHECK_EQ(again.value().to_canonical(), record.canonical);
        if (evaluation.value().verdict == SafeToRemoveVerdict::kGranted) {
          FDC_CHECK(evaluation.value().all_required_proven);
          FDC_CHECK_EQ(evaluation.value().primary_blocking_code, ErrorCode::kOk);
          FDC_CHECK(evaluation.value().blocking_codes.empty());
        } else {
          FDC_CHECK(evaluation.value().primary_blocking_code != ErrorCode::kOk);
        }
      }
    } else if (choice < 93U) {
      record.operation = "grant";
      GrantSafeToRemoveRequest request;
      request.context = scaffold.next(target, scaffold.revision_of(target));
      request.granted_by = "property-test";
      auto outcome = scaffold.coordinator().grant_safe_to_remove(request);
      if (!outcome) {
        record.code = outcome.error().code();
        FDC_CHECK(record.code != ErrorCode::kOk);
      } else {
        FDC_CHECK(scaffold.plan(target).grant_live);
      }
    } else if (choice < 97U) {
      record.operation = "revise";
      RevisePlanRequest request;
      request.context = scaffold.next(target, scaffold.revision_of(target));
      request.reason = FenceReason::kPlanRevised;
      request.detail = "property revision";
      if (rng.chance(1U, 3U)) {
        request.targets = std::vector<DrainScope>{DrainScope{ScopeKind::kRack, 9},
                                                  DrainScope{ScopeKind::kAsset, 50U + rng.below(5U)}};
      }
      auto outcome = scaffold.coordinator().revise_plan(request);
      if (!outcome) {
        record.code = outcome.error().code();
      }
    } else {
      record.operation = "residual";
      RecordResidualRequest request;
      request.context = scaffold.next(target, scaffold.revision_of(target));
      ResidualEntry entry;
      entry.obligation = ObligationId{3000U + rng.below(4U)};
      entry.domain = owner_domain_at(rng.below(kOwnerDomainCount));
      entry.kind = static_cast<ResidualKind>(1U + rng.below(10U));
      entry.detail = "property residual";
      request.entry = entry;
      auto outcome = scaffold.coordinator().record_residual(request);
      if (!outcome) {
        record.code = outcome.error().code();
      }
    }

    const CoordinatorSnapshot snapshot = scaffold.coordinator().snapshot().value();
    for (const DrainPlanSnapshot& plan : snapshot.plans) {
      check_plan_invariants(plan);
    }
    trace.push_back(std::move(record));
  }
  return trace;
}

[[nodiscard]] std::string trace_digest(const std::vector<Step>& trace) {
  std::string text;
  for (const Step& step : trace) {
    text.append(step.operation);
    text.push_back(':');
    text.append(to_token(step.code));
    text.push_back('|');
    text.append(step.canonical);
    text.push_back('\n');
  }
  return digest_text(text).to_hex();
}

}  // namespace

FDC_TEST(property, a_seeded_state_machine_stays_within_its_invariants) {
  Scaffold scaffold = Scaffold::ephemeral();
  std::vector<PlanId> plans;
  const std::vector<Step> trace = run_machine(scaffold, kSeed, 600U, plans);
  FDC_CHECK_EQ(trace.size(), 600U);
  std::uint32_t accepted = 0;
  for (const Step& step : trace) {
    if (step.code == ErrorCode::kOk) {
      ++accepted;
    }
  }
  // A run in which nothing ever succeeded would satisfy the invariants
  // vacuously, so the machine has to make real progress.
  FDC_CHECK(accepted > 100U);
  FDC_CHECK(!plans.empty());
  FDC_CHECK_EQ(scaffold.coordinator().snapshot().value().plans.size(), plans.size());
}

FDC_TEST(property, the_same_seed_reproduces_the_same_trace) {
  Scaffold first = Scaffold::ephemeral();
  std::vector<PlanId> first_plans;
  const std::vector<Step> first_trace = run_machine(first, kSeed, 300U, first_plans);

  Scaffold second = Scaffold::ephemeral();
  std::vector<PlanId> second_plans;
  const std::vector<Step> second_trace = run_machine(second, kSeed, 300U, second_plans);

  FDC_CHECK_EQ(trace_digest(first_trace), trace_digest(second_trace));
  FDC_CHECK(first_plans == second_plans);
  FDC_CHECK(first.coordinator().snapshot().value().plans.size() ==
            second.coordinator().snapshot().value().plans.size());
}

FDC_TEST(property, a_durable_state_survives_a_restart_after_any_random_prefix) {
  TempDir dir{"property-restart"};
  std::vector<std::string> digests_before;
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    std::vector<PlanId> plans;
    const std::vector<Step> trace = run_machine(scaffold, kSeed ^ 0x5A5AULL, 160U, plans);
    FDC_CHECK(!trace.empty());
    const CoordinatorSnapshot snapshot = scaffold.coordinator().snapshot().value();
    for (const DrainPlanSnapshot& plan : snapshot.plans) {
      digests_before.push_back(plan.plan_digest.to_hex());
    }
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  const CoordinatorSnapshot snapshot = reopened.coordinator().snapshot().value();
  FDC_REQUIRE(snapshot.plans.size() == digests_before.size());
  for (std::size_t index = 0; index < snapshot.plans.size(); ++index) {
    FDC_CHECK_EQ(snapshot.plans[index].plan_digest.to_hex(), digests_before[index]);
    check_plan_invariants(snapshot.plans[index]);
    // A recovered grant is never live: the epoch moved with the restart.
    FDC_CHECK(!snapshot.plans[index].grant_live);
  }
}
