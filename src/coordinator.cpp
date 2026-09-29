// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "coordinator_session.hpp"

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/errors.hpp"
#include "file_ops.hpp"
#include "utf8.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/report.hpp"
#include "facilitydrain/version.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace facilitydrain {

namespace {

/// A fresh, never zero writer identity for a coordinator that has no durable
/// identity to recover. It is derived from the process, the clock and a random
/// token so that two incarnations can never be confused for one another.
IncarnationId fresh_incarnation(const ClockPtr& clock) {
  const std::string material = "incarnation|" + std::to_string(detail::current_process_id()) + "|" +
                               std::to_string(clock->now_milliseconds()) + "|" + detail::random_token();
  const ContentDigest digest = digest_text(material);
  const auto bytes = digest.bytes();
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
  }
  return IncarnationId{value == 0 ? 1U : value};
}

}  // namespace

namespace detail {

Status validate_context(const Coordinator::Impl& impl, const MutationContext& context, const PlanRecord* plan,
                        bool require_plan, const Limits& limits) {
  const Status shape = context.validate(limits);
  if (!shape.ok()) {
    return shape;
  }
  if (context.incarnation != impl.state.incarnation) {
    return Status::failure(ErrorCode::kStaleAuthority,
                           "the mutation names incarnation " + to_string(context.incarnation) +
                               " but this coordinator is incarnation " + to_string(impl.state.incarnation));
  }
  if (context.expected_epoch != impl.state.control_epoch) {
    return Status::failure(ErrorCode::kEpochMismatch,
                           "the mutation names control epoch " + to_string(context.expected_epoch) +
                               " but the current control epoch is " + to_string(impl.state.control_epoch));
  }
  if (require_plan) {
    if (plan == nullptr) {
      return Status::failure(ErrorCode::kPlanNotFound,
                             "plan " + to_string(context.plan) + " is not known to this coordinator");
    }
    if (plan->spec.revision != context.expected_revision) {
      return Status::failure(ErrorCode::kRevisionConflict,
                             "the mutation names revision " + to_string(context.expected_revision) +
                                 " but plan " + to_string(context.plan) + " is at revision " +
                                 to_string(plan->spec.revision));
    }
    if (context.observation <= plan->last_observation) {
      return Status::failure(ErrorCode::kInvalidGenerationOrder,
                             "observation " + to_string(context.observation) +
                                 " does not advance the plan's last observation " +
                                 to_string(plan->last_observation));
    }
    return Status::success();
  }
  // Creating a plan has no prior plan state to order against, so the caller's
  // observation is only required to be a real observation. The coordinator's
  // sequence is a high water mark, not a global lock: two independent plans may
  // legitimately be observed concurrently by two callers, and requiring a
  // global order here would make concurrent creation impossible without buying
  // anything, because every authority decision is bound by generation, epoch
  // and revision rather than by this counter.
  return Status::success();
}

std::int64_t resolve_milliseconds(const ClockPtr& clock, std::int64_t requested) {
  if (requested != 0) {
    return requested;
  }
  return clock->now_milliseconds();
}

CommitSequence next_sequence(const CoordinatorState& state) {
  return CommitSequence{state.commit_sequence.value() + 1U};
}

Status publish_locked(Coordinator::Impl& impl, CoordinatorState&& next) {
  next.updated_at_milliseconds = impl.clock->now_milliseconds();
  if (impl.durable) {
    const Status published = impl.store.publish(next);
    if (!published.ok()) {
      // The durable generation is still the previous one, so the in memory
      // state must stay with it: a mutation that did not publish did not happen.
      return published;
    }
  }
  impl.state = std::move(next);
  return Status::success();
}

EvidenceGeneration latest_evidence_generation(const PlanRecord& plan, OwnerDomain domain) noexcept {
  EvidenceGeneration latest{};
  if (const EnumerationRecord* enumeration = newest_enumeration(plan, domain); enumeration != nullptr) {
    latest = enumeration->evidence.generation;
  }
  if (const CompletionEvidence* completion = newest_completion(plan, domain); completion != nullptr) {
    if (completion->generation > latest) {
      latest = completion->generation;
    }
  }
  return latest;
}

EvidenceId evidence_id_for(PlanId plan, OwnerDomain domain, EvidenceGeneration generation) noexcept {
  const std::string material = "evidence|" + to_string(plan) + "|" + std::string{to_token(domain)} + "|" +
                               to_string(generation);
  const ContentDigest digest = digest_text(material);
  const auto bytes = digest.bytes();
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
  }
  return EvidenceId{value == 0 ? 1U : value};
}

bool upsert_residual(PlanRecord& plan, ResidualEntry entry, const Limits& limits) {
  bool reopened = false;
  bool replaced = false;
  for (auto& existing : plan.residuals.entries) {
    if (!(existing == entry)) {
      continue;
    }
    // Same domain, same obligation, same reason: one ledger entry. A resolved
    // entry that is recorded open again means the obligation came back, which
    // is exactly the event that has to withdraw a removal answer.
    reopened = existing.state != ResidualState::kOpen;
    existing = std::move(entry);
    replaced = true;
    break;
  }
  if (!replaced) {
    plan.residuals.entries.push_back(std::move(entry));
  }
  std::sort(plan.residuals.entries.begin(), plan.residuals.entries.end());
  if (limits.max_residuals_per_plan != 0 && plan.residuals.entries.size() > limits.max_residuals_per_plan) {
    const std::size_t excess = plan.residuals.entries.size() - limits.max_residuals_per_plan;
    plan.residuals.entries.erase(plan.residuals.entries.begin(),
                                 plan.residuals.entries.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  return reopened;
}

void note_transition(PlanRecord& plan, DrainState previous, DrainState current, PlanOperation cause,
                     ObservationSequence observation, CommitSequence commit, std::int64_t milliseconds,
                     const Limits& limits, std::string detail) {
  record_history(plan, previous, current, cause, observation, commit, milliseconds, limits, std::move(detail));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Mutation context
// ---------------------------------------------------------------------------

Status MutationContext::validate(const Limits& limits) const {
  if (plan.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "context plan is not set");
  }
  if (expected_revision.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "context expected_revision is not set");
  }
  if (incarnation.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "context incarnation is not set");
  }
  if (expected_epoch.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "context expected_epoch is not set");
  }
  if (observation.value() == 0) {
    return Status::failure(ErrorCode::kMissingRequiredField, "context observation is not set");
  }
  return detail::validate_text(principal, "context principal", limits.max_text_bytes);
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

Coordinator::Coordinator(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Coordinator::~Coordinator() = default;
Coordinator::Coordinator(Coordinator&& other) noexcept = default;
Coordinator& Coordinator::operator=(Coordinator&& other) noexcept = default;

Result<Coordinator> Coordinator::open(const CoordinatorOpenRequest& request) {
  const Limits limits = request.limits;
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return Result<Coordinator>{limits_status.error()};
  }
  if (request.root.empty()) {
    return make_error<Coordinator>(ErrorCode::kMissingRequiredField, "the store root is empty");
  }
  if (request.read_only && request.create_if_missing) {
    // A read only session never creates anything, so asking it to create is a
    // contradiction rather than an option.
    return make_error<Coordinator>(ErrorCode::kConflictingField,
                                   "a read only session cannot create a store");
  }

  auto impl = std::make_unique<Impl>();
  impl->clock = request.clock ? request.clock : std::make_shared<const SystemClock>();
  impl->store_root = detail::to_utf8(request.root);
  impl->durable = true;

  detail::StoreOpenOptions options;
  options.root = request.root;
  options.limits = limits;
  options.faults = request.faults;
  options.writer_label = request.writer_label;
  options.create_if_missing = request.create_if_missing;
  options.read_only = request.read_only;

  auto session = detail::StoreSession::open(options, impl->recovery, impl->state);
  if (!session) {
    return Result<Coordinator>{session.error()};
  }
  impl->store = std::move(session).value();
  return Result<Coordinator>{Coordinator{std::move(impl)}};
}

Result<Coordinator> Coordinator::open_ephemeral(const EphemeralOptions& options) {
  const Limits limits = options.limits;
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return Result<Coordinator>{limits_status.error()};
  }

  auto impl = std::make_unique<Impl>();
  impl->clock = options.clock ? options.clock : std::make_shared<const SystemClock>();
  impl->durable = false;
  impl->state.payload_version = kStateFormatVersion;
  impl->state.limits = limits;
  impl->state.control_epoch = ControlEpoch{1};
  impl->state.incarnation = options.incarnation.value() != 0 ? options.incarnation
                                                             : fresh_incarnation(impl->clock);
  impl->state.commit_sequence = CommitSequence{0};
  impl->state.observation_sequence = ObservationSequence{0};
  impl->state.created_at_milliseconds = impl->clock->now_milliseconds();
  impl->state.updated_at_milliseconds = impl->state.created_at_milliseconds;
  impl->recovery.created_new_store = true;
  impl->recovery.current_epoch = impl->state.control_epoch;
  impl->recovery.detail = "ephemeral coordinator: no durable state";
  return Result<Coordinator>{Coordinator{std::move(impl)}};
}

// ---------------------------------------------------------------------------
// Facts
// ---------------------------------------------------------------------------

const RecoveryReport& Coordinator::recovery() const noexcept { return impl_->recovery; }
IncarnationId Coordinator::incarnation() const noexcept { return impl_->state.incarnation; }
ControlEpoch Coordinator::control_epoch() const noexcept { return impl_->state.control_epoch; }
CommitSequence Coordinator::commit_sequence() const noexcept { return impl_->state.commit_sequence; }
ObservationSequence Coordinator::observation_sequence() const noexcept {
  return impl_->state.observation_sequence;
}
Limits Coordinator::limits() const noexcept { return impl_->state.limits; }
bool Coordinator::durable() const noexcept { return impl_->durable; }

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Result<CoordinatorSnapshot> Coordinator::snapshot() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<CoordinatorSnapshot>{
      detail::build_snapshot(impl_->state, impl_->durable, impl_->store_root)};
}

Result<DrainPlanSnapshot> Coordinator::plan(PlanId id) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const detail::PlanRecord* record = detail::find_plan(impl_->state, id);
  if (record == nullptr) {
    return make_error<DrainPlanSnapshot>(ErrorCode::kPlanNotFound,
                                         "plan " + to_string(id) + " is not known to this coordinator");
  }
  return Result<DrainPlanSnapshot>{
      detail::build_plan_snapshot(*record, impl_->state.control_epoch)};
}

Result<ResidualLedger> Coordinator::residuals(PlanId id) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const detail::PlanRecord* record = detail::find_plan(impl_->state, id);
  if (record == nullptr) {
    return make_error<ResidualLedger>(ErrorCode::kPlanNotFound,
                                      "plan " + to_string(id) + " is not known to this coordinator");
  }
  return Result<ResidualLedger>{record->residuals};
}

Result<std::vector<DrainRequest>> Coordinator::requests(PlanId id) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const detail::PlanRecord* record = detail::find_plan(impl_->state, id);
  if (record == nullptr) {
    return make_error<std::vector<DrainRequest>>(
        ErrorCode::kPlanNotFound, "plan " + to_string(id) + " is not known to this coordinator");
  }
  return Result<std::vector<DrainRequest>>{record->requests};
}

Result<SafeToRemoveEvaluation> Coordinator::evaluate_safe_to_remove(PlanId id) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const detail::PlanRecord* record = detail::find_plan(impl_->state, id);
  if (record == nullptr) {
    return make_error<SafeToRemoveEvaluation>(
        ErrorCode::kPlanNotFound, "plan " + to_string(id) + " is not known to this coordinator");
  }
  const DrainPlanSnapshot view = detail::build_plan_snapshot(*record, impl_->state.control_epoch);
  return Result<SafeToRemoveEvaluation>{detail::evaluate_safe_to_remove(view, impl_->state.control_epoch)};
}

Result<std::string> Coordinator::explain(PlanId id) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const detail::PlanRecord* record = detail::find_plan(impl_->state, id);
  if (record == nullptr) {
    return make_error<std::string>(ErrorCode::kPlanNotFound,
                                   "plan " + to_string(id) + " is not known to this coordinator");
  }
  const DrainPlanSnapshot view = detail::build_plan_snapshot(*record, impl_->state.control_epoch);
  const SafeToRemoveEvaluation evaluation =
      detail::evaluate_safe_to_remove(view, impl_->state.control_epoch);
  std::string text = "plan " + to_string(id) + " scope=" + format_scope(view.spec.scope) +
                     " state=" + std::string{to_token(view.state)} +
                     " revision=" + to_string(view.spec.revision) +
                     " grant-live=" + (view.grant_live ? "yes" : "no") + "\n";
  text.append(evaluation.explanation);
  return Result<std::string>{std::move(text)};
}

Result<std::string> Coordinator::export_text(const ReportOptions& options) const {
  const auto current = snapshot();
  if (!current) {
    return Result<std::string>{current.error()};
  }
  return facilitydrain::export_coordinator_text(current.value(), options);
}

Result<std::string> Coordinator::export_json(const ReportOptions& options) const {
  const auto current = snapshot();
  if (!current) {
    return Result<std::string>{current.error()};
  }
  return facilitydrain::export_coordinator_json(current.value(), options);
}

Result<std::string> Coordinator::export_plan_text(PlanId id, const ReportOptions& options) const {
  const auto view = plan(id);
  if (!view) {
    return Result<std::string>{view.error()};
  }
  return facilitydrain::export_plan_text(view.value(), options);
}

Result<std::string> Coordinator::export_plan_json(PlanId id, const ReportOptions& options) const {
  const auto view = plan(id);
  if (!view) {
    return Result<std::string>{view.error()};
  }
  return facilitydrain::export_plan_json(view.value(), options);
}

Status Coordinator::flush() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->durable) {
    return Status::success();
  }
  return impl_->store.flush();
}

}  // namespace facilitydrain
