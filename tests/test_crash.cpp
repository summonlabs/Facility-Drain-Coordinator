// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Crash consistency, in process: a publish that fails at any point of its
// sequence either becomes the next whole generation or leaves the previous one
// exactly as it was. The companion test in test_multiprocess.cpp kills a real
// process at the same points; this one proves the error paths that a killed
// process never gets to run.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::TempDir;

namespace {

constexpr PlanId kPlan{101};

struct PointCase {
  FaultPoint point;
  const char* name;
  /// True when the generation may already have been written but the pointer
  /// definitely was not, which makes the outcome unknowable from the bytes.
  bool ambiguous_outcome;
};

const PointCase kPoints[] = {
    {FaultPoint::kBeforeStageWrite, "before-stage", false},
    {FaultPoint::kAfterStageWriteBeforeSync, "after-stage", false},
    {FaultPoint::kAfterSyncBeforePublish, "after-sync", false},
    {FaultPoint::kAfterPublishBeforePointer, "after-publish", true},
    {FaultPoint::kAfterPointerBeforeFlush, "after-pointer", true},
    {FaultPoint::kAfterPointerFlush, "after-pointer-flush", true},
};

[[nodiscard]] std::filesystem::path seeded(const TempDir& dir) {
  const std::filesystem::path root = dir.sub("store");
  Scaffold seed = Scaffold::durable(root);
  seed.create_plan(kPlan);
  if (seed.coordinator().commit_sequence().value() != 1U) {
    fdc_test::fail_now("the seed session did not commit exactly one generation");
  }
  return root;
}

}  // namespace

FDC_TEST(crash, every_fault_point_either_commits_whole_or_changes_nothing) {
  for (const PointCase& test_case : kPoints) {
    TempDir dir{std::string{"crash-"} + test_case.name};
    const std::filesystem::path root = seeded(dir);

    {
      PublishFaultHooks faults;
      faults.fail_at = test_case.point;
      Scaffold scaffold = Scaffold::durable(root, Limits{}, faults);

      RevisePlanRequest revise;
      revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
      revise.reason = FenceReason::kPlanRevised;
      revise.detail = "should not survive";
      auto outcome = scaffold.coordinator().revise_plan(revise);
      FDC_CHECK(!outcome);
      FDC_CHECK(outcome.error().code() == ErrorCode::kCommitFailed);
      // A mutation that did not publish did not happen, in memory either.
      FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 1U);
      FDC_CHECK_EQ(scaffold.coordinator().plan(kPlan).value().spec.revision.value(), 1U);
    }

    CoordinatorOpenRequest request;
    request.root = root;
    auto reopened = Coordinator::open(request);
    if (!reopened) {
      // Only the window between publishing the generation and replacing the
      // pointer may make the outcome unknowable, and only that window is
      // allowed to refuse the store.
      FDC_CHECK(test_case.ambiguous_outcome);
      FDC_CHECK(reopened.error().code() == ErrorCode::kStoreRecoveryFailed);
      continue;
    }

    const CoordinatorSnapshot snapshot = reopened.value().snapshot().value();
    FDC_REQUIRE(snapshot.plans.size() == 1U);
    const std::uint64_t sequence = reopened.value().commit_sequence().value();
    const std::uint64_t revision = snapshot.plans.front().spec.revision.value();
    // Exactly the pre-mutation generation or exactly the post-mutation one.
    FDC_CHECK(sequence == 1U || sequence == 2U);
    FDC_CHECK_EQ(revision, sequence);
    FDC_CHECK_EQ(snapshot.plans.front().spec.id.value(), kPlan.value());
    FDC_CHECK(snapshot.plans.front().spec.targets.contains(DrainScope{ScopeKind::kRack, 9}));
  }
}

FDC_TEST(crash, a_truncated_staging_file_is_caught_by_the_read_back_verification) {
  TempDir dir{"crash-truncated-staging"};
  const std::filesystem::path root = seeded(dir);
  {
    PublishFaultHooks faults;
    faults.truncate_staged_bytes = 24;
    Scaffold scaffold = Scaffold::durable(root, Limits{}, faults);
    RevisePlanRequest revise;
    revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
    revise.reason = FenceReason::kPlanRevised;
    revise.detail = "truncated";
    auto outcome = scaffold.coordinator().revise_plan(revise);
    FDC_CHECK(!outcome);
    FDC_CHECK(outcome.error().code() == ErrorCode::kCommitFailed);
    FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 1U);
  }

  // The staged file never became a generation, so the store is exactly as it was.
  CoordinatorOpenRequest request;
  request.root = root;
  auto reopened = Coordinator::open(request);
  FDC_REQUIRE_OK(reopened);
  FDC_CHECK_EQ(reopened.value().commit_sequence().value(), 1U);
  FDC_CHECK_EQ(reopened.value().plan(kPlan).value().spec.revision.value(), 1U);
}

FDC_TEST(crash, repeated_failures_do_not_consume_commit_sequences) {
  TempDir dir{"crash-sequences"};
  const std::filesystem::path root = seeded(dir);
  {
    PublishFaultHooks faults;
    faults.fail_at = FaultPoint::kAfterStageWriteBeforeSync;
    Scaffold scaffold = Scaffold::durable(root, Limits{}, faults);
    for (std::uint32_t attempt = 0; attempt < 3U; ++attempt) {
      RevisePlanRequest revise;
      revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
      revise.reason = FenceReason::kPlanRevised;
      revise.detail = "attempt " + std::to_string(attempt);
      auto outcome = scaffold.coordinator().revise_plan(revise);
      FDC_CHECK(!outcome);
      FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 1U);
    }
  }

  Scaffold healthy = Scaffold::durable(root);
  FDC_CHECK_EQ(healthy.coordinator().commit_sequence().value(), 1U);
  RevisePlanRequest revise;
  revise.context = healthy.next(kPlan, healthy.revision_of(kPlan));
  revise.reason = FenceReason::kPlanRevised;
  revise.detail = "the first publish that succeeds";
  auto outcome = healthy.coordinator().revise_plan(revise);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK_EQ(healthy.coordinator().commit_sequence().value(), 2U);
}

FDC_TEST(crash, a_failed_publish_leaves_no_new_generation_behind) {
  TempDir dir{"crash-no-generation"};
  const std::filesystem::path root = seeded(dir);
  const std::size_t before = fdc_test::count_files_with_prefix(root, kGenerationFilePrefix);
  {
    PublishFaultHooks faults;
    faults.fail_at = FaultPoint::kBeforeStageWrite;
    Scaffold scaffold = Scaffold::durable(root, Limits{}, faults);
    RevisePlanRequest revise;
    revise.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
    revise.reason = FenceReason::kPlanRevised;
    revise.detail = "refused before staging";
    auto outcome = scaffold.coordinator().revise_plan(revise);
    FDC_CHECK(!outcome);
  }
  FDC_CHECK_EQ(fdc_test::count_files_with_prefix(root, kGenerationFilePrefix), before);
  const auto inspection = inspect_store(root, Limits{});
  FDC_REQUIRE_OK(inspection);
  FDC_CHECK_EQ(inspection.value().sequence.value(), 1U);
}
