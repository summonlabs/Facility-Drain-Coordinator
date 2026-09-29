// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/report.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace facilitydrain;

namespace {

constexpr std::int64_t kClockMilliseconds = 1767225600000;
constexpr std::uint64_t kTestIncarnation = 0x5EEDULL;
constexpr std::string_view kFormatHeader = "format facility-drain-coordinator/1";

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

[[nodiscard]] Result<Coordinator> open_coordinator() {
  EphemeralOptions options;
  options.clock = test_clock();
  options.incarnation = IncarnationId{kTestIncarnation};
  return Coordinator::open_ephemeral(options);
}

[[nodiscard]] Revision current_revision(const Coordinator& coordinator, PlanId plan) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(plan);
  return view.has_value() ? view.value().spec.revision : Revision{1};
}

[[nodiscard]] MutationContext context_for(const Coordinator& coordinator, PlanId plan) {
  MutationContext context;
  context.plan = plan;
  context.expected_revision = current_revision(coordinator, plan);
  context.incarnation = coordinator.incarnation();
  context.expected_epoch = coordinator.control_epoch();
  context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  context.principal = "report-tests";
  context.requested_at_milliseconds = kClockMilliseconds;
  return context;
}

[[nodiscard]] ConsumerRecord asi_obligation(ObligationId obligation) {
  ConsumerRecord record;
  record.obligation = obligation;
  record.category = ConsumerCategory::kWorkload;
  record.generation = ObligationGeneration{100};
  record.reservation = ReservationGeneration{7};
  record.strength = ObligationStrength::kMandatory;
  record.label = "workload-alpha";
  record.source = "asi";
  return record;
}

[[nodiscard]] ConsumerRecord dfi_obligation(ObligationId obligation) {
  ConsumerRecord record;
  record.obligation = obligation;
  record.category = ConsumerCategory::kNetworkPath;
  record.generation = ObligationGeneration{200};
  record.reservation = ReservationGeneration{8};
  record.strength = ObligationStrength::kMandatory;
  record.label = "path-beta";
  record.source = "dfi";
  return record;
}

[[nodiscard]] ContentDigest scope_digest(const Coordinator& coordinator, PlanId id) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(id);
  return view.has_value() ? view.value().spec.targets.digest() : ContentDigest{};
}

[[nodiscard]] ContentDigest bound_manifest(const Coordinator& coordinator, PlanId id, OwnerDomain domain) {
  const Result<DrainPlanSnapshot> view = coordinator.plan(id);
  if (!view.has_value()) {
    return ContentDigest{};
  }
  const std::optional<ContentDigest>& bound = view.value().spec.bindings.manifest_digest(domain);
  return bound.has_value() ? bound.value() : ContentDigest{};
}

[[nodiscard]] Result<RecordEnumerationOutcome> enumerate(Coordinator& coordinator, PlanId id, OwnerDomain domain,
                                                         EvidenceGeneration generation,
                                                         const std::vector<ConsumerRecord>& consumers) {
  RecordEnumerationRequest request;
  request.context = context_for(coordinator, id);
  request.domain = domain;
  request.coverage = CoverageState::kComplete;
  request.generation = generation;
  request.observed_at = context_for(coordinator, id).observation;
  request.scope_manifest_digest = scope_digest(coordinator, id);
  request.consumers = consumers;
  request.generations = complete_generations();
  request.source = std::string{to_token(domain)};
  return coordinator.record_enumeration(request);
}

[[nodiscard]] Result<IngestCompletionOutcome> complete(Coordinator& coordinator, PlanId id, OwnerDomain domain,
                                                       EvidenceGeneration generation) {
  IngestCompletionRequest request;
  request.context = context_for(coordinator, id);
  request.domain = domain;
  request.state = CompletionState::kDrained;
  request.generation = generation;
  request.observed_at = context_for(coordinator, id).observation;
  request.payload_digest = digest_text("completion-payload");
  request.manifest_digest = bound_manifest(coordinator, id, domain);
  request.scope_manifest_digest = scope_digest(coordinator, id);
  request.generations = complete_generations();
  request.residual_count_known = true;
  request.residual_count = 0;
  request.source = std::string{to_token(domain)};
  return coordinator.ingest_completion(request);
}

[[nodiscard]] Result<RecordResidualOutcome> record_residual(Coordinator& coordinator, PlanId id,
                                                            ObligationId obligation, OwnerDomain domain,
                                                            ResidualKind kind, std::string detail) {
  RecordResidualRequest request;
  request.context = context_for(coordinator, id);
  request.entry.obligation = obligation;
  request.entry.domain = domain;
  request.entry.kind = kind;
  request.entry.generation = ObligationGeneration{300};
  request.entry.detail = std::move(detail);
  return coordinator.record_residual(request);
}

[[nodiscard]] Result<IssueRequestsOutcome> issue(Coordinator& coordinator, PlanId id, DomainMask domains) {
  IssueRequestsRequest request;
  request.context = context_for(coordinator, id);
  request.domains = domains;
  return coordinator.issue_requests(request);
}

/// A plan with two required domains, two open residuals, two staged requests
/// and a live grant, so every section of a report has something to render.
[[nodiscard]] Result<DrainPlanSnapshot> build_rich_plan(Coordinator& coordinator) {
  const std::vector<ConsumerRecord> asi{asi_obligation(ObligationId{1001})};
  const std::vector<ConsumerRecord> dfi{dfi_obligation(ObligationId{2002})};

  CreatePlanRequest create;
  create.context = context_for(coordinator, PlanId{1});
  create.id = PlanId{1};
  create.scope = DrainScope{ScopeKind::kAsset, 1};
  create.targets = std::vector<DrainScope>{create.scope};
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  create.generations = complete_generations();
  create.policy_id = PolicyId{11};
  create.policy_digest = digest_text("policy-document");
  create.consumers = std::vector<ConsumerRecord>{asi[0], dfi[0]};
  create.label = "report asset 1";
  create.requested_by = "operator";

  const Result<CreatePlanOutcome> created = coordinator.create_plan(create);
  if (!created.has_value()) {
    return Result<DrainPlanSnapshot>{created.error()};
  }
  const Result<RecordEnumerationOutcome> enumerated_asi =
      enumerate(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{1}, asi);
  if (!enumerated_asi.has_value()) {
    return Result<DrainPlanSnapshot>{enumerated_asi.error()};
  }
  const Result<RecordEnumerationOutcome> enumerated_dfi =
      enumerate(coordinator, PlanId{1}, OwnerDomain::kDfi, EvidenceGeneration{1}, dfi);
  if (!enumerated_dfi.has_value()) {
    return Result<DrainPlanSnapshot>{enumerated_dfi.error()};
  }
  const Result<RecordResidualOutcome> first_residual =
      record_residual(coordinator, PlanId{1}, ObligationId{3001}, OwnerDomain::kMonitoring,
                      ResidualKind::kObligationActive, "monitoring still reports a dependency");
  if (!first_residual.has_value()) {
    return Result<DrainPlanSnapshot>{first_residual.error()};
  }
  const Result<RecordResidualOutcome> second_residual =
      record_residual(coordinator, PlanId{1}, ObligationId{3002}, OwnerDomain::kMonitoring,
                      ResidualKind::kOwnerRefused, "monitoring declined to relinquish");
  if (!second_residual.has_value()) {
    return Result<DrainPlanSnapshot>{second_residual.error()};
  }
  const Result<IssueRequestsOutcome> issued = issue(
      coordinator, PlanId{1}, DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi));
  if (!issued.has_value()) {
    return Result<DrainPlanSnapshot>{issued.error()};
  }
  const Result<IngestCompletionOutcome> completed_asi =
      complete(coordinator, PlanId{1}, OwnerDomain::kAsi, EvidenceGeneration{2});
  if (!completed_asi.has_value()) {
    return Result<DrainPlanSnapshot>{completed_asi.error()};
  }
  const Result<IngestCompletionOutcome> completed_dfi =
      complete(coordinator, PlanId{1}, OwnerDomain::kDfi, EvidenceGeneration{2});
  if (!completed_dfi.has_value()) {
    return Result<DrainPlanSnapshot>{completed_dfi.error()};
  }
  GrantSafeToRemoveRequest grant;
  grant.context = context_for(coordinator, PlanId{1});
  grant.granted_by = "operator";
  const Result<GrantSafeToRemoveOutcome> granted = coordinator.grant_safe_to_remove(grant);
  if (!granted.has_value()) {
    return Result<DrainPlanSnapshot>{granted.error()};
  }
  return coordinator.plan(PlanId{1});
}

[[nodiscard]] std::string text_report(const Result<std::string>& report) {
  return report.has_value() ? report.value() : std::string{};
}

[[nodiscard]] std::string first_line(const std::string& text) {
  const std::size_t end = text.find('\n');
  return end == std::string::npos ? text : text.substr(0, end);
}

[[nodiscard]] bool mentions(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

/// The canonical report line for one residual record. The report grammar fixes
/// the keyword and the field order, so the expected text is derived from the
/// record itself rather than from a copy of the rendering.
[[nodiscard]] std::string residual_record_text(const ResidualEntry& entry) {
  std::string text{"residual domain="};
  text += to_token(entry.domain);
  text += " obligation=";
  text += entry.obligation.is_default() ? std::string{"scope"} : to_string(entry.obligation);
  text += " kind=";
  text += to_token(entry.kind);
  text += " state=";
  text += to_token(entry.state);
  return text;
}

/// The canonical report line for one bounded drain request, up to the attempt.
[[nodiscard]] std::string request_record_text(const DrainRequest& request) {
  std::string text{"request id="};
  text += to_string(request.id);
  text += " domain=";
  text += to_token(request.key.domain);
  text += " scope=";
  text += format_scope(request.key.scope);
  text += " state=";
  text += to_token(request.state);
  text += " attempt=";
  text += to_string(request.key.attempt);
  return text;
}

/// The canonical report line for one consumer record.
[[nodiscard]] std::string consumer_record_text(const ConsumerRecord& consumer) {
  std::string text{"consumer domain="};
  text += to_token(consumer.domain());
  text += " obligation=";
  text += to_string(consumer.obligation);
  return text;
}

}  // namespace

FDC_TEST(report, the_text_report_starts_with_the_canonical_format_line) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(build_rich_plan(coordinator));

  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  FDC_REQUIRE_OK(snapshot);
  Result<std::string> coordinator_text = export_coordinator_text(snapshot.value());
  FDC_REQUIRE_OK(coordinator_text);
  FDC_CHECK_EQ(first_line(coordinator_text.value()), std::string{kFormatHeader});

  Result<DrainPlanSnapshot> plan = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(plan);
  Result<std::string> plan_text = export_plan_text(plan.value());
  FDC_REQUIRE_OK(plan_text);
  FDC_CHECK_EQ(first_line(plan_text.value()), std::string{kFormatHeader});
}

FDC_TEST(report, rendering_the_same_snapshot_twice_is_byte_identical) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(build_rich_plan(coordinator));

  Result<DrainPlanSnapshot> plan = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(plan);
  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  FDC_REQUIRE_OK(snapshot);

  const std::string plan_first = text_report(export_plan_text(plan.value()));
  const std::string plan_second = text_report(export_plan_text(plan.value()));
  FDC_CHECK(!plan_first.empty());
  FDC_CHECK_EQ(plan_first, plan_second);

  const std::string coordinator_first = text_report(export_coordinator_text(snapshot.value()));
  const std::string coordinator_second = text_report(export_coordinator_text(snapshot.value()));
  FDC_CHECK(!coordinator_first.empty());
  FDC_CHECK_EQ(coordinator_first, coordinator_second);

  const std::string plan_json_first = text_report(export_plan_json(plan.value()));
  const std::string plan_json_second = text_report(export_plan_json(plan.value()));
  FDC_CHECK_EQ(plan_json_first, plan_json_second);

  const std::string coordinator_json_first = text_report(export_coordinator_json(snapshot.value()));
  const std::string coordinator_json_second = text_report(export_coordinator_json(snapshot.value()));
  FDC_CHECK_EQ(coordinator_json_first, coordinator_json_second);
}

FDC_TEST(report, the_json_rendering_is_stable_and_carries_no_floating_point) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(build_rich_plan(coordinator));

  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  FDC_REQUIRE_OK(snapshot);

  ReportOptions compact;
  compact.pretty = false;
  Result<std::string> json = export_coordinator_json(snapshot.value(), compact);
  FDC_REQUIRE_OK(json);
  FDC_CHECK(!json.value().empty());
  // No floating point value is ever rendered: every number in the canonical
  // format is an integer, a count or an identity.
  FDC_CHECK_EQ(json.value().find('.'), std::string::npos);
  FDC_CHECK(mentions(json.value(), "safe-to-remove"));

  Result<std::string> pretty = export_plan_json(snapshot.value().plans.front());
  FDC_REQUIRE_OK(pretty);
  FDC_CHECK_EQ(pretty.value().find('.'), std::string::npos);
}

FDC_TEST(report, json_escape_handles_quotes_backslashes_and_control_characters) {
  FDC_CHECK_EQ(json_escape("plain"), std::string{"plain"});
  FDC_CHECK_EQ(json_escape(""), std::string{""});

  // The rule is byte exact: '"' and '\\' are escaped, and every C0 control
  // character and U+007F is escaped as \u00XX with uppercase hex digits.
  const std::string raw = std::string{"a\"b\\c\nd\te"} + '\x01' + "f";
  FDC_CHECK_EQ(json_escape(raw), std::string{"a\\\"b\\\\c\\u000Ad\\u0009e\\u0001f"});
  FDC_CHECK_EQ(json_escape(std::string{"a"} + '\x7F'), std::string{"a\\u007F"});
  FDC_CHECK_EQ(json_escape("asset:1|rack:9"), std::string{"asset:1|rack:9"});
}

FDC_TEST(report, bounded_reports_omit_records_and_say_that_they_did) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(build_rich_plan(coordinator));

  Result<DrainPlanSnapshot> plan = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(plan);
  FDC_REQUIRE(plan.value().residuals.entries.size() == std::size_t{2});
  FDC_REQUIRE(plan.value().requests.size() == std::size_t{2});

  const std::string full = text_report(export_plan_text(plan.value()));
  FDC_CHECK(mentions(full, residual_record_text(plan.value().residuals.entries.front())));
  FDC_CHECK(mentions(full, request_record_text(plan.value().requests.front())));

  ReportOptions one_residual;
  one_residual.max_residuals = 1;
  const std::string bounded_residuals = text_report(export_plan_text(plan.value(), one_residual));
  FDC_CHECK(!bounded_residuals.empty());
  FDC_CHECK(bounded_residuals != full);
  // A bounded list states what it left out rather than silently shortening.
  FDC_CHECK(mentions(bounded_residuals, "omit"));

  ReportOptions one_request;
  one_request.max_requests = 1;
  const std::string bounded_requests = text_report(export_plan_text(plan.value(), one_request));
  FDC_CHECK(!bounded_requests.empty());
  FDC_CHECK(bounded_requests != full);
  FDC_CHECK(mentions(bounded_requests, "omit"));

  ReportOptions no_residuals;
  no_residuals.include_residuals = false;
  const std::string without_residuals = text_report(export_plan_text(plan.value(), no_residuals));
  FDC_CHECK(!without_residuals.empty());
  FDC_CHECK(!mentions(without_residuals, residual_record_text(plan.value().residuals.entries.front())));
  FDC_CHECK(!mentions(without_residuals, residual_record_text(plan.value().residuals.entries.back())));
  FDC_CHECK(mentions(without_residuals, "omit"));

  ReportOptions no_requests;
  no_requests.include_requests = false;
  const std::string without_requests = text_report(export_plan_text(plan.value(), no_requests));
  FDC_CHECK(!without_requests.empty());
  FDC_CHECK(!mentions(without_requests, request_record_text(plan.value().requests.front())));
  FDC_CHECK(!mentions(without_requests, request_record_text(plan.value().requests.back())));
  FDC_CHECK(mentions(without_requests, "omit"));

  ReportOptions no_consumers;
  no_consumers.include_consumers = false;
  const std::string without_consumers = text_report(export_plan_text(plan.value(), no_consumers));
  FDC_CHECK(!without_consumers.empty());
  FDC_CHECK(!mentions(without_consumers, consumer_record_text(plan.value().consumers.front())));
  FDC_CHECK(mentions(without_consumers, "omit"));
}

FDC_TEST(report, a_report_carries_the_canonical_text_of_every_recorded_fact) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(build_rich_plan(coordinator));

  Result<DrainPlanSnapshot> plan = coordinator.plan(PlanId{1});
  FDC_REQUIRE_OK(plan);
  const DrainPlanSnapshot& view = plan.value();

  const std::string text = text_report(export_plan_text(view));
  FDC_CHECK(!text.empty());
  FDC_CHECK(mentions(text, view.spec.targets.to_canonical()));
  FDC_CHECK(mentions(text, std::string{to_token(view.state)}));
  for (const ResidualEntry& entry : view.residuals.entries) {
    FDC_CHECK(mentions(text, residual_record_text(entry)));
  }
  for (const DrainRequest& request : view.requests) {
    FDC_CHECK(mentions(text, request_record_text(request)));
  }
  for (const ConsumerRecord& consumer : view.consumers) {
    FDC_CHECK(mentions(text, consumer_record_text(consumer)));
  }
  FDC_REQUIRE(view.grant.has_value());
  FDC_CHECK(mentions(text, "grant present=yes live=yes"));
  FDC_CHECK(mentions(text, view.grant.value().evidence_digest.to_hex()));

  const std::string json = text_report(export_plan_json(view));
  FDC_CHECK(!json.empty());
  FDC_CHECK(mentions(json, std::string{to_token(view.state)}));
}

FDC_TEST(report, format_evaluation_is_deterministic) {
  Result<Coordinator> opened = open_coordinator();
  FDC_REQUIRE_OK(opened);
  Coordinator& coordinator = opened.value();
  FDC_REQUIRE_OK(build_rich_plan(coordinator));

  Result<SafeToRemoveEvaluation> first = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(first);
  Result<SafeToRemoveEvaluation> second = coordinator.evaluate_safe_to_remove(PlanId{1});
  FDC_REQUIRE_OK(second);

  const std::string rendered = format_evaluation(first.value());
  FDC_CHECK_EQ(rendered, format_evaluation(second.value()));
  FDC_CHECK_EQ(rendered, first.value().explanation);
  FDC_CHECK(mentions(rendered, "verdict granted"));
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    FDC_CHECK(mentions(rendered, std::string{"domain "} + std::string{to_token(owner_domain_at(index))}));
  }
}
