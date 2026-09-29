// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Recovery: what a restart is allowed to conclude. Persisted facts come back;
// persisted conclusions do not. A crash that leaves a publish half finished is
// reported as unknowable rather than guessed at, because guessing there is the
// difference between one drain request and two.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{41};

DomainMask asi_and_dfi() {
  return DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
}

}  // namespace

FDC_TEST(recovery, a_restart_does_not_inherit_removal_authority) {
  TempDir dir{"recovery-authority"};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan);
    scaffold.prove_empty_scope(kPlan, asi_and_dfi());
    scaffold.grant(kPlan);
    FDC_CHECK(scaffold.plan(kPlan).grant_live);
    FDC_CHECK(scaffold.plan(kPlan).state == DrainState::kSafeToRemove);
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  const DrainPlanSnapshot plan = reopened.plan(kPlan);
  // The recorded grant is history. It names the epoch it was granted under, and
  // that epoch is over, so the answer must be asked again rather than inherited.
  FDC_REQUIRE(plan.grant.has_value());
  FDC_CHECK(!plan.grant_live);
  FDC_CHECK(plan.grant->epoch.value() == 1U);
  FDC_CHECK_EQ(reopened.coordinator().control_epoch().value(), 2U);
  FDC_CHECK(plan.state == DrainState::kDrained);
  FDC_CHECK_EQ(reopened.coordinator().recovery().grants_fenced, 1U);
  FDC_CHECK_EQ(reopened.coordinator().recovery().plans_reopened, 1U);

  const SafeToRemoveEvaluation evaluation = reopened.evaluate(kPlan);
  FDC_CHECK(evaluation.verdict == SafeToRemoveVerdict::kGranted);
  FDC_CHECK(!reopened.plan(kPlan).grant_live);

  // Re-granting is an explicit act, and it re-evaluates under the new epoch.
  const SafeToRemoveGrant regranted = reopened.grant(kPlan);
  FDC_CHECK_EQ(regranted.epoch.value(), 2U);
  FDC_CHECK(reopened.plan(kPlan).grant_live);
  FDC_CHECK(reopened.plan(kPlan).state == DrainState::kSafeToRemove);
}

FDC_TEST(recovery, persisted_facts_and_requests_come_back_unchanged) {
  TempDir dir{"recovery-facts"};
  DrainRequestId staged{};
  ContentDigest key{};
  std::uint64_t observation_before = 0;
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan, {workload(1001)});
    scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
    const IssueRequestsOutcome issued = scaffold.issue(kPlan, DomainMask::of(OwnerDomain::kAsi));
    FDC_REQUIRE(issued.to_deliver.size() == 1U);
    staged = issued.to_deliver.front().id;
    key = issued.to_deliver.front().idempotency_key;
    observation_before = scaffold.plan(kPlan).last_observation.value();
  }

  Scaffold reopened = Scaffold::durable(dir.path());
  const std::vector<DrainRequest> requests =
      reopened.coordinator().requests(kPlan).value();
  FDC_REQUIRE(requests.size() == 1U);
  FDC_CHECK(requests.front().id == staged);
  FDC_CHECK(requests.front().idempotency_key == key);
  FDC_CHECK(requests.front().state == RequestState::kStaged);
  FDC_CHECK_EQ(reopened.plan(kPlan).last_observation.value(), observation_before);
  FDC_CHECK(reopened.plan(kPlan).state == DrainState::kRequested);
}

FDC_TEST(recovery, a_second_writer_is_locked_out) {
  TempDir dir{"recovery-lock"};
  Scaffold first = Scaffold::durable(dir.path());
  first.create_plan(kPlan);

  CoordinatorOpenRequest request;
  request.root = dir.path();
  auto second = Coordinator::open(request);
  FDC_REQUIRE(!second);
  FDC_CHECK(second.error().code() == ErrorCode::kStoreLocked);

  // The diagnostic names the holder, so an operator does not have to guess.
  FDC_CHECK(second.error().detail().find("fdc-tests") != std::string::npos);

  // A read only session takes no lock, so inspection is always possible.
  CoordinatorOpenRequest reader;
  reader.root = dir.path();
  reader.read_only = true;
  reader.create_if_missing = false;
  auto inspection = Coordinator::open(reader);
  FDC_REQUIRE_OK(inspection);
}

FDC_TEST(recovery, a_publish_that_never_reached_the_pointer_is_not_guessed_at) {
  TempDir dir{"recovery-unpublished"};
  {
    PublishFaultHooks faults;
    faults.fail_at = FaultPoint::kAfterPublishBeforePointer;
    Scaffold scaffold = Scaffold::durable(dir.path(), Limits{}, faults);
    CreatePlanRequest create;
    create.context = scaffold.next(kPlan, Revision{1});
    create.id = kPlan;
    create.scope = DrainScope{ScopeKind::kRack, 9};
    create.targets = {DrainScope{ScopeKind::kRack, 9}};
    create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi);
    create.generations = fdc_test::generations(1);
    create.policy_id = PolicyId{7};
    create.policy_digest = digest_text("policy");
    auto outcome = scaffold.coordinator().create_plan(create);
    FDC_REQUIRE(!outcome);
    FDC_CHECK(outcome.error().code() == ErrorCode::kCommitFailed);
    // The failed mutation had no effect at all, not even in memory.
    FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 0U);
  }

  // A generation file now exists that CURRENT does not name. Whether that
  // publish happened is not knowable from the bytes, so the store refuses to
  // pretend: an operator decides, not the reader.
  CoordinatorOpenRequest request;
  request.root = dir.path();
  auto refused = Coordinator::open(request);
  FDC_REQUIRE(!refused);
  FDC_CHECK(refused.error().code() == ErrorCode::kStoreRecoveryFailed);
  FDC_CHECK(refused.error().detail().find("gen-") != std::string::npos);

  // Removing the unpublished generation is the documented operator action, and
  // afterwards the previous generation is intact.
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator{dir.path(), error}) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(std::string{kGenerationFilePrefix}, 0) == 0) {
      std::filesystem::remove(entry.path(), error);
    }
  }
  auto recovered = Coordinator::open(request);
  FDC_REQUIRE_OK(recovered);
  // With nothing published at all, this is a brand new store, which is the
  // honest description of a directory that never committed a generation. What
  // matters is that nothing from the unpublished generation was adopted.
  FDC_CHECK(recovered.value().recovery().created_new_store);
  FDC_CHECK_EQ(recovered.value().commit_sequence().value(), 0U);
  FDC_CHECK(recovered.value().snapshot().value().plans.empty());
}

FDC_TEST(recovery, a_corrupt_pointer_is_reported_rather_than_ignored) {
  TempDir dir{"recovery-pointer"};
  {
    Scaffold scaffold = Scaffold::durable(dir.path());
    scaffold.create_plan(kPlan);
  }

  const std::filesystem::path pointer = dir.path() / std::string{kPointerFileName};
  {
    std::FILE* file = nullptr;
    const auto open_status = fopen_s(&file, pointer.string().c_str(), "r+b");
    FDC_REQUIRE(open_status == 0);
    FDC_REQUIRE(file != nullptr);
    unsigned char byte = 0;
    FDC_REQUIRE(std::fread(&byte, 1, 1, file) == 1U);
    byte = static_cast<unsigned char>(byte ^ 0xFFU);
    FDC_REQUIRE(std::fseek(file, 0, SEEK_SET) == 0);
    FDC_REQUIRE(std::fwrite(&byte, 1, 1, file) == 1U);
    std::fclose(file);
  }

  CoordinatorOpenRequest request;
  request.root = dir.path();
  auto outcome = Coordinator::open(request);
  FDC_REQUIRE(!outcome);
  FDC_CHECK(outcome.error().code() == ErrorCode::kStoreCorrupt ||
            outcome.error().code() == ErrorCode::kStoreChecksumMismatch);
}
