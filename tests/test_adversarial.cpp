// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Adversarial input. Everything here is something a caller, an operator or a
// hostile integration could actually produce: absurd counters, text that is not
// text, duplicated identities, paths that are not stores. None of it may crash,
// hang, silently truncate, or convert an unknown into a healthy answer.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{111};
constexpr std::uint64_t kMax = (std::numeric_limits<std::uint64_t>::max)();

DomainMask asi_only() { return DomainMask::of(OwnerDomain::kAsi); }

}  // namespace

FDC_TEST(adversarial, the_largest_representable_identifiers_are_handled_without_wrapping) {
  TempDir dir{"adversarial-identifiers"};
  Scaffold scaffold = Scaffold::durable(dir.path());

  const PlanId huge_plan{kMax};
  scaffold.create_plan(huge_plan, {workload(kMax, kMax)}, {DrainScope{ScopeKind::kAsset, kMax}}, asi_only());
  scaffold.enumerate(huge_plan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(kMax, kMax)});
  const DrainPlanSnapshot plan = scaffold.plan(huge_plan);
  FDC_CHECK_EQ(plan.spec.id.value(), kMax);
  FDC_CHECK_EQ(plan.consumers.size(), 1U);
  FDC_CHECK_EQ(plan.consumers.front().obligation.value(), kMax);
  FDC_CHECK_EQ(plan.consumers.front().generation.value(), kMax);
  FDC_CHECK_EQ(plan.spec.targets.targets().front().id, kMax);

  // The observation counter at its maximum cannot be advanced, so the plan is
  // frozen. That is a refusal a caller can reason about, not a silent wrap.
  MutationContext context = scaffold.next(huge_plan, scaffold.revision_of(huge_plan));
  context.observation = ObservationSequence{kMax};
  RecordEnumerationRequest request;
  request.context = context;
  request.domain = OwnerDomain::kAsi;
  request.coverage = CoverageState::kComplete;
  request.generation = EvidenceGeneration{2};
  request.observed_at = context.observation;
  request.scope_manifest_digest = plan.spec.targets.digest();
  request.consumers = {workload(kMax, kMax)};
  request.generations = fdc_test::generations(1);
  auto outcome = scaffold.coordinator().record_enumeration(request);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK_EQ(scaffold.plan(huge_plan).last_observation.value(), kMax);

  // The counter at its maximum cannot be advanced, so a further mutation is
  // refused rather than silently reusing an observation the plan already took.
  RecordEnumerationRequest refused = request;
  refused.generation = EvidenceGeneration{3};
  auto rejected = scaffold.coordinator().record_enumeration(refused);
  FDC_CHECK_CODE(rejected, ErrorCode::kInvalidGenerationOrder);
}

FDC_TEST(adversarial, text_that_is_not_text_is_refused_at_the_boundary) {
  // The byte sequences below are written as escapes on purpose: this file holds
  // no non UTF-8 bytes itself, and the values under test are built at runtime.
  const std::vector<std::string> hostile = {
      std::string(100000U, 'a'),
      std::string{"overlong \xC0\x80 encoding"},
      std::string{"stray continuation \x80 byte"},
      std::string{"truncated \xE2\x82"},
      std::string("nul \x00 inside", 12U),
      std::string{"carriage \x0D return"},
      std::string{"escape \x1B sequence"},
      std::string{"delete \x7F character"},
      std::string{"surrogate \xED\xA0\x80"},
      std::string{"byte 0xFF is not UTF-8 \xFF"},
  };

  for (const std::string& label : hostile) {
    ConsumerRecord record = workload(1001);
    record.label = label;
    const Status status = record.validate(Limits{});
    if (status.ok()) {
      fdc_test::fail_now("a hostile label was accepted: " + std::to_string(label.size()) + " bytes");
    }
    FDC_CHECK(status.code() == ErrorCode::kInvalidText || status.code() == ErrorCode::kFieldTooLong);
  }

  // Text that is text is accepted, including outside the ASCII range, and it
  // survives a durable round trip unchanged. U+6771 U+6771 U+6771 U+6771 plus
  // U+1F170 and an accented e: four three byte sequences, one four byte
  // sequence and one two byte sequence.
  const std::string unicode_label = "\xE6\x9D\xB1\xE4\xBA\xAC\xE6\x88\xBF-\xF0\x9F\x9B\xB0 \xC3\xA9";
  const std::string unicode_source = "op\xC3\xA9rateur";

  TempDir dir{"adversarial-unicode"};
  {
    Scaffold durable = Scaffold::durable(dir.path());
    ConsumerRecord record = workload(1002);
    record.label = unicode_label;
    record.source = unicode_source;
    durable.create_plan(kPlan, {record}, {DrainScope{ScopeKind::kRack, 9}}, asi_only());
  }
  Scaffold reopened = Scaffold::durable(dir.path());
  const DrainPlanSnapshot plan = reopened.plan(kPlan);
  FDC_REQUIRE(plan.consumers.size() == 1U);
  FDC_CHECK_EQ(plan.consumers.front().label, unicode_label);
  FDC_CHECK_EQ(plan.consumers.front().source, unicode_source);
}

FDC_TEST(adversarial, duplicated_identities_are_refused_rather_than_merged) {
  Scaffold scaffold = Scaffold::ephemeral();

  CreatePlanRequest first;
  first.context = scaffold.next(kPlan, Revision{1});
  first.id = kPlan;
  first.scope = DrainScope{ScopeKind::kRack, 9};
  first.targets = {DrainScope{ScopeKind::kRack, 9}};
  first.declared_required_domains = asi_only();
  first.generations = fdc_test::generations(1);
  first.policy_id = PolicyId{7};
  first.policy_digest = digest_text("policy");
  auto created = scaffold.coordinator().create_plan(first);
  FDC_REQUIRE_OK(created);

  CreatePlanRequest again = first;
  again.context = scaffold.next(kPlan, Revision{1});
  auto duplicate = scaffold.coordinator().create_plan(again);
  FDC_CHECK_CODE(duplicate, ErrorCode::kDuplicateIdentifier);

  // Two records for the same obligation inside one manifest are a contradiction
  // about identity, not two obligations.
  CreatePlanRequest two_records;
  two_records.context = scaffold.next(PlanId{112}, Revision{1});
  two_records.id = PlanId{112};
  two_records.scope = DrainScope{ScopeKind::kRack, 9};
  two_records.targets = {DrainScope{ScopeKind::kRack, 9}};
  two_records.declared_required_domains = asi_only();
  two_records.generations = fdc_test::generations(1);
  two_records.policy_id = PolicyId{7};
  two_records.policy_digest = digest_text("policy");
  two_records.consumers = {workload(1001), workload(1001)};
  auto conflict = scaffold.coordinator().create_plan(two_records);
  FDC_CHECK_CODE(conflict, ErrorCode::kDuplicateIdentifier);

  RecordEnumerationRequest enumeration;
  enumeration.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  enumeration.domain = OwnerDomain::kAsi;
  enumeration.coverage = CoverageState::kComplete;
  enumeration.generation = EvidenceGeneration{1};
  enumeration.observed_at = enumeration.context.observation;
  enumeration.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
  enumeration.consumers = {workload(1001), workload(1001)};
  enumeration.generations = fdc_test::generations(1);
  auto enumerated = scaffold.coordinator().record_enumeration(enumeration);
  FDC_CHECK_CODE(enumerated, ErrorCode::kDuplicateIdentifier);
}

FDC_TEST(adversarial, paths_that_are_not_stores_are_refused_cleanly) {
  TempDir dir{"adversarial-paths"};

  // A regular file where a store directory belongs.
  const std::filesystem::path file_path = dir.sub("not-a-directory");
  {
    std::ofstream file{file_path, std::ios::binary};
    file << "just a file";
  }
  CoordinatorOpenRequest request;
  request.root = file_path;
  auto refused = Coordinator::open(request);
  FDC_REQUIRE(!refused);
  FDC_CHECK(is_store_error(refused.error().code()));

  // A directory holding a file that belongs to somebody else, and a store, is
  // not confused with the store itself.
  const std::filesystem::path shared = dir.sub("shared");
  {
    Scaffold scaffold = Scaffold::durable(shared);
    scaffold.create_plan(kPlan);
  }
  {
    std::ofstream bystander{shared / "unrelated.bin", std::ios::binary};
    bystander << "not mine";
  }
  request.root = shared;
  auto reopened = Coordinator::open(request);
  FDC_REQUIRE_OK(reopened);
  FDC_CHECK(reopened.value().plan(kPlan).has_value());
  FDC_CHECK(reopened.value().snapshot().has_value());

  // A trailing separator names the same directory, and that directory is
  // already held by the session above, so the lock is the refusal.
  std::filesystem::path with_separator = shared;
  with_separator += std::filesystem::path::preferred_separator;
  request.root = with_separator;
  auto trailing = Coordinator::open(request);
  FDC_CHECK(!trailing);
  FDC_CHECK(trailing.error().code() == ErrorCode::kStoreLocked);
}

FDC_TEST(adversarial, bounds_that_are_too_small_to_hold_the_state_are_enforced) {
  TempDir dir{"adversarial-bounds"};
  Limits tiny;
  tiny.max_state_bytes = 8192;
  tiny.max_document_bytes = 4096;
  tiny.max_plans = 2;
  Scaffold scaffold = Scaffold::durable(dir.path(), tiny);

  scaffold.create_plan(kPlan);
  CreatePlanRequest second;
  second.context = scaffold.next(PlanId{113}, Revision{1});
  second.id = PlanId{113};
  second.scope = DrainScope{ScopeKind::kRack, 9};
  second.targets = {DrainScope{ScopeKind::kRack, 9}};
  second.declared_required_domains = asi_only();
  second.generations = fdc_test::generations(1);
  second.policy_id = PolicyId{7};
  second.policy_digest = digest_text("policy");
  auto created = scaffold.coordinator().create_plan(second);
  FDC_REQUIRE_OK(created);

  CreatePlanRequest third = second;
  third.id = PlanId{114};
  third.context = scaffold.next(PlanId{114}, Revision{1});
  auto refused = scaffold.coordinator().create_plan(third);
  FDC_CHECK_CODE(refused, ErrorCode::kTooManyEntries);

  // A manifest large enough to exceed the store's byte bound is refused rather
  // than published as a truncated payload, and the refusal leaves the state
  // exactly as it was.
  std::vector<ConsumerRecord> many;
  for (std::uint64_t index = 0; index < 60U; ++index) {
    ConsumerRecord record = workload(2000U + index);
    record.label = std::string(200U, 'x');
    record.source = std::string(200U, 'y');
    many.push_back(record);
  }
  CreatePlanRequest heavy;
  heavy.context = scaffold.next(PlanId{115}, Revision{1});
  heavy.id = PlanId{115};
  heavy.scope = DrainScope{ScopeKind::kRack, 9};
  heavy.targets = {DrainScope{ScopeKind::kRack, 9}};
  heavy.declared_required_domains = asi_only();
  heavy.generations = fdc_test::generations(1);
  heavy.policy_id = PolicyId{7};
  heavy.policy_digest = digest_text("policy");
  heavy.consumers = many;
  auto oversized = scaffold.coordinator().create_plan(heavy);
  FDC_REQUIRE(!oversized);
  const ErrorCode code = oversized.error().code();
  if (!(code == ErrorCode::kPayloadTooLarge || code == ErrorCode::kTooManyEntries ||
        code == ErrorCode::kLimitExceeded || code == ErrorCode::kCommitFailed)) {
    fdc_test::fail_now("an oversized state was refused with " + std::string{to_token(code)} + ": " +
                       oversized.error().detail());
  }
  FDC_CHECK_EQ(scaffold.coordinator().snapshot().value().plans.size(), 2U);
}

FDC_TEST(adversarial, unknown_enumeration_values_are_never_accepted_as_a_default) {
  Scaffold scaffold = Scaffold::ephemeral();
  scaffold.create_plan(kPlan);

  RecordEnumerationRequest request;
  request.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  request.domain = OwnerDomain::kAsi;
  request.coverage = static_cast<CoverageState>(200);
  request.generation = EvidenceGeneration{1};
  request.observed_at = request.context.observation;
  request.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
  request.generations = fdc_test::generations(1);
  auto outcome = scaffold.coordinator().record_enumeration(request);
  FDC_CHECK_CODE(outcome, ErrorCode::kInvalidEnumValue);

  IngestCompletionRequest completion;
  completion.context = scaffold.next(kPlan, scaffold.revision_of(kPlan));
  completion.domain = OwnerDomain::kAsi;
  completion.state = static_cast<CompletionState>(77);
  completion.generation = EvidenceGeneration{1};
  completion.observed_at = completion.context.observation;
  completion.payload_digest = digest_text("payload");
  completion.manifest_digest = scaffold.plan(kPlan).spec.bindings.manifest_digest(OwnerDomain::kAsi).value();
  completion.scope_manifest_digest = scaffold.plan(kPlan).spec.targets.digest();
  completion.generations = fdc_test::generations(1);
  auto ingested = scaffold.coordinator().ingest_completion(completion);
  FDC_CHECK(ingested.error().code() == ErrorCode::kInvalidEnumValue ||
            ingested.error().code() == ErrorCode::kMissingRequiredField);
}
