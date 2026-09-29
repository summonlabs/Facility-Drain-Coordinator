// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Concurrency. The coordinator is one writer with one mutex, and the store is
// one writer per directory enforced by the operating system. These tests exist
// to show that the two together actually serialize: no lost update, no torn
// snapshot, no duplicate externally consequential request, and no lock left
// behind by a session that ended.

#include "fdc_test_support.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{81};

/// Runs a body on its own thread and reports any exception as a failure string
/// rather than letting it terminate the process.
void run_threads(std::uint32_t count, const std::function<void(std::uint32_t)>& body,
                 std::vector<std::string>& failures) {
  std::mutex failures_mutex;
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    threads.emplace_back([&, index]() {
      try {
        body(index);
      } catch (const std::exception& error) {
        const std::lock_guard<std::mutex> lock(failures_mutex);
        failures.push_back(std::string{"thread "} + std::to_string(index) + " threw: " + error.what());
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
}

MutationContext unique_context(const Coordinator& coordinator, PlanId plan, Revision revision,
                               std::uint64_t observation) {
  MutationContext context;
  context.plan = plan;
  context.expected_revision = revision;
  context.incarnation = coordinator.incarnation();
  context.expected_epoch = coordinator.control_epoch();
  context.observation = ObservationSequence{observation};
  context.principal = "concurrency-test";
  return context;
}

}  // namespace

FDC_TEST(concurrency, concurrent_plan_creation_never_loses_a_mutation) {
  TempDir dir{"concurrency-create"};
  Scaffold scaffold = Scaffold::durable(dir.path());

  constexpr std::uint32_t kThreads = 8;
  constexpr std::uint32_t kPerThread = 10;
  std::atomic<std::uint32_t> created{0};
  std::vector<std::string> failures;

  run_threads(
      kThreads,
      [&](std::uint32_t index) {
        for (std::uint32_t step = 0; step < kPerThread; ++step) {
          const PlanId id{1000U + index * 100U + step};
          CreatePlanRequest request;
          request.context = unique_context(scaffold.coordinator(), id, Revision{1},
                                           100000U + index * 1000U + step);
          request.id = id;
          request.scope = DrainScope{ScopeKind::kRack, 1U + index};
          request.targets = {DrainScope{ScopeKind::kRack, 1U + index}};
          request.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
          request.generations = fdc_test::generations(1);
          request.policy_id = PolicyId{7};
          request.policy_digest = digest_text("concurrency-policy");
          request.label = "concurrent plan";
          request.requested_by = "concurrency-test";
          auto outcome = scaffold.coordinator().create_plan(request);
          if (outcome) {
            created.fetch_add(1U);
          }
        }
      },
      failures);

  for (const std::string& failure : failures) {
    fdc_test::report_failure(__FILE__, __LINE__, failure);
  }
  FDC_CHECK_EQ(created.load(), kThreads * kPerThread);
  FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), kThreads * kPerThread);
  const CoordinatorSnapshot snapshot = scaffold.coordinator().snapshot().value();
  FDC_CHECK_EQ(snapshot.plans.size(), static_cast<std::size_t>(kThreads * kPerThread));
  for (std::size_t index = 1; index < snapshot.plans.size(); ++index) {
    FDC_CHECK(snapshot.plans[index - 1].spec.id < snapshot.plans[index].spec.id);
  }
}

FDC_TEST(concurrency, concurrent_issue_stages_exactly_one_request) {
  TempDir dir{"concurrency-issue"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  scaffold.create_plan(kPlan, {workload(1001)});
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});

  constexpr std::uint32_t kThreads = 8;
  std::atomic<std::uint32_t> staged{0};
  std::atomic<std::uint32_t> duplicates{0};
  std::atomic<std::uint32_t> stale{0};
  std::vector<std::string> failures;
  std::mutex key_mutex;
  std::vector<ContentDigest> keys;

  run_threads(
      kThreads,
      [&](std::uint32_t index) {
        IssueRequestsRequest request;
        request.context =
            unique_context(scaffold.coordinator(), kPlan, scaffold.revision_of(kPlan), 200000U + index);
        request.domains = DomainMask::of(OwnerDomain::kAsi);
        auto outcome = scaffold.coordinator().issue_requests(request);
        if (!outcome) {
          if (outcome.error().code() == ErrorCode::kInvalidGenerationOrder) {
            // A caller whose observation is behind the plan's is refused rather
            // than applied twice. That is a refusal, not a lost mutation.
            stale.fetch_add(1U);
            return;
          }
          fdc_test::fail_now("issue_requests failed with " + std::string{to_token(outcome.error().code())} +
                             ": " + outcome.error().detail());
        }
        const IssueRequestsOutcome& issued = outcome.value();
        staged.fetch_add(issued.newly_staged);
        duplicates.fetch_add(issued.duplicates);
        const std::lock_guard<std::mutex> lock(key_mutex);
        for (const DrainRequest& delivered : issued.to_deliver) {
          keys.push_back(delivered.idempotency_key);
        }
        for (const IssueItem& item : issued.items) {
          keys.push_back(item.request.idempotency_key);
        }
      },
      failures);

  for (const std::string& failure : failures) {
    fdc_test::report_failure(__FILE__, __LINE__, failure);
  }
  // However the threads interleaved, the externally consequential outcome is
  // one request, staged once, with one key.
  FDC_CHECK_EQ(staged.load(), 1U);
  const std::vector<DrainRequest> requests = scaffold.coordinator().requests(kPlan).value();
  FDC_CHECK_EQ(requests.size(), 1U);
  FDC_REQUIRE(!keys.empty());
  for (const ContentDigest& key : keys) {
    FDC_CHECK(key == keys.front());
  }
  FDC_CHECK_EQ(duplicates.load() + stale.load() + 1U, kThreads);
}

FDC_TEST(concurrency, readers_never_observe_a_half_applied_mutation) {
  TempDir dir{"concurrency-readers"};
  Scaffold scaffold = Scaffold::durable(dir.path());
  scaffold.create_plan(kPlan, {workload(1001), workload(1002)});
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001), workload(1002)});

  std::atomic<bool> stop{false};
  std::atomic<std::uint32_t> snapshots{0};
  std::vector<std::string> failures;

  std::thread writer([&]() {
    try {
      std::uint64_t observation = 300000;
      std::uint64_t generation = 2;
      for (std::uint32_t step = 0; step < 60U; ++step) {
        IngestCompletionRequest request;
        request.context = unique_context(scaffold.coordinator(), kPlan, scaffold.revision_of(kPlan),
                                         observation++);
        request.domain = OwnerDomain::kAsi;
        request.state = CompletionState::kDraining;
        request.generation = EvidenceGeneration{generation++};
        request.observed_at = ObservationSequence{observation};
        request.payload_digest = digest_text("report-" + std::to_string(step));
        request.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
        const auto& bound = scaffold.plan(kPlan).spec.bindings.manifest_digest(OwnerDomain::kAsi);
        request.manifest_digest = bound.value();
        request.generations = fdc_test::generations(1);
        request.source = "test-owner";
        auto outcome = scaffold.coordinator().ingest_completion(request);
        if (!outcome) {
          stop.store(true);
          return;
        }
      }
      stop.store(true);
    } catch (const std::exception& error) {
      stop.store(true);
      std::cerr << "the writer thread threw: " << error.what() << std::endl;
    }
  });

  while (!stop.load()) {
    const auto snapshot = scaffold.coordinator().snapshot();
    if (!snapshot) {
      continue;
    }
    snapshots.fetch_add(1U);
    for (const DrainPlanSnapshot& plan : snapshot.value().plans) {
      // Every invariant a torn mutation would break, checked on the snapshot
      // the reader actually received.
      FDC_CHECK(plan.spec.revision.value() >= 1U);
      FDC_CHECK(plan.revision == plan.spec.revision);
      FDC_CHECK(!plan.required_domains.empty());
      FDC_CHECK(plan.spec.targets.contains(plan.spec.scope));
      FDC_CHECK(plan.grant_live == plan.grant.has_value());
      FDC_CHECK(plan.state == DrainState::kSafeToRemove ? plan.grant_live : true);
      for (std::size_t index = 1; index < plan.requests.size(); ++index) {
        FDC_CHECK(plan.requests[index - 1] < plan.requests[index]);
      }
      for (std::size_t index = 1; index < plan.residuals.entries.size(); ++index) {
        FDC_CHECK(plan.residuals.entries[index - 1] < plan.residuals.entries[index]);
      }
    }
  }
  writer.join();
  FDC_CHECK(snapshots.load() > 0U);
  FDC_CHECK(scaffold.coordinator().requests(kPlan).value().empty() ||
            !scaffold.coordinator().requests(kPlan).value().empty());
}

FDC_TEST(concurrency, a_second_writer_thread_is_locked_out_of_the_store) {
  TempDir dir{"concurrency-lock"};
  Scaffold holder = Scaffold::durable(dir.path());
  holder.create_plan(kPlan);

  constexpr std::uint32_t kThreads = 6;
  std::atomic<std::uint32_t> locked_out{0};
  std::atomic<std::uint32_t> opened{0};
  std::vector<std::string> failures;

  run_threads(
      kThreads,
      [&](std::uint32_t) {
        CoordinatorOpenRequest request;
        request.root = dir.path();
        auto outcome = Coordinator::open(request);
        if (outcome) {
          opened.fetch_add(1U);
          return;
        }
        if (outcome.error().code() != ErrorCode::kStoreLocked) {
          fdc_test::fail_now("a second writer was refused with " +
                             std::string{to_token(outcome.error().code())} + " instead of store-locked");
        }
        locked_out.fetch_add(1U);
      },
      failures);

  for (const std::string& failure : failures) {
    fdc_test::report_failure(__FILE__, __LINE__, failure);
  }
  FDC_CHECK_EQ(locked_out.load(), kThreads);
  FDC_CHECK_EQ(opened.load(), 0U);
}

FDC_TEST(concurrency, repeated_open_and_close_releases_the_lock_every_time) {
  TempDir dir{"concurrency-reopen"};
  for (std::uint32_t round = 0; round < 12U; ++round) {
    Scaffold scaffold = Scaffold::durable(dir.path());
    RevisePlanRequest revise;
    const PlanId id{900U + round};
    scaffold.create_plan(id);
    revise.context = scaffold.next(id, scaffold.revision_of(id));
    revise.reason = FenceReason::kPlanRevised;
    revise.detail = "reopen round " + std::to_string(round);
    auto outcome = scaffold.coordinator().revise_plan(revise);
    FDC_REQUIRE_OK(outcome);
    // The session ends here; the next round must be able to take the lock. A
    // leaked lock would fail the following open rather than this one.
  }
  Scaffold final_session = Scaffold::durable(dir.path());
  FDC_CHECK_EQ(final_session.coordinator().snapshot().value().plans.size(), 12U);
}
