// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Multiprocess claims, proven with real independent operating system processes.
// Every case here starts a second copy of this same test executable: nothing is
// simulated with threads, because a thread cannot demonstrate that an operating
// system releases a lock when a process dies, and a thread cannot die in a way
// that leaves a store half published.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace facilitydrain;
using fdc_test::ChildProcess;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{61};

/// Writes a small rendezvous file. A child that has reached a point writes it,
/// and the parent waits for it, so the two processes agree on a moment without
/// either one sleeping blindly.
void touch(const std::filesystem::path& path) {
  std::ofstream file{path, std::ios::binary | std::ios::trunc};
  file << "ready";
}

/// Blocks until the stop file appears or the bound expires. The bound exists so
/// that a bug in the parent becomes a failed test rather than a stuck suite:
/// the child always ends on its own.
void hold_until_stopped(const std::filesystem::path& stop_path, std::uint32_t bound_milliseconds = 60000) {
  std::uint32_t waited = 0;
  while (waited < bound_milliseconds) {
    if (std::filesystem::exists(stop_path)) {
      return;
    }
    fdc_test::sleep_milliseconds(50);
    waited += 50;
  }
}

int hold_lock_mode(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 64;
  }
  const std::filesystem::path root = args[0];
  const std::filesystem::path ready = args[1];
  const std::filesystem::path stop = args[2];

  CoordinatorOpenRequest request;
  request.root = root;
  request.writer_label = "child-writer";
  auto coordinator = Coordinator::open(request);
  if (!coordinator) {
    std::cerr << "hold-lock: open failed for root=[" << root.string() << "] with "
              << coordinator.error().to_text() << std::endl;
    return 10;
  }
  touch(ready);
  hold_until_stopped(stop);
  return 0;
}

int stage_then_wait_mode(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 64;
  }
  const std::filesystem::path root = args[0];
  const std::filesystem::path ready = args[1];
  const std::filesystem::path stop = args[2];

  Scaffold scaffold = Scaffold::durable(root);
  scaffold.create_plan(kPlan, {workload(1001)});
  scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
  const IssueRequestsOutcome issued = scaffold.issue(kPlan, DomainMask::of(OwnerDomain::kAsi));
  if (issued.to_deliver.size() != 1U) {
    return 11;
  }
  // The request is durable and staged. The delivery has not happened, and this
  // process is about to die without ever having confirmed it.
  touch(ready);
  hold_until_stopped(stop);
  return 0;
}

int publish_loop_mode(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 64;
  }
  const std::filesystem::path root = args[0];
  const std::filesystem::path ready = args[1];
  const std::filesystem::path stop = args[2];

  Scaffold scaffold = Scaffold::durable(root);
  scaffold.create_plan(kPlan);
  // The rendezvous file lives outside the store: a store directory holds the
  // store and nothing else, and it refuses to adopt a directory that does not
  // look like one.
  touch(ready);
  std::uint32_t revision = 1;
  while (!std::filesystem::exists(stop)) {
    RevisePlanRequest revise;
    revise.context = scaffold.next(kPlan, Revision{revision});
    revise.reason = FenceReason::kPlanRevised;
    revise.detail = "publish loop";
    auto outcome = scaffold.coordinator().revise_plan(revise);
    if (!outcome) {
      return 12;
    }
    revision += 1U;
  }
  return 0;
}

int crash_at_mode(const std::vector<std::string>& args) {
  if (args.size() < 3) {
    return 64;
  }
  const std::filesystem::path root = args[0];
  const std::string point = args[1];
  const std::filesystem::path ready = args[2];

  PublishFaultHooks faults;
  if (point == "before-stage") {
    faults.crash_at = FaultPoint::kBeforeStageWrite;
  } else if (point == "after-stage") {
    faults.crash_at = FaultPoint::kAfterStageWriteBeforeSync;
  } else if (point == "after-sync") {
    faults.crash_at = FaultPoint::kAfterSyncBeforePublish;
  } else if (point == "after-publish") {
    faults.crash_at = FaultPoint::kAfterPublishBeforePointer;
  } else if (point == "after-pointer") {
    faults.crash_at = FaultPoint::kAfterPointerFlush;
  } else {
    return 64;
  }

  touch(ready);
  Scaffold scaffold = Scaffold::durable(root, Limits{}, faults);
  scaffold.create_plan(kPlan);
  // Reaching here means the crash hook did not fire, which is a failure of the
  // hook rather than of the store.
  return 13;
}

/// Registers every child mode of this file before main runs.
struct ChildModes {
  ChildModes() {
    fdc_test::register_child_mode("hold-lock", hold_lock_mode);
    fdc_test::register_child_mode("stage-then-wait", stage_then_wait_mode);
    fdc_test::register_child_mode("publish-loop", publish_loop_mode);
    fdc_test::register_child_mode("crash-at", crash_at_mode);
  }
};

const ChildModes kChildModes;

/// Waits, with a bound, until the store can be opened again. A terminated
/// process releases its file lock, but the release is not synchronised with the
/// terminator returning, so the parent re-tries instead of assuming.
[[nodiscard]] Result<Coordinator> open_until_available(const std::filesystem::path& root,
                                                       std::string& last_detail) {
  for (std::uint32_t attempt = 0; attempt < 200U; ++attempt) {
    CoordinatorOpenRequest request;
    request.root = root;
    auto outcome = Coordinator::open(request);
    if (outcome) {
      return outcome;
    }
    if (outcome.error().code() != ErrorCode::kStoreLocked) {
      last_detail = outcome.error().to_text();
      return outcome;
    }
    last_detail = outcome.error().to_text();
    fdc_test::sleep_milliseconds(25);
  }
  return make_error<Coordinator>(ErrorCode::kStoreLocked, last_detail);
}

}  // namespace

FDC_TEST(multiprocess, a_live_writer_excludes_another_process_and_dying_releases_it) {
  TempDir dir{"multi-lock"};
  const std::filesystem::path store = dir.sub("store");
  const std::filesystem::path ready = dir.sub("ready");
  const std::filesystem::path stop = dir.sub("stop");
  const std::filesystem::path output = dir.sub("child-output.txt");

  ChildProcess child = ChildProcess::start("hold-lock", {store.string(), ready.string(), stop.string()}, output);
  FDC_REQUIRE(child.valid());
  FDC_REQUIRE(fdc_test::wait_for_file(ready, child));

  // A second writer in this process is refused, and the diagnostic names the
  // holder so an operator knows who to talk to.
  CoordinatorOpenRequest request;
  request.root = store;
  auto blocked = Coordinator::open(request);
  FDC_REQUIRE(!blocked);
  FDC_CHECK(blocked.error().code() == ErrorCode::kStoreLocked);
  FDC_CHECK(blocked.error().detail().find("child-writer") != std::string::npos);

  // Killing the holder must release the lock without any cleanup of its own:
  // that is the entire reason the lock is held by the operating system.
  child.terminate();
  std::string detail;
  auto recovered = open_until_available(store, detail);
  if (!recovered) {
    fdc_test::fail_now("the store stayed locked after the holder was killed: " + detail);
  }
  FDC_CHECK(recovered.value().commit_sequence().value() == 0U);
}

FDC_TEST(multiprocess, a_process_that_dies_before_confirming_delivery_replays_the_same_key) {
  TempDir dir{"multi-replay"};
  const std::filesystem::path store = dir.sub("store");
  const std::filesystem::path ready = dir.sub("ready");
  const std::filesystem::path stop = dir.sub("stop");
  const std::filesystem::path output = dir.sub("child-output.txt");

  ChildProcess child =
      ChildProcess::start("stage-then-wait", {store.string(), ready.string(), stop.string()}, output);
  FDC_REQUIRE(child.valid());
  FDC_REQUIRE(fdc_test::wait_for_file(ready, child));
  child.terminate();

  std::string detail;
  auto reopened = open_until_available(store, detail);
  if (!reopened) {
    fdc_test::fail_now("the store did not reopen after the child died: " + detail);
  }
  Coordinator coordinator = std::move(reopened).value();
  const std::vector<DrainRequest> before = coordinator.requests(kPlan).value();
  FDC_REQUIRE(before.size() == 1U);
  FDC_CHECK(before.front().state == RequestState::kStaged);
  const ContentDigest key = before.front().idempotency_key;

  // The parent now does what a restarted adapter would do. It must re-offer the
  // same request, with the same idempotency key, so the owning system can see
  // that this is the same drain and not a new one.
  IssueRequestsRequest issue;
  issue.context.plan = kPlan;
  issue.context.expected_revision = coordinator.plan(kPlan).value().spec.revision;
  issue.context.incarnation = coordinator.incarnation();
  issue.context.expected_epoch = coordinator.control_epoch();
  issue.context.observation = ObservationSequence{before.front().staged_at.value() + 1U};
  issue.context.principal = "fdc-tests";
  issue.domains = DomainMask::of(OwnerDomain::kAsi);
  auto issued = coordinator.issue_requests(issue);
  FDC_REQUIRE_OK(issued);
  FDC_CHECK(issued.value().to_deliver.size() == 1U);
  FDC_CHECK(issued.value().to_deliver.front().idempotency_key == key);
  FDC_CHECK(coordinator.requests(kPlan).value().size() == 1U);
}

FDC_TEST(multiprocess, killing_a_writer_mid_publish_never_yields_a_hybrid_store) {
  TempDir dir{"multi-kill"};
  const std::filesystem::path store = dir.sub("store");
  const std::filesystem::path stop = dir.sub("stop");
  const std::filesystem::path ready = dir.sub("loop-ready");
  const std::filesystem::path output = dir.sub("child-output.txt");

  ChildProcess child =
      ChildProcess::start("publish-loop", {store.string(), ready.string(), stop.string()}, output);
  FDC_REQUIRE(child.valid());
  FDC_REQUIRE(fdc_test::wait_for_file(ready, child, 30000));

  // Let the child publish for a while, then kill it at an arbitrary instant.
  fdc_test::sleep_milliseconds(400);
  child.terminate();
  touch(stop);

  std::string detail;
  auto reopened = open_until_available(store, detail);
  if (!reopened) {
    // Being killed inside the publish window is allowed to make the store
    // refuse to guess, and only that.
    FDC_CHECK(detail.find("store-recovery-failed") != std::string::npos);
    // The documented operator action is to remove the unpublished generation.
    std::error_code error;
    const auto inspection = inspect_store(store, Limits{});
    FDC_REQUIRE_OK(inspection);
    for (const auto& entry : std::filesystem::directory_iterator{store, error}) {
      const std::string name = entry.path().filename().string();
      if (name.rfind(std::string{kGenerationFilePrefix}, 0) != 0) {
        continue;
      }
      const auto sequence = std::stoull(name.substr(kGenerationFilePrefix.size(), 20));
      if (sequence > inspection.value().sequence.value()) {
        std::filesystem::remove(entry.path(), error);
      }
    }
    auto recovered = open_until_available(store, detail);
    if (!recovered) {
      fdc_test::fail_now("the store did not recover after the unpublished generation was removed: " + detail);
    }
    // Whatever generation was adopted, it is complete and self consistent.
    const CoordinatorSnapshot snapshot = recovered.value().snapshot().value();
    FDC_CHECK(snapshot.state_digest.to_hex().size() == 64U);
    return;
  }

  // The kill landed outside the publish window, so the store recovered on its
  // own. Either way the recovered state is one whole generation.
  const CoordinatorSnapshot snapshot = reopened.value().snapshot().value();
  FDC_REQUIRE(snapshot.plans.size() == 1U);
  const DrainPlanSnapshot& plan = snapshot.plans.front();
  FDC_CHECK(plan.spec.id == kPlan);
  FDC_CHECK(plan.spec.revision.value() >= 1U);
  FDC_CHECK(plan.spec.targets.contains(DrainScope{ScopeKind::kRack, 9}));
  FDC_CHECK(!plan.spec.bindings.generations.to_canonical().empty());
}

FDC_TEST(multiprocess, a_process_killed_at_a_commit_point_leaves_one_whole_generation) {
  struct Case {
    const char* point;
    bool expect_recovery_refusal;
  };
  const Case cases[] = {{"before-stage", false},
                        {"after-stage", false},
                        {"after-sync", false},
                        {"after-publish", true},
                        {"after-pointer", false}};

  for (const Case& test_case : cases) {
    TempDir dir{std::string{"multi-crash-"} + test_case.point};
    const std::filesystem::path store = dir.sub("store");
    const std::filesystem::path ready = dir.sub("ready");
    const std::filesystem::path output = dir.sub("child-output.txt");
    ChildProcess child =
        ChildProcess::start("crash-at", {store.string(), test_case.point, ready.string()}, output);
    FDC_REQUIRE(child.valid());
    FDC_REQUIRE(fdc_test::wait_for_file(ready, child));

    int exit_code = 0;
    std::string text;
    FDC_REQUIRE(child.wait(exit_code, text));
    FDC_CHECK(exit_code != 0);

    std::string detail;
    auto reopened = open_until_available(store, detail);
    if (test_case.expect_recovery_refusal) {
      if (reopened) {
        fdc_test::fail_now(std::string{"a kill after publishing but before the pointer was accepted as "
                                       "recoverable for point "} +
                           test_case.point);
      }
      FDC_CHECK(detail.find("store-recovery-failed") != std::string::npos);
      continue;
    }
    if (!reopened) {
      // Before the first publish there is nothing at all to recover, which is a
      // different and equally honest outcome from a half published generation.
      FDC_CHECK(detail.find("store-not-found") != std::string::npos ||
                detail.find("store-corrupt") != std::string::npos ||
                detail.find("store-recovery-failed") != std::string::npos);
      continue;
    }
    // If the mutation was published it is whole; if it was not, the store is
    // empty. A hybrid is what this test exists to rule out.
    const CoordinatorSnapshot snapshot = reopened.value().snapshot().value();
    FDC_CHECK(snapshot.plans.size() <= 1U);
    if (snapshot.plans.size() == 1U) {
      FDC_CHECK(snapshot.plans.front().spec.id == kPlan);
      FDC_CHECK(snapshot.plans.front().spec.revision.value() == 1U);
    }
  }
}
