// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "state_codec.hpp"

#include "byte_codec.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace facilitydrain {
namespace detail {
namespace {

// ---------------------------------------------------------------------------
// Rejection helpers
// ---------------------------------------------------------------------------
//
// Every rejection names the field it happened in. The detail text is diagnostic
// only, but it is deterministic: the same corrupt payload always produces the
// same words, which is what makes a failing decode reproducible rather than
// dependent on where the reader happened to notice.

[[nodiscard]] Status truncated(std::string_view field) {
  std::string detail = "state payload ends inside ";
  detail.append(field);
  return Status::failure(ErrorCode::kStoreTruncated, detail);
}

[[nodiscard]] Status unknown_enum_value(std::string_view field, std::uint8_t raw) {
  std::string detail = "field ";
  detail.append(field);
  detail.append(" holds the unknown value ");
  detail.append(std::to_string(static_cast<unsigned int>(raw)));
  return Status::failure(ErrorCode::kInvalidEnumValue, detail);
}

[[nodiscard]] Status over_long_field(std::string_view field) {
  std::string detail = "field ";
  detail.append(field);
  detail.append(" declares more bytes than the configured bound allows");
  return Status::failure(ErrorCode::kPayloadTooLarge, detail);
}

[[nodiscard]] Status too_many(std::string_view field, std::size_t actual, std::uint64_t bound) {
  std::string detail = "field ";
  detail.append(field);
  detail.append(" declares ");
  detail.append(std::to_string(static_cast<std::uint64_t>(actual)));
  detail.append(" entries, the bound is ");
  detail.append(std::to_string(bound));
  return Status::failure(ErrorCode::kTooManyEntries, detail);
}

[[nodiscard]] Status malformed(std::string detail) {
  return Status::failure(ErrorCode::kMalformedRecord, detail);
}

[[nodiscard]] std::string plan_name(const PlanRecord& plan) {
  std::string text = "plan ";
  text += to_string(plan.spec.id);
  return text;
}

[[nodiscard]] Result<CoordinatorState> rejected(const Status& status) {
  return Result<CoordinatorState>{status.error()};
}

// ---------------------------------------------------------------------------
// Known enumerations
// ---------------------------------------------------------------------------
//
// Each enumeration occupies one byte and is validated against the complete set
// of values this build knows. The set is spelled out as case labels rather than
// a numeric range so that adding an enumerator to a public header is a
// deliberate edit here as well: a value this build does not know is refused,
// never folded onto the nearest value it does know.

[[nodiscard]] bool is_known_scope_kind(std::uint8_t raw) noexcept {
  switch (static_cast<ScopeKind>(raw)) {
    case ScopeKind::kAsset:
    case ScopeKind::kRack:
    case ScopeKind::kZone:
    case ScopeKind::kSubscope:
    case ScopeKind::kSite:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_owner_domain(std::uint8_t raw) noexcept {
  switch (static_cast<OwnerDomain>(raw)) {
    case OwnerDomain::kAsi:
    case OwnerDomain::kDfi:
    case OwnerDomain::kFacility:
    case OwnerDomain::kMonitoring:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_consumer_category(std::uint8_t raw) noexcept {
  switch (static_cast<ConsumerCategory>(raw)) {
    case ConsumerCategory::kWorkload:
    case ConsumerCategory::kAcceleratorReservation:
    case ConsumerCategory::kNetworkPath:
    case ConsumerCategory::kNetworkAttachment:
    case ConsumerCategory::kServiceClass:
    case ConsumerCategory::kMaintenanceProtection:
    case ConsumerCategory::kFacilityReservation:
    case ConsumerCategory::kMonitoringDependency:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_obligation_strength(std::uint8_t raw) noexcept {
  switch (static_cast<ObligationStrength>(raw)) {
    case ObligationStrength::kMandatory:
    case ObligationStrength::kAdvisory:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_coverage_state(std::uint8_t raw) noexcept {
  switch (static_cast<CoverageState>(raw)) {
    case CoverageState::kNotEnumerated:
    case CoverageState::kPartial:
    case CoverageState::kComplete:
    case CoverageState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_completion_state(std::uint8_t raw) noexcept {
  switch (static_cast<CompletionState>(raw)) {
    case CompletionState::kUnknown:
    case CompletionState::kRequested:
    case CompletionState::kAcknowledged:
    case CompletionState::kDraining:
    case CompletionState::kDrainedWithResiduals:
    case CompletionState::kDrained:
    case CompletionState::kRefused:
    case CompletionState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_residual_kind(std::uint8_t raw) noexcept {
  switch (static_cast<ResidualKind>(raw)) {
    case ResidualKind::kObligationActive:
    case ResidualKind::kObligationUnknown:
    case ResidualKind::kOwnerRefused:
    case ResidualKind::kEnumerationIncomplete:
    case ResidualKind::kEvidenceMissing:
    case ResidualKind::kResidualCountUnknown:
    case ResidualKind::kRequestUnacknowledged:
    case ResidualKind::kDomainFailed:
    case ResidualKind::kEvidenceStale:
    case ResidualKind::kProtectedObligation:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_residual_state(std::uint8_t raw) noexcept {
  switch (static_cast<ResidualState>(raw)) {
    case ResidualState::kOpen:
    case ResidualState::kRelinquished:
    case ResidualState::kSuperseded:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_request_state(std::uint8_t raw) noexcept {
  switch (static_cast<RequestState>(raw)) {
    case RequestState::kStaged:
    case RequestState::kIssued:
    case RequestState::kAcknowledged:
    case RequestState::kCompleted:
    case RequestState::kRefused:
    case RequestState::kFailed:
    case RequestState::kSuperseded:
    case RequestState::kCancelled:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_drain_state(std::uint8_t raw) noexcept {
  switch (static_cast<DrainState>(raw)) {
    case DrainState::kProposed:
    case DrainState::kEnumerating:
    case DrainState::kRequested:
    case DrainState::kDraining:
    case DrainState::kResidualsPresent:
    case DrainState::kDrained:
    case DrainState::kSafeToRemove:
    case DrainState::kCancelled:
    case DrainState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_plan_operation(std::uint8_t raw) noexcept {
  switch (static_cast<PlanOperation>(raw)) {
    case PlanOperation::kRevise:
    case PlanOperation::kRecordEnumeration:
    case PlanOperation::kIssueRequests:
    case PlanOperation::kRecordAcknowledgement:
    case PlanOperation::kIngestCompletion:
    case PlanOperation::kRecordResidual:
    case PlanOperation::kResolveResidual:
    case PlanOperation::kEvaluate:
    case PlanOperation::kGrant:
    case PlanOperation::kFence:
    case PlanOperation::kCancel:
    case PlanOperation::kFail:
    case PlanOperation::kSupersedeRequest:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_fence_reason(std::uint8_t raw) noexcept {
  switch (static_cast<FenceReason>(raw)) {
    case FenceReason::kNone:
    case FenceReason::kRestart:
    case FenceReason::kControlEpochChanged:
    case FenceReason::kNewObligation:
    case FenceReason::kPlanRevised:
    case FenceReason::kEvidenceSuperseded:
    case FenceReason::kScopeManifestChanged:
    case FenceReason::kOperatorFence:
    case FenceReason::kPlanCancelled:
    case FenceReason::kPlanFailed:
    case FenceReason::kDependencyChange:
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Text policy
// ---------------------------------------------------------------------------
//
// The format has exactly two text policies: identity, label and source text is
// bounded by max_text_bytes, and free form annotation, detail and explanation
// text by max_annotation_bytes. Naming the policy once is what keeps the
// reader's bound and the validator's bound the same number, so a state that can
// be encoded can always be decoded again.

enum class TextPolicy : std::uint8_t {
  kIdentity,
  kAnnotation,
};

[[nodiscard]] std::uint32_t text_bound(const Limits& limits, TextPolicy policy) noexcept {
  return policy == TextPolicy::kAnnotation ? limits.max_annotation_bytes : limits.max_text_bytes;
}

[[nodiscard]] Status validate_field(const std::string& value, std::string_view field, const Limits& limits,
                                    TextPolicy policy) {
  return validate_text(value, field, text_bound(limits, policy));
}

// ---------------------------------------------------------------------------
// Reading primitives
// ---------------------------------------------------------------------------

[[nodiscard]] Status read_bool(ByteReader& reader, std::string_view field, bool& out) {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) {
    return truncated(field);
  }
  // A boolean flag and an optional's presence byte are the same field shape: 0
  // or 1 and nothing else. Any other value is an unknown encoding, not a
  // truthy byte, so it is refused rather than interpreted.
  if (raw > 1U) {
    return unknown_enum_value(field, raw);
  }
  out = raw != 0;
  return Status::success();
}

[[nodiscard]] Status read_text(ByteReader& reader, const Limits& limits, TextPolicy policy, std::string_view field,
                               std::string& out) {
  const std::uint32_t bound = text_bound(limits, policy);
  std::string text;
  if (!reader.sized_string(bound, text)) {
    // A declared length above the bound never reaches the payload bytes, and a
    // declared length running past the input is truncation rather than an
    // over-long field. The reader records which of the two it saw.
    return reader.limit_exceeded() ? over_long_field(field) : truncated(field);
  }
  const Status status = validate_text(text, field, bound);
  if (!status.ok()) {
    return status;
  }
  out = std::move(text);
  return Status::success();
}

[[nodiscard]] Status read_digest(ByteReader& reader, std::string_view field, ContentDigest& out) {
  std::array<std::byte, ContentDigest::kSize> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    std::uint8_t byte = 0;
    if (!reader.u8(byte)) {
      return truncated(field);
    }
    bytes[index] = static_cast<std::byte>(byte);
  }
  out = ContentDigest::from_bytes(
      std::span<const std::byte, ContentDigest::kSize>{bytes.data(), ContentDigest::kSize});
  return Status::success();
}

// The ten generations are written in the fixed order the format specification
// lists, which is the order GenerationSet declares them in. Collecting them
// into one array is what makes the write loop and the read loop share a single
// statement of that order instead of two that can drift apart.
using GenerationFields = std::array<std::uint64_t, kGenerationFieldCount>;

[[nodiscard]] GenerationFields generation_fields(const GenerationSet& generations) noexcept {
  return GenerationFields{generations.scope.value(),       generations.dependency.value(),
                          generations.reservation.value(), generations.obligation.value(),
                          generations.policy.value(),      generations.topology.value(),
                          generations.maintenance.value(), generations.capacity.value(),
                          generations.hardware.value(),    generations.firmware.value()};
}

[[nodiscard]] GenerationSet generation_set_from(const GenerationFields& fields) noexcept {
  GenerationSet generations{};
  generations.scope = ScopeGeneration{fields[0]};
  generations.dependency = DependencyGeneration{fields[1]};
  generations.reservation = ReservationGeneration{fields[2]};
  generations.obligation = ObligationGeneration{fields[3]};
  generations.policy = PolicyGeneration{fields[4]};
  generations.topology = TopologyGeneration{fields[5]};
  generations.maintenance = MaintenanceGeneration{fields[6]};
  generations.capacity = CapacityGeneration{fields[7]};
  generations.hardware = HardwareGeneration{fields[8]};
  generations.firmware = FirmwareGeneration{fields[9]};
  return generations;
}

[[nodiscard]] Status read_generation_set(ByteReader& reader, std::string_view field, GenerationSet& out) {
  GenerationFields fields{};
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (!reader.u64(fields[index])) {
      return truncated(field);
    }
  }
  out = generation_set_from(fields);
  return Status::success();
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------
//
// The writer cannot fail: the text bounds are enforced by validate_state before
// encoding starts, and every collection bound is checked below before its count
// is written, so a payload is never produced with a count a reader would refuse.

[[nodiscard]] constexpr std::uint8_t flag_byte(bool value) noexcept {
  return value ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0);
}

void write_generation_set(ByteWriter& writer, const GenerationSet& generations) {
  const GenerationFields fields = generation_fields(generations);
  for (const std::uint64_t field : fields) {
    writer.u64(field);
  }
}

void write_digest(ByteWriter& writer, const ContentDigest& digest) {
  writer.raw(digest.bytes());
}

void write_consumer_record(ByteWriter& writer, const ConsumerRecord& record) {
  writer.u64(record.obligation.value());
  writer.u8(static_cast<std::uint8_t>(record.category));
  writer.u64(record.generation.value());
  writer.u64(record.reservation.value());
  writer.u8(static_cast<std::uint8_t>(record.strength));
  writer.sized_string(record.label);
  writer.sized_string(record.source);
}

[[nodiscard]] Status write_enumeration_record(ByteWriter& writer, const EnumerationRecord& record,
                                              const Limits& limits) {
  const EnumerationEvidence& evidence = record.evidence;
  writer.u8(static_cast<std::uint8_t>(evidence.domain));
  writer.u8(static_cast<std::uint8_t>(evidence.coverage));
  writer.u64(evidence.generation.value());
  writer.u64(evidence.observed_at.value());
  write_digest(writer, evidence.manifest_digest);
  write_digest(writer, evidence.scope_manifest_digest);
  write_generation_set(writer, evidence.generations);
  writer.sized_string(evidence.source);
  writer.sized_string(evidence.annotation);
  writer.i64(evidence.observed_at_milliseconds);
  if (record.consumers.size() > static_cast<std::size_t>(limits.max_consumers_per_domain)) {
    return too_many("enumeration consumer count", record.consumers.size(), limits.max_consumers_per_domain);
  }
  writer.u32(static_cast<std::uint32_t>(record.consumers.size()));
  for (const ConsumerRecord& consumer : record.consumers) {
    write_consumer_record(writer, consumer);
  }
  return Status::success();
}

void write_completion_record(ByteWriter& writer, const CompletionEvidence& record) {
  writer.u64(record.id.value());
  writer.u8(static_cast<std::uint8_t>(record.domain));
  writer.u8(static_cast<std::uint8_t>(record.state));
  writer.u64(record.generation.value());
  writer.u64(record.observed_at.value());
  write_digest(writer, record.payload_digest);
  write_digest(writer, record.manifest_digest);
  write_digest(writer, record.scope_manifest_digest);
  write_generation_set(writer, record.generations);
  writer.u8(flag_byte(record.residual_count_known));
  writer.u64(record.residual_count);
  writer.sized_string(record.source);
  writer.sized_string(record.annotation);
  writer.i64(record.observed_at_milliseconds);
}

void write_residual_entry(ByteWriter& writer, const ResidualEntry& entry) {
  writer.u64(entry.obligation.value());
  writer.u8(static_cast<std::uint8_t>(entry.domain));
  writer.u8(static_cast<std::uint8_t>(entry.kind));
  writer.u8(static_cast<std::uint8_t>(entry.state));
  writer.u64(entry.generation.value());
  writer.u64(entry.resolution_evidence_generation.value());
  writer.u64(entry.recorded_at.value());
  writer.u64(entry.resolved_at.value());
  write_digest(writer, entry.detail_digest);
  writer.sized_string(entry.detail);
}

void write_request(ByteWriter& writer, const DrainRequest& request) {
  writer.u64(request.id.value());
  writer.u64(request.key.plan.value());
  writer.u8(static_cast<std::uint8_t>(request.key.domain));
  writer.u8(static_cast<std::uint8_t>(request.key.scope.kind));
  writer.u64(request.key.scope.id);
  write_digest(writer, request.key.obligation_digest);
  writer.u64(request.key.policy_generation.value());
  writer.u64(request.key.attempt.value());
  write_digest(writer, request.idempotency_key);
  writer.u8(static_cast<std::uint8_t>(request.state));
  writer.u64(request.staged_at.value());
  writer.u64(request.issued_at.value());
  writer.u32(request.bound_operations);
  writer.sized_string(request.target_system);
  writer.sized_string(request.instruction);
  writer.i64(request.staged_at_milliseconds);
  writer.i64(request.issued_at_milliseconds);
  writer.u64(request.acknowledged_at.value());
  writer.sized_string(request.acknowledgement_source);
  writer.u64(request.settled_at.value());
  writer.sized_string(request.settlement_detail);
}

void write_grant(ByteWriter& writer, const SafeToRemoveGrant& grant) {
  writer.u64(grant.plan.value());
  writer.u64(grant.revision.value());
  writer.u64(grant.epoch.value());
  write_generation_set(writer, grant.generations);
  write_digest(writer, grant.evidence_digest);
  write_digest(writer, grant.manifest_digest);
  writer.u64(grant.observation_floor.value());
  writer.u64(grant.granted_commit.value());
  writer.i64(grant.granted_at_milliseconds);
  writer.sized_string(grant.granted_by);
}

void write_fence(ByteWriter& writer, const FenceRecord& fence) {
  writer.u8(static_cast<std::uint8_t>(fence.reason));
  writer.u64(fence.floor.value());
  writer.u64(fence.revision.value());
  writer.u64(fence.epoch.value());
  writer.u64(fence.commit.value());
  writer.i64(fence.recorded_at_milliseconds);
  writer.sized_string(fence.detail);
}

void write_history_entry(ByteWriter& writer, const PlanHistoryEntry& entry) {
  writer.u8(static_cast<std::uint8_t>(entry.from_state));
  writer.u8(static_cast<std::uint8_t>(entry.to_state));
  writer.u8(static_cast<std::uint8_t>(entry.cause));
  writer.u64(entry.observed_at.value());
  writer.u64(entry.commit.value());
  writer.i64(entry.recorded_at_milliseconds);
  writer.sized_string(entry.detail);
}

[[nodiscard]] Status write_plan_record(ByteWriter& writer, const PlanRecord& plan, const Limits& limits) {
  const DrainPlanSpec& spec = plan.spec;
  writer.u64(spec.id.value());
  writer.u8(static_cast<std::uint8_t>(spec.scope.kind));
  writer.u64(spec.scope.id);

  const std::vector<DrainScope>& targets = spec.targets.targets();
  if (targets.size() > static_cast<std::size_t>(limits.max_targets_per_plan)) {
    return too_many("plan target count", targets.size(), limits.max_targets_per_plan);
  }
  writer.u32(static_cast<std::uint32_t>(targets.size()));
  for (const DrainScope& target : targets) {
    writer.u8(static_cast<std::uint8_t>(target.kind));
    writer.u64(target.id);
  }

  writer.u64(spec.bindings.facility_epoch.value());
  write_generation_set(writer, spec.bindings.generations);
  writer.u64(spec.bindings.policy_id.value());
  write_digest(writer, spec.bindings.policy_digest);
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const std::optional<ContentDigest>& digest = spec.bindings.domain_manifest_digests[index];
    writer.u8(flag_byte(digest.has_value()));
    if (digest.has_value()) {
      write_digest(writer, *digest);
    }
  }
  writer.u64(spec.revision.value());
  writer.u8(spec.declared_required_domains.bits());
  writer.sized_string(spec.label);
  writer.sized_string(spec.requested_by);
  writer.i64(spec.created_at_milliseconds);

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const std::vector<ConsumerRecord>& consumers = plan.planning_consumers[index];
    if (consumers.size() > static_cast<std::size_t>(limits.max_consumers_per_domain)) {
      return too_many("plan planning consumer count", consumers.size(), limits.max_consumers_per_domain);
    }
    writer.u32(static_cast<std::uint32_t>(consumers.size()));
    for (const ConsumerRecord& consumer : consumers) {
      write_consumer_record(writer, consumer);
    }

    const std::vector<EnumerationRecord>& enumerations = plan.enumerations[index];
    if (enumerations.size() > static_cast<std::size_t>(limits.max_evidence_per_domain)) {
      return too_many("plan enumeration count", enumerations.size(), limits.max_evidence_per_domain);
    }
    writer.u32(static_cast<std::uint32_t>(enumerations.size()));
    for (const EnumerationRecord& enumeration : enumerations) {
      const Status status = write_enumeration_record(writer, enumeration, limits);
      if (!status.ok()) {
        return status;
      }
    }

    const std::vector<CompletionEvidence>& completions = plan.completions[index];
    if (completions.size() > static_cast<std::size_t>(limits.max_evidence_per_domain)) {
      return too_many("plan completion count", completions.size(), limits.max_evidence_per_domain);
    }
    writer.u32(static_cast<std::uint32_t>(completions.size()));
    for (const CompletionEvidence& completion : completions) {
      write_completion_record(writer, completion);
    }
  }

  if (plan.residuals.entries.size() > static_cast<std::size_t>(limits.max_residuals_per_plan)) {
    return too_many("plan residual count", plan.residuals.entries.size(), limits.max_residuals_per_plan);
  }
  writer.u32(static_cast<std::uint32_t>(plan.residuals.entries.size()));
  for (const ResidualEntry& entry : plan.residuals.entries) {
    write_residual_entry(writer, entry);
  }

  if (plan.requests.size() > static_cast<std::size_t>(limits.max_requests_per_plan)) {
    return too_many("plan request count", plan.requests.size(), limits.max_requests_per_plan);
  }
  writer.u32(static_cast<std::uint32_t>(plan.requests.size()));
  for (const DrainRequest& request : plan.requests) {
    write_request(writer, request);
  }

  writer.u8(flag_byte(plan.grant.has_value()));
  if (plan.grant.has_value()) {
    write_grant(writer, *plan.grant);
  }
  writer.u8(flag_byte(plan.fence.has_value()));
  if (plan.fence.has_value()) {
    write_fence(writer, *plan.fence);
  }

  if (plan.history.size() > static_cast<std::size_t>(limits.max_history_per_plan)) {
    return too_many("plan history count", plan.history.size(), limits.max_history_per_plan);
  }
  writer.u32(static_cast<std::uint32_t>(plan.history.size()));
  for (const PlanHistoryEntry& entry : plan.history) {
    write_history_entry(writer, entry);
  }

  writer.u8(flag_byte(plan.cancelled));
  writer.sized_string(plan.cancellation_detail);
  writer.u8(flag_byte(plan.failed));
  writer.sized_string(plan.failure_detail);
  writer.u64(plan.last_observation.value());
  writer.u64(plan.last_commit.value());
  return Status::success();
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
//
// Every reader refuses rather than repairs. A record is built in a local and
// only assigned to the caller's output once it is complete, so a half decoded
// record can never be mistaken for a decoded one.

[[nodiscard]] Status read_consumer_record(ByteReader& reader, const Limits& limits, ConsumerRecord& out) {
  ConsumerRecord record{};
  std::uint64_t obligation = 0;
  if (!reader.u64(obligation)) {
    return truncated("consumer obligation");
  }
  record.obligation = ObligationId{obligation};

  std::uint8_t category = 0;
  if (!reader.u8(category)) {
    return truncated("consumer category");
  }
  if (!is_known_consumer_category(category)) {
    return unknown_enum_value("consumer category", category);
  }
  record.category = static_cast<ConsumerCategory>(category);

  std::uint64_t generation = 0;
  if (!reader.u64(generation)) {
    return truncated("consumer obligation generation");
  }
  record.generation = ObligationGeneration{generation};

  std::uint64_t reservation = 0;
  if (!reader.u64(reservation)) {
    return truncated("consumer reservation generation");
  }
  record.reservation = ReservationGeneration{reservation};

  std::uint8_t strength = 0;
  if (!reader.u8(strength)) {
    return truncated("consumer strength");
  }
  if (!is_known_obligation_strength(strength)) {
    return unknown_enum_value("consumer strength", strength);
  }
  record.strength = static_cast<ObligationStrength>(strength);

  Status status = read_text(reader, limits, TextPolicy::kIdentity, "consumer label", record.label);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kIdentity, "consumer source", record.source);
  if (!status.ok()) {
    return status;
  }
  out = std::move(record);
  return Status::success();
}

[[nodiscard]] Status read_enumeration_record(ByteReader& reader, const Limits& limits, EnumerationRecord& out) {
  EnumerationRecord record{};
  EnumerationEvidence& evidence = record.evidence;

  std::uint8_t domain = 0;
  if (!reader.u8(domain)) {
    return truncated("enumeration domain");
  }
  if (!is_known_owner_domain(domain)) {
    return unknown_enum_value("enumeration domain", domain);
  }
  evidence.domain = static_cast<OwnerDomain>(domain);

  std::uint8_t coverage = 0;
  if (!reader.u8(coverage)) {
    return truncated("enumeration coverage");
  }
  if (!is_known_coverage_state(coverage)) {
    return unknown_enum_value("enumeration coverage", coverage);
  }
  evidence.coverage = static_cast<CoverageState>(coverage);

  std::uint64_t generation = 0;
  if (!reader.u64(generation)) {
    return truncated("enumeration generation");
  }
  evidence.generation = EvidenceGeneration{generation};

  std::uint64_t observed_at = 0;
  if (!reader.u64(observed_at)) {
    return truncated("enumeration observed_at");
  }
  evidence.observed_at = ObservationSequence{observed_at};

  Status status = read_digest(reader, "enumeration manifest digest", evidence.manifest_digest);
  if (!status.ok()) {
    return status;
  }
  status = read_digest(reader, "enumeration scope manifest digest", evidence.scope_manifest_digest);
  if (!status.ok()) {
    return status;
  }
  status = read_generation_set(reader, "enumeration generation set", evidence.generations);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kIdentity, "enumeration source", evidence.source);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kAnnotation, "enumeration annotation", evidence.annotation);
  if (!status.ok()) {
    return status;
  }
  if (!reader.i64(evidence.observed_at_milliseconds)) {
    return truncated("enumeration observed_at_milliseconds");
  }

  std::uint32_t consumer_count = 0;
  if (!reader.u32(consumer_count)) {
    return truncated("enumeration consumer count");
  }
  if (consumer_count > limits.max_consumers_per_domain) {
    return too_many("enumeration consumer count", consumer_count, limits.max_consumers_per_domain);
  }
  for (std::uint32_t index = 0; index < consumer_count; ++index) {
    ConsumerRecord consumer{};
    status = read_consumer_record(reader, limits, consumer);
    if (!status.ok()) {
      return status;
    }
    record.consumers.push_back(std::move(consumer));
  }
  out = std::move(record);
  return Status::success();
}

[[nodiscard]] Status read_completion_record(ByteReader& reader, const Limits& limits, CompletionEvidence& out) {
  CompletionEvidence record{};
  std::uint64_t id = 0;
  if (!reader.u64(id)) {
    return truncated("completion id");
  }
  record.id = EvidenceId{id};

  std::uint8_t domain = 0;
  if (!reader.u8(domain)) {
    return truncated("completion domain");
  }
  if (!is_known_owner_domain(domain)) {
    return unknown_enum_value("completion domain", domain);
  }
  record.domain = static_cast<OwnerDomain>(domain);

  std::uint8_t state = 0;
  if (!reader.u8(state)) {
    return truncated("completion state");
  }
  if (!is_known_completion_state(state)) {
    return unknown_enum_value("completion state", state);
  }
  record.state = static_cast<CompletionState>(state);

  std::uint64_t generation = 0;
  if (!reader.u64(generation)) {
    return truncated("completion generation");
  }
  record.generation = EvidenceGeneration{generation};

  std::uint64_t observed_at = 0;
  if (!reader.u64(observed_at)) {
    return truncated("completion observed_at");
  }
  record.observed_at = ObservationSequence{observed_at};

  Status status = read_digest(reader, "completion payload digest", record.payload_digest);
  if (!status.ok()) {
    return status;
  }
  status = read_digest(reader, "completion manifest digest", record.manifest_digest);
  if (!status.ok()) {
    return status;
  }
  status = read_digest(reader, "completion scope manifest digest", record.scope_manifest_digest);
  if (!status.ok()) {
    return status;
  }
  status = read_generation_set(reader, "completion generation set", record.generations);
  if (!status.ok()) {
    return status;
  }
  status = read_bool(reader, "completion residual_count_known", record.residual_count_known);
  if (!status.ok()) {
    return status;
  }
  if (!reader.u64(record.residual_count)) {
    return truncated("completion residual_count");
  }
  status = read_text(reader, limits, TextPolicy::kIdentity, "completion source", record.source);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kAnnotation, "completion annotation", record.annotation);
  if (!status.ok()) {
    return status;
  }
  if (!reader.i64(record.observed_at_milliseconds)) {
    return truncated("completion observed_at_milliseconds");
  }
  out = std::move(record);
  return Status::success();
}

[[nodiscard]] Status read_residual_entry(ByteReader& reader, const Limits& limits, ResidualEntry& out) {
  ResidualEntry entry{};
  std::uint64_t obligation = 0;
  if (!reader.u64(obligation)) {
    return truncated("residual obligation");
  }
  entry.obligation = ObligationId{obligation};

  std::uint8_t domain = 0;
  if (!reader.u8(domain)) {
    return truncated("residual domain");
  }
  if (!is_known_owner_domain(domain)) {
    return unknown_enum_value("residual domain", domain);
  }
  entry.domain = static_cast<OwnerDomain>(domain);

  std::uint8_t kind = 0;
  if (!reader.u8(kind)) {
    return truncated("residual kind");
  }
  if (!is_known_residual_kind(kind)) {
    return unknown_enum_value("residual kind", kind);
  }
  entry.kind = static_cast<ResidualKind>(kind);

  std::uint8_t state = 0;
  if (!reader.u8(state)) {
    return truncated("residual state");
  }
  if (!is_known_residual_state(state)) {
    return unknown_enum_value("residual state", state);
  }
  entry.state = static_cast<ResidualState>(state);

  std::uint64_t generation = 0;
  if (!reader.u64(generation)) {
    return truncated("residual generation");
  }
  entry.generation = ObligationGeneration{generation};

  std::uint64_t resolution = 0;
  if (!reader.u64(resolution)) {
    return truncated("residual resolution evidence generation");
  }
  entry.resolution_evidence_generation = EvidenceGeneration{resolution};

  std::uint64_t recorded_at = 0;
  if (!reader.u64(recorded_at)) {
    return truncated("residual recorded_at");
  }
  entry.recorded_at = ObservationSequence{recorded_at};

  std::uint64_t resolved_at = 0;
  if (!reader.u64(resolved_at)) {
    return truncated("residual resolved_at");
  }
  entry.resolved_at = ObservationSequence{resolved_at};

  Status status = read_digest(reader, "residual detail digest", entry.detail_digest);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kAnnotation, "residual detail", entry.detail);
  if (!status.ok()) {
    return status;
  }
  out = std::move(entry);
  return Status::success();
}

[[nodiscard]] Status read_request(ByteReader& reader, const Limits& limits, DrainRequest& out) {
  DrainRequest request{};
  std::uint64_t id = 0;
  if (!reader.u64(id)) {
    return truncated("request id");
  }
  request.id = DrainRequestId{id};

  std::uint64_t plan = 0;
  if (!reader.u64(plan)) {
    return truncated("request key plan");
  }
  request.key.plan = PlanId{plan};

  std::uint8_t domain = 0;
  if (!reader.u8(domain)) {
    return truncated("request key domain");
  }
  if (!is_known_owner_domain(domain)) {
    return unknown_enum_value("request key domain", domain);
  }
  request.key.domain = static_cast<OwnerDomain>(domain);

  std::uint8_t scope_kind = 0;
  if (!reader.u8(scope_kind)) {
    return truncated("request key scope kind");
  }
  if (!is_known_scope_kind(scope_kind)) {
    return unknown_enum_value("request key scope kind", scope_kind);
  }
  request.key.scope.kind = static_cast<ScopeKind>(scope_kind);

  if (!reader.u64(request.key.scope.id)) {
    return truncated("request key scope id");
  }

  Status status = read_digest(reader, "request key obligation digest", request.key.obligation_digest);
  if (!status.ok()) {
    return status;
  }

  std::uint64_t policy_generation = 0;
  if (!reader.u64(policy_generation)) {
    return truncated("request key policy generation");
  }
  request.key.policy_generation = PolicyGeneration{policy_generation};

  std::uint64_t attempt = 0;
  if (!reader.u64(attempt)) {
    return truncated("request key attempt");
  }
  request.key.attempt = AttemptId{attempt};

  status = read_digest(reader, "request idempotency key", request.idempotency_key);
  if (!status.ok()) {
    return status;
  }

  std::uint8_t state = 0;
  if (!reader.u8(state)) {
    return truncated("request state");
  }
  if (!is_known_request_state(state)) {
    return unknown_enum_value("request state", state);
  }
  request.state = static_cast<RequestState>(state);

  std::uint64_t staged_at = 0;
  if (!reader.u64(staged_at)) {
    return truncated("request staged_at");
  }
  request.staged_at = ObservationSequence{staged_at};

  std::uint64_t issued_at = 0;
  if (!reader.u64(issued_at)) {
    return truncated("request issued_at");
  }
  request.issued_at = ObservationSequence{issued_at};

  if (!reader.u32(request.bound_operations)) {
    return truncated("request bound_operations");
  }

  status = read_text(reader, limits, TextPolicy::kIdentity, "request target_system", request.target_system);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kAnnotation, "request instruction", request.instruction);
  if (!status.ok()) {
    return status;
  }
  if (!reader.i64(request.staged_at_milliseconds)) {
    return truncated("request staged_at_milliseconds");
  }
  if (!reader.i64(request.issued_at_milliseconds)) {
    return truncated("request issued_at_milliseconds");
  }

  std::uint64_t acknowledged_at = 0;
  if (!reader.u64(acknowledged_at)) {
    return truncated("request acknowledged_at");
  }
  request.acknowledged_at = ObservationSequence{acknowledged_at};

  status = read_text(reader, limits, TextPolicy::kIdentity, "request acknowledgement_source",
                     request.acknowledgement_source);
  if (!status.ok()) {
    return status;
  }

  std::uint64_t settled_at = 0;
  if (!reader.u64(settled_at)) {
    return truncated("request settled_at");
  }
  request.settled_at = ObservationSequence{settled_at};

  status = read_text(reader, limits, TextPolicy::kAnnotation, "request settlement_detail", request.settlement_detail);
  if (!status.ok()) {
    return status;
  }
  out = std::move(request);
  return Status::success();
}

[[nodiscard]] Status read_grant(ByteReader& reader, const Limits& limits, SafeToRemoveGrant& out) {
  SafeToRemoveGrant grant{};
  std::uint64_t plan = 0;
  if (!reader.u64(plan)) {
    return truncated("grant plan");
  }
  grant.plan = PlanId{plan};

  std::uint64_t revision = 0;
  if (!reader.u64(revision)) {
    return truncated("grant revision");
  }
  grant.revision = Revision{revision};

  std::uint64_t epoch = 0;
  if (!reader.u64(epoch)) {
    return truncated("grant epoch");
  }
  grant.epoch = ControlEpoch{epoch};

  Status status = read_generation_set(reader, "grant generation set", grant.generations);
  if (!status.ok()) {
    return status;
  }
  status = read_digest(reader, "grant evidence digest", grant.evidence_digest);
  if (!status.ok()) {
    return status;
  }
  status = read_digest(reader, "grant manifest digest", grant.manifest_digest);
  if (!status.ok()) {
    return status;
  }

  std::uint64_t observation_floor = 0;
  if (!reader.u64(observation_floor)) {
    return truncated("grant observation_floor");
  }
  grant.observation_floor = ObservationSequence{observation_floor};

  std::uint64_t granted_commit = 0;
  if (!reader.u64(granted_commit)) {
    return truncated("grant granted_commit");
  }
  grant.granted_commit = CommitSequence{granted_commit};

  if (!reader.i64(grant.granted_at_milliseconds)) {
    return truncated("grant granted_at_milliseconds");
  }
  status = read_text(reader, limits, TextPolicy::kIdentity, "grant granted_by", grant.granted_by);
  if (!status.ok()) {
    return status;
  }
  out = std::move(grant);
  return Status::success();
}

[[nodiscard]] Status read_fence(ByteReader& reader, const Limits& limits, FenceRecord& out) {
  FenceRecord fence{};
  std::uint8_t reason = 0;
  if (!reader.u8(reason)) {
    return truncated("fence reason");
  }
  if (!is_known_fence_reason(reason)) {
    return unknown_enum_value("fence reason", reason);
  }
  fence.reason = static_cast<FenceReason>(reason);

  std::uint64_t floor = 0;
  if (!reader.u64(floor)) {
    return truncated("fence floor");
  }
  fence.floor = ObservationSequence{floor};

  std::uint64_t revision = 0;
  if (!reader.u64(revision)) {
    return truncated("fence revision");
  }
  fence.revision = Revision{revision};

  std::uint64_t epoch = 0;
  if (!reader.u64(epoch)) {
    return truncated("fence epoch");
  }
  fence.epoch = ControlEpoch{epoch};

  std::uint64_t commit = 0;
  if (!reader.u64(commit)) {
    return truncated("fence commit");
  }
  fence.commit = CommitSequence{commit};

  if (!reader.i64(fence.recorded_at_milliseconds)) {
    return truncated("fence recorded_at_milliseconds");
  }
  const Status status = read_text(reader, limits, TextPolicy::kAnnotation, "fence detail", fence.detail);
  if (!status.ok()) {
    return status;
  }
  out = std::move(fence);
  return Status::success();
}

[[nodiscard]] Status read_history_entry(ByteReader& reader, const Limits& limits, PlanHistoryEntry& out) {
  PlanHistoryEntry entry{};
  std::uint8_t from_state = 0;
  if (!reader.u8(from_state)) {
    return truncated("history from_state");
  }
  if (!is_known_drain_state(from_state)) {
    return unknown_enum_value("history from_state", from_state);
  }
  entry.from_state = static_cast<DrainState>(from_state);

  std::uint8_t to_state = 0;
  if (!reader.u8(to_state)) {
    return truncated("history to_state");
  }
  if (!is_known_drain_state(to_state)) {
    return unknown_enum_value("history to_state", to_state);
  }
  entry.to_state = static_cast<DrainState>(to_state);

  std::uint8_t cause = 0;
  if (!reader.u8(cause)) {
    return truncated("history cause");
  }
  if (!is_known_plan_operation(cause)) {
    return unknown_enum_value("history cause", cause);
  }
  entry.cause = static_cast<PlanOperation>(cause);

  std::uint64_t observed_at = 0;
  if (!reader.u64(observed_at)) {
    return truncated("history observed_at");
  }
  entry.observed_at = ObservationSequence{observed_at};

  std::uint64_t commit = 0;
  if (!reader.u64(commit)) {
    return truncated("history commit");
  }
  entry.commit = CommitSequence{commit};

  if (!reader.i64(entry.recorded_at_milliseconds)) {
    return truncated("history recorded_at_milliseconds");
  }
  const Status status = read_text(reader, limits, TextPolicy::kAnnotation, "history detail", entry.detail);
  if (!status.ok()) {
    return status;
  }
  out = std::move(entry);
  return Status::success();
}

[[nodiscard]] Status read_plan_record(ByteReader& reader, const Limits& limits, PlanRecord& out) {
  PlanRecord record{};
  DrainPlanSpec& spec = record.spec;

  std::uint64_t id = 0;
  if (!reader.u64(id)) {
    return truncated("plan id");
  }
  spec.id = PlanId{id};

  std::uint8_t scope_kind = 0;
  if (!reader.u8(scope_kind)) {
    return truncated("plan scope kind");
  }
  if (!is_known_scope_kind(scope_kind)) {
    return unknown_enum_value("plan scope kind", scope_kind);
  }
  spec.scope.kind = static_cast<ScopeKind>(scope_kind);
  if (!reader.u64(spec.scope.id)) {
    return truncated("plan scope id");
  }

  std::uint32_t target_count = 0;
  if (!reader.u32(target_count)) {
    return truncated("plan target count");
  }
  if (target_count > limits.max_targets_per_plan) {
    return too_many("plan target count", target_count, limits.max_targets_per_plan);
  }
  std::vector<DrainScope> targets;
  for (std::uint32_t index = 0; index < target_count; ++index) {
    DrainScope target{};
    std::uint8_t kind = 0;
    if (!reader.u8(kind)) {
      return truncated("plan target kind");
    }
    if (!is_known_scope_kind(kind)) {
      return unknown_enum_value("plan target kind", kind);
    }
    target.kind = static_cast<ScopeKind>(kind);
    if (!reader.u64(target.id)) {
      return truncated("plan target id");
    }
    targets.push_back(target);
  }
  // A manifest can only be built through create(), which is what enforces the
  // membership rules the rest of the library relies on. The decoded order must
  // already be the canonical one: create() canonicalises, so a payload whose
  // order it had to repair is not a payload this encoder produced.
  Result<DrainTargetManifest> manifest = DrainTargetManifest::create(targets, limits);
  if (!manifest) {
    std::string detail = "plan target manifest is not valid: ";
    detail += manifest.error().detail();
    return malformed(detail);
  }
  const std::vector<DrainScope>& canonical = manifest.value().targets();
  if (canonical.size() != targets.size()) {
    return malformed("plan target manifest was not canonical");
  }
  for (std::size_t index = 0; index < canonical.size(); ++index) {
    if (canonical[index] != targets[index]) {
      return malformed("plan target manifest is not in canonical order");
    }
  }
  spec.targets = std::move(manifest.value());

  std::uint64_t facility_epoch = 0;
  if (!reader.u64(facility_epoch)) {
    return truncated("plan facility epoch");
  }
  spec.bindings.facility_epoch = ControlEpoch{facility_epoch};

  Status status = read_generation_set(reader, "plan generation set", spec.bindings.generations);
  if (!status.ok()) {
    return status;
  }

  std::uint64_t policy_id = 0;
  if (!reader.u64(policy_id)) {
    return truncated("plan policy id");
  }
  spec.bindings.policy_id = PolicyId{policy_id};

  status = read_digest(reader, "plan policy digest", spec.bindings.policy_digest);
  if (!status.ok()) {
    return status;
  }

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    bool digest_present = false;
    status = read_bool(reader, "plan domain manifest digest presence", digest_present);
    if (!status.ok()) {
      return status;
    }
    if (!digest_present) {
      continue;
    }
    ContentDigest digest{};
    status = read_digest(reader, "plan domain manifest digest", digest);
    if (!status.ok()) {
      return status;
    }
    spec.bindings.domain_manifest_digests[index] = digest;
  }

  std::uint64_t revision = 0;
  if (!reader.u64(revision)) {
    return truncated("plan revision");
  }
  spec.revision = Revision{revision};

  std::uint8_t declared = 0;
  if (!reader.u8(declared)) {
    return truncated("plan declared required domains");
  }
  const std::optional<DomainMask> mask = DomainMask::from_bits(declared);
  if (!mask.has_value()) {
    return unknown_enum_value("plan declared required domains", declared);
  }
  spec.declared_required_domains = *mask;

  status = read_text(reader, limits, TextPolicy::kIdentity, "plan label", spec.label);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kIdentity, "plan requested_by", spec.requested_by);
  if (!status.ok()) {
    return status;
  }
  if (!reader.i64(spec.created_at_milliseconds)) {
    return truncated("plan created_at_milliseconds");
  }

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    std::uint32_t planning_count = 0;
    if (!reader.u32(planning_count)) {
      return truncated("plan planning consumer count");
    }
    if (planning_count > limits.max_consumers_per_domain) {
      return too_many("plan planning consumer count", planning_count, limits.max_consumers_per_domain);
    }
    for (std::uint32_t item = 0; item < planning_count; ++item) {
      ConsumerRecord consumer{};
      status = read_consumer_record(reader, limits, consumer);
      if (!status.ok()) {
        return status;
      }
      record.planning_consumers[index].push_back(std::move(consumer));
    }

    std::uint32_t enumeration_count = 0;
    if (!reader.u32(enumeration_count)) {
      return truncated("plan enumeration count");
    }
    if (enumeration_count > limits.max_evidence_per_domain) {
      return too_many("plan enumeration count", enumeration_count, limits.max_evidence_per_domain);
    }
    for (std::uint32_t item = 0; item < enumeration_count; ++item) {
      EnumerationRecord enumeration{};
      status = read_enumeration_record(reader, limits, enumeration);
      if (!status.ok()) {
        return status;
      }
      record.enumerations[index].push_back(std::move(enumeration));
    }

    std::uint32_t completion_count = 0;
    if (!reader.u32(completion_count)) {
      return truncated("plan completion count");
    }
    if (completion_count > limits.max_evidence_per_domain) {
      return too_many("plan completion count", completion_count, limits.max_evidence_per_domain);
    }
    for (std::uint32_t item = 0; item < completion_count; ++item) {
      CompletionEvidence completion{};
      status = read_completion_record(reader, limits, completion);
      if (!status.ok()) {
        return status;
      }
      record.completions[index].push_back(std::move(completion));
    }
  }

  std::uint32_t residual_count = 0;
  if (!reader.u32(residual_count)) {
    return truncated("plan residual count");
  }
  if (residual_count > limits.max_residuals_per_plan) {
    return too_many("plan residual count", residual_count, limits.max_residuals_per_plan);
  }
  for (std::uint32_t index = 0; index < residual_count; ++index) {
    ResidualEntry entry{};
    status = read_residual_entry(reader, limits, entry);
    if (!status.ok()) {
      return status;
    }
    record.residuals.entries.push_back(std::move(entry));
  }

  std::uint32_t request_count = 0;
  if (!reader.u32(request_count)) {
    return truncated("plan request count");
  }
  if (request_count > limits.max_requests_per_plan) {
    return too_many("plan request count", request_count, limits.max_requests_per_plan);
  }
  for (std::uint32_t index = 0; index < request_count; ++index) {
    DrainRequest request{};
    status = read_request(reader, limits, request);
    if (!status.ok()) {
      return status;
    }
    record.requests.push_back(std::move(request));
  }

  bool present = false;
  status = read_bool(reader, "plan grant presence", present);
  if (!status.ok()) {
    return status;
  }
  if (present) {
    SafeToRemoveGrant grant{};
    status = read_grant(reader, limits, grant);
    if (!status.ok()) {
      return status;
    }
    record.grant = std::move(grant);
  }

  status = read_bool(reader, "plan fence presence", present);
  if (!status.ok()) {
    return status;
  }
  if (present) {
    FenceRecord fence{};
    status = read_fence(reader, limits, fence);
    if (!status.ok()) {
      return status;
    }
    record.fence = std::move(fence);
  }

  std::uint32_t history_count = 0;
  if (!reader.u32(history_count)) {
    return truncated("plan history count");
  }
  if (history_count > limits.max_history_per_plan) {
    return too_many("plan history count", history_count, limits.max_history_per_plan);
  }
  for (std::uint32_t index = 0; index < history_count; ++index) {
    PlanHistoryEntry entry{};
    status = read_history_entry(reader, limits, entry);
    if (!status.ok()) {
      return status;
    }
    record.history.push_back(std::move(entry));
  }

  status = read_bool(reader, "plan cancelled", record.cancelled);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kAnnotation, "plan cancellation_detail", record.cancellation_detail);
  if (!status.ok()) {
    return status;
  }
  status = read_bool(reader, "plan failed", record.failed);
  if (!status.ok()) {
    return status;
  }
  status = read_text(reader, limits, TextPolicy::kAnnotation, "plan failure_detail", record.failure_detail);
  if (!status.ok()) {
    return status;
  }

  std::uint64_t last_observation = 0;
  if (!reader.u64(last_observation)) {
    return truncated("plan last_observation");
  }
  record.last_observation = ObservationSequence{last_observation};

  std::uint64_t last_commit = 0;
  if (!reader.u64(last_commit)) {
    return truncated("plan last_commit");
  }
  record.last_commit = CommitSequence{last_commit};

  out = std::move(record);
  return Status::success();
}

// ---------------------------------------------------------------------------
// Enumeration membership
// ---------------------------------------------------------------------------
//
// One exhaustive predicate per enumeration field. Exhaustive switches are used
// deliberately: adding a value to an enumeration forces an edit here instead of
// silently widening what a store will accept.

[[nodiscard]] bool is_known_domain(OwnerDomain domain) noexcept {
  switch (domain) {
    case OwnerDomain::kAsi:
    case OwnerDomain::kDfi:
    case OwnerDomain::kFacility:
    case OwnerDomain::kMonitoring:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_category(ConsumerCategory category) noexcept {
  switch (category) {
    case ConsumerCategory::kWorkload:
    case ConsumerCategory::kAcceleratorReservation:
    case ConsumerCategory::kNetworkPath:
    case ConsumerCategory::kNetworkAttachment:
    case ConsumerCategory::kServiceClass:
    case ConsumerCategory::kMaintenanceProtection:
    case ConsumerCategory::kFacilityReservation:
    case ConsumerCategory::kMonitoringDependency:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_strength(ObligationStrength strength) noexcept {
  switch (strength) {
    case ObligationStrength::kMandatory:
    case ObligationStrength::kAdvisory:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_scope_kind(ScopeKind kind) noexcept {
  switch (kind) {
    case ScopeKind::kAsset:
    case ScopeKind::kRack:
    case ScopeKind::kZone:
    case ScopeKind::kSubscope:
    case ScopeKind::kSite:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_coverage(CoverageState coverage) noexcept {
  switch (coverage) {
    case CoverageState::kNotEnumerated:
    case CoverageState::kPartial:
    case CoverageState::kComplete:
    case CoverageState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_completion_state(CompletionState state) noexcept {
  switch (state) {
    case CompletionState::kUnknown:
    case CompletionState::kRequested:
    case CompletionState::kAcknowledged:
    case CompletionState::kDraining:
    case CompletionState::kDrainedWithResiduals:
    case CompletionState::kDrained:
    case CompletionState::kRefused:
    case CompletionState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_residual_kind(ResidualKind kind) noexcept {
  switch (kind) {
    case ResidualKind::kObligationActive:
    case ResidualKind::kObligationUnknown:
    case ResidualKind::kOwnerRefused:
    case ResidualKind::kEnumerationIncomplete:
    case ResidualKind::kEvidenceMissing:
    case ResidualKind::kResidualCountUnknown:
    case ResidualKind::kRequestUnacknowledged:
    case ResidualKind::kDomainFailed:
    case ResidualKind::kEvidenceStale:
    case ResidualKind::kProtectedObligation:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_residual_state(ResidualState state) noexcept {
  switch (state) {
    case ResidualState::kOpen:
    case ResidualState::kRelinquished:
    case ResidualState::kSuperseded:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_request_state(RequestState state) noexcept {
  switch (state) {
    case RequestState::kStaged:
    case RequestState::kIssued:
    case RequestState::kAcknowledged:
    case RequestState::kCompleted:
    case RequestState::kRefused:
    case RequestState::kFailed:
    case RequestState::kSuperseded:
    case RequestState::kCancelled:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_drain_state(DrainState state) noexcept {
  switch (state) {
    case DrainState::kProposed:
    case DrainState::kEnumerating:
    case DrainState::kRequested:
    case DrainState::kDraining:
    case DrainState::kResidualsPresent:
    case DrainState::kDrained:
    case DrainState::kSafeToRemove:
    case DrainState::kCancelled:
    case DrainState::kFailed:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_plan_operation(PlanOperation operation) noexcept {
  switch (operation) {
    case PlanOperation::kRevise:
    case PlanOperation::kRecordEnumeration:
    case PlanOperation::kIssueRequests:
    case PlanOperation::kRecordAcknowledgement:
    case PlanOperation::kIngestCompletion:
    case PlanOperation::kRecordResidual:
    case PlanOperation::kResolveResidual:
    case PlanOperation::kEvaluate:
    case PlanOperation::kGrant:
    case PlanOperation::kFence:
    case PlanOperation::kCancel:
    case PlanOperation::kFail:
    case PlanOperation::kSupersedeRequest:
      return true;
  }
  return false;
}

[[nodiscard]] bool is_known_fence_reason(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::kNone:
    case FenceReason::kRestart:
    case FenceReason::kControlEpochChanged:
    case FenceReason::kNewObligation:
    case FenceReason::kPlanRevised:
    case FenceReason::kEvidenceSuperseded:
    case FenceReason::kScopeManifestChanged:
    case FenceReason::kOperatorFence:
    case FenceReason::kPlanCancelled:
    case FenceReason::kPlanFailed:
    case FenceReason::kDependencyChange:
      return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
//
// The payload records the limits it was written under, and a caller must supply
// bounds that are at least as generous. A caller whose bounds are tighter than
// the recorded ones would have to refuse records the writer legitimately
// published, and adopting the payload under tighter bounds is a rescaling, and
// a rescaling is a refusal. A caller with looser bounds is accepted, because
// every record that was published still fits inside a bound that is larger.

[[nodiscard]] std::string_view first_limits_excess(const Limits& recorded, const Limits& requested) noexcept {
  if (recorded.max_plans > requested.max_plans) {
    return "max_plans";
  }
  if (recorded.max_targets_per_plan > requested.max_targets_per_plan) {
    return "max_targets_per_plan";
  }
  if (recorded.max_consumers_per_domain > requested.max_consumers_per_domain) {
    return "max_consumers_per_domain";
  }
  if (recorded.max_evidence_per_domain > requested.max_evidence_per_domain) {
    return "max_evidence_per_domain";
  }
  if (recorded.max_requests_per_plan > requested.max_requests_per_plan) {
    return "max_requests_per_plan";
  }
  if (recorded.max_residuals_per_plan > requested.max_residuals_per_plan) {
    return "max_residuals_per_plan";
  }
  if (recorded.max_history_per_plan > requested.max_history_per_plan) {
    return "max_history_per_plan";
  }
  if (recorded.max_text_bytes > requested.max_text_bytes) {
    return "max_text_bytes";
  }
  if (recorded.max_annotation_bytes > requested.max_annotation_bytes) {
    return "max_annotation_bytes";
  }
  if (recorded.max_state_bytes > requested.max_state_bytes) {
    return "max_state_bytes";
  }
  if (recorded.max_document_bytes > requested.max_document_bytes) {
    return "max_document_bytes";
  }
  if (recorded.max_attempts_per_key > requested.max_attempts_per_key) {
    return "max_attempts_per_key";
  }
  return {};
}

[[nodiscard]] Status check_recorded_limits(const Limits& recorded, const Limits& requested) {
  const std::string_view field = first_limits_excess(recorded, requested);
  if (field.empty()) {
    return Status::success();
  }
  std::string detail = "the recorded limits exceed the requested limits at ";
  detail.append(field);
  return Status::failure(ErrorCode::kLimitExceeded, detail);
}

void write_limits(ByteWriter& writer, const Limits& limits) {
  writer.u32(limits.max_plans);
  writer.u32(limits.max_targets_per_plan);
  writer.u32(limits.max_consumers_per_domain);
  writer.u32(limits.max_evidence_per_domain);
  writer.u32(limits.max_requests_per_plan);
  writer.u32(limits.max_residuals_per_plan);
  writer.u32(limits.max_history_per_plan);
  writer.u32(limits.max_text_bytes);
  writer.u32(limits.max_annotation_bytes);
  writer.u64(limits.max_state_bytes);
  writer.u64(limits.max_document_bytes);
  writer.u32(limits.max_attempts_per_key);
}

[[nodiscard]] Status read_limits(ByteReader& reader, Limits& out) {
  Limits limits{};
  if (!reader.u32(limits.max_plans)) {
    return truncated("limits max_plans");
  }
  if (!reader.u32(limits.max_targets_per_plan)) {
    return truncated("limits max_targets_per_plan");
  }
  if (!reader.u32(limits.max_consumers_per_domain)) {
    return truncated("limits max_consumers_per_domain");
  }
  if (!reader.u32(limits.max_evidence_per_domain)) {
    return truncated("limits max_evidence_per_domain");
  }
  if (!reader.u32(limits.max_requests_per_plan)) {
    return truncated("limits max_requests_per_plan");
  }
  if (!reader.u32(limits.max_residuals_per_plan)) {
    return truncated("limits max_residuals_per_plan");
  }
  if (!reader.u32(limits.max_history_per_plan)) {
    return truncated("limits max_history_per_plan");
  }
  if (!reader.u32(limits.max_text_bytes)) {
    return truncated("limits max_text_bytes");
  }
  if (!reader.u32(limits.max_annotation_bytes)) {
    return truncated("limits max_annotation_bytes");
  }
  if (!reader.u64(limits.max_state_bytes)) {
    return truncated("limits max_state_bytes");
  }
  if (!reader.u64(limits.max_document_bytes)) {
    return truncated("limits max_document_bytes");
  }
  if (!reader.u32(limits.max_attempts_per_key)) {
    return truncated("limits max_attempts_per_key");
  }
  out = limits;
  return Status::success();
}

// ---------------------------------------------------------------------------
// Text validation
// ---------------------------------------------------------------------------
//
// The validator walks every string the encoder writes, with the same policy the
// reader applies, so a state that passes validation is a state whose text the
// codec can always reproduce byte for byte.

[[nodiscard]] Status validate_consumer_texts(const ConsumerRecord& record, const Limits& limits) {
  Status status = validate_field(record.label, "label", limits, TextPolicy::kIdentity);
  if (!status.ok()) {
    return status;
  }
  return validate_field(record.source, "source", limits, TextPolicy::kIdentity);
}

[[nodiscard]] Status validate_plan_texts(const PlanRecord& plan, const Limits& limits) {
  Status status = validate_field(plan.spec.label, "plan label", limits, TextPolicy::kIdentity);
  if (!status.ok()) {
    return status;
  }
  status = validate_field(plan.spec.requested_by, "plan requested_by", limits, TextPolicy::kIdentity);
  if (!status.ok()) {
    return status;
  }
  status = validate_field(plan.cancellation_detail, "plan cancellation_detail", limits, TextPolicy::kAnnotation);
  if (!status.ok()) {
    return status;
  }
  status = validate_field(plan.failure_detail, "plan failure_detail", limits, TextPolicy::kAnnotation);
  if (!status.ok()) {
    return status;
  }

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    for (const ConsumerRecord& consumer : plan.planning_consumers[index]) {
      status = validate_consumer_texts(consumer, limits);
      if (!status.ok()) {
        return status;
      }
    }
    for (const EnumerationRecord& enumeration : plan.enumerations[index]) {
      status = validate_field(enumeration.evidence.source, "source", limits, TextPolicy::kIdentity);
      if (!status.ok()) {
        return status;
      }
      status = validate_field(enumeration.evidence.annotation, "annotation", limits, TextPolicy::kAnnotation);
      if (!status.ok()) {
        return status;
      }
      for (const ConsumerRecord& consumer : enumeration.consumers) {
        status = validate_consumer_texts(consumer, limits);
        if (!status.ok()) {
          return status;
        }
      }
    }
    for (const CompletionEvidence& completion : plan.completions[index]) {
      status = validate_field(completion.source, "source", limits, TextPolicy::kIdentity);
      if (!status.ok()) {
        return status;
      }
      status = validate_field(completion.annotation, "annotation", limits, TextPolicy::kAnnotation);
      if (!status.ok()) {
        return status;
      }
    }
  }

  for (const ResidualEntry& entry : plan.residuals.entries) {
    status = validate_field(entry.detail, "detail", limits, TextPolicy::kAnnotation);
    if (!status.ok()) {
      return status;
    }
  }

  for (const DrainRequest& request : plan.requests) {
    status = validate_field(request.target_system, "target_system", limits, TextPolicy::kIdentity);
    if (!status.ok()) {
      return status;
    }
    status = validate_field(request.instruction, "instruction", limits, TextPolicy::kAnnotation);
    if (!status.ok()) {
      return status;
    }
    status = validate_field(request.acknowledgement_source, "acknowledgement_source", limits, TextPolicy::kIdentity);
    if (!status.ok()) {
      return status;
    }
    status = validate_field(request.settlement_detail, "settlement_detail", limits, TextPolicy::kAnnotation);
    if (!status.ok()) {
      return status;
    }
  }

  if (plan.grant.has_value()) {
    status = validate_field(plan.grant->granted_by, "grant granted_by", limits, TextPolicy::kIdentity);
    if (!status.ok()) {
      return status;
    }
  }
  if (plan.fence.has_value()) {
    status = validate_field(plan.fence->detail, "fence detail", limits, TextPolicy::kAnnotation);
    if (!status.ok()) {
      return status;
    }
  }
  for (const PlanHistoryEntry& entry : plan.history) {
    status = validate_field(entry.detail, "history detail", limits, TextPolicy::kAnnotation);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

Status encode_state(const CoordinatorState& state, const Limits& limits, std::vector<std::byte>& out) {
  // Validate first: a store must never contain a payload that its own build
  // cannot read back, so an inconsistent state is refused before a single byte
  // is written.
  const Status validation = validate_state(state, limits);
  if (!validation.ok()) {
    return validation;
  }
  // The header records state.limits, and decode_state accepts exactly those
  // limits again, so a state whose recorded limits disagree with the limits it
  // is being encoded under would produce a payload nothing could read.
  const Status limits_status = check_recorded_limits(state.limits, limits);
  if (!limits_status.ok()) {
    return limits_status;
  }

  ByteWriter writer;
  writer.u32(state.payload_version);
  // The reserved field keeps the fixed header width stable. It is written as
  // zero and refused on read when it is anything else, so a future version can
  // add a header field without an old reader mistaking the new bytes for data.
  writer.u32(0);
  write_limits(writer, state.limits);
  writer.u64(state.control_epoch.value());
  writer.u64(state.incarnation.value());
  writer.u64(state.commit_sequence.value());
  writer.u64(state.observation_sequence.value());
  writer.i64(state.created_at_milliseconds);
  writer.i64(state.updated_at_milliseconds);

  if (state.plans.size() > static_cast<std::size_t>(limits.max_plans)) {
    return too_many("plan count", state.plans.size(), limits.max_plans);
  }
  writer.u32(static_cast<std::uint32_t>(state.plans.size()));
  for (const PlanRecord& plan : state.plans) {
    const Status status = write_plan_record(writer, plan, limits);
    if (!status.ok()) {
      return status;
    }
  }

  // The caller's buffer is assigned only now, from a complete payload, so a
  // partial encode can never be mistaken for a durable state.
  out = std::move(writer).take();
  return Status::success();
}

Result<CoordinatorState> decode_state(std::span<const std::byte> bytes, const Limits& limits) {
  ByteReader reader{bytes};
  CoordinatorState state{};

  std::uint32_t payload_version = 0;
  if (!reader.u32(payload_version)) {
    return rejected(truncated("payload version"));
  }
  if (payload_version != kStateFormatVersion) {
    std::string detail = "state payload version ";
    detail += std::to_string(payload_version);
    detail += " is not supported";
    return rejected(Status::failure(ErrorCode::kStoreVersionUnsupported, detail));
  }
  state.payload_version = payload_version;

  std::uint32_t reserved = 0;
  if (!reader.u32(reserved)) {
    return rejected(truncated("reserved field"));
  }
  if (reserved != 0) {
    return rejected(Status::failure(ErrorCode::kReservedNotZero, "the reserved header field is not zero"));
  }

  Status status = read_limits(reader, state.limits);
  if (!status.ok()) {
    return rejected(status);
  }
  // Checked before any content is read: the bounds a payload declares are what
  // every count and length in it is measured against, so a reader that does not
  // share them refuses the payload rather than reinterpreting its numbers.
  status = check_recorded_limits(state.limits, limits);
  if (!status.ok()) {
    return rejected(status);
  }

  std::uint64_t control_epoch = 0;
  if (!reader.u64(control_epoch)) {
    return rejected(truncated("control epoch"));
  }
  state.control_epoch = ControlEpoch{control_epoch};

  std::uint64_t incarnation = 0;
  if (!reader.u64(incarnation)) {
    return rejected(truncated("incarnation"));
  }
  state.incarnation = IncarnationId{incarnation};

  std::uint64_t commit_sequence = 0;
  if (!reader.u64(commit_sequence)) {
    return rejected(truncated("commit sequence"));
  }
  state.commit_sequence = CommitSequence{commit_sequence};

  std::uint64_t observation_sequence = 0;
  if (!reader.u64(observation_sequence)) {
    return rejected(truncated("observation sequence"));
  }
  state.observation_sequence = ObservationSequence{observation_sequence};

  if (!reader.i64(state.created_at_milliseconds)) {
    return rejected(truncated("created_at_milliseconds"));
  }
  if (!reader.i64(state.updated_at_milliseconds)) {
    return rejected(truncated("updated_at_milliseconds"));
  }

  std::uint32_t plan_count = 0;
  if (!reader.u32(plan_count)) {
    return rejected(truncated("plan count"));
  }
  if (plan_count > limits.max_plans) {
    return rejected(too_many("plan count", plan_count, limits.max_plans));
  }
  for (std::uint32_t index = 0; index < plan_count; ++index) {
    PlanRecord plan{};
    status = read_plan_record(reader, limits, plan);
    if (!status.ok()) {
      return rejected(status);
    }
    state.plans.push_back(std::move(plan));
  }

  if (!reader.at_end()) {
    return rejected(Status::failure(ErrorCode::kStoreTrailingBytes,
                                    "the payload continues past the last field of the state"));
  }

  // The decoder validates after reading, exactly as the encoder validates
  // before writing, so a state that violates an invariant can neither be
  // written nor adopted.
  status = validate_state(state, limits);
  if (!status.ok()) {
    return rejected(status);
  }
  return Result<CoordinatorState>{std::move(state)};
}

Status validate_state(const CoordinatorState& state, const Limits& limits) {
  // 1. The payload version is part of the state, not just of the container: a
  //    state that claims a version this build cannot read is never validated as
  //    if it were this build's version.
  if (state.payload_version != kStateFormatVersion) {
    std::string detail = "state payload version ";
    detail += std::to_string(state.payload_version);
    detail += " is not supported";
    return Status::failure(ErrorCode::kStoreVersionUnsupported, detail);
  }

  // 2. The configured bounds are validated first: every later check measures a
  //    record against them, so nonsense bounds must not be used to measure.
  const Status limits_status = limits.validate();
  if (!limits_status.ok()) {
    return limits_status;
  }

  // The invariant order below is part of the contract: one inconsistent state
  // must always report the same primary code, so each invariant is checked
  // across every plan before the next invariant begins.

  // 3. Plan ids strictly increasing.
  for (std::size_t index = 1; index < state.plans.size(); ++index) {
    if (!(state.plans[index - 1].spec.id < state.plans[index].spec.id)) {
      return malformed("plan ids are not strictly increasing");
    }
  }

  // 4. Every plan revision at least one.
  for (const PlanRecord& plan : state.plans) {
    if (plan.spec.revision.value() == 0) {
      return malformed(plan_name(plan) + " has revision 0, and the first revision is 1");
    }
  }

  // 5. Every plan requires at least one domain to be proven.
  for (const PlanRecord& plan : state.plans) {
    if (plan.spec.declared_required_domains.empty()) {
      return malformed(plan_name(plan) + " declares no required domain");
    }
  }

  // 6. Every target manifest is non empty, canonical, within the bound, and
  //    contains the plan's own scope.
  for (const PlanRecord& plan : state.plans) {
    const std::vector<DrainScope>& targets = plan.spec.targets.targets();
    const std::string name = plan_name(plan);
    if (targets.empty()) {
      return malformed(name + " has an empty target manifest");
    }
    if (targets.size() > static_cast<std::size_t>(limits.max_targets_per_plan)) {
      return too_many(name + " target manifest", targets.size(), limits.max_targets_per_plan);
    }
    for (std::size_t index = 1; index < targets.size(); ++index) {
      if (!(targets[index - 1] < targets[index])) {
        return malformed(name + " target manifest is not in canonical order");
      }
    }
    if (!plan.spec.targets.contains(plan.spec.scope)) {
      return malformed(name + " target manifest does not contain its scope");
    }
  }

  // 7. No bound consumer manifest digest is zero: a zero digest means no digest
  //    was recorded, and treating it as a manifest would bind the plan to
  //    nothing while looking like it was bound to something.
  for (const PlanRecord& plan : state.plans) {
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      const std::optional<ContentDigest>& digest = plan.spec.bindings.domain_manifest_digests[index];
      if (digest.has_value() && digest->is_zero()) {
        return malformed(plan_name(plan) + " binds a zero consumer manifest digest");
      }
    }
  }

  // 8. Per domain, evidence generations strictly increase: a record that does
  //    not advance the newest generation is stale, and keeping it would let an
  //    old observation outlive the one that superseded it.
  for (const PlanRecord& plan : state.plans) {
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      const std::vector<EnumerationRecord>& enumerations = plan.enumerations[index];
      for (std::size_t item = 1; item < enumerations.size(); ++item) {
        if (!(enumerations[item - 1].evidence.generation < enumerations[item].evidence.generation)) {
          return malformed(plan_name(plan) + " enumeration generations do not strictly increase");
        }
      }
      const std::vector<CompletionEvidence>& completions = plan.completions[index];
      for (std::size_t item = 1; item < completions.size(); ++item) {
        if (!(completions[item - 1].generation < completions[item].generation)) {
          return malformed(plan_name(plan) + " completion generations do not strictly increase");
        }
      }
    }
  }

  // 9. The residual ledger is in canonical order with no repeated identity.
  for (const PlanRecord& plan : state.plans) {
    const std::vector<ResidualEntry>& entries = plan.residuals.entries;
    for (std::size_t index = 1; index < entries.size(); ++index) {
      // Identity first: two entries for one (domain, obligation, kind) are the
      // same ledger entry recorded twice however their other fields differ, and
      // the canonical order keeps such a pair adjacent.
      if (entries[index - 1] == entries[index]) {
        return malformed(plan_name(plan) + " residual ledger repeats an entry identity");
      }
      if (!(entries[index - 1] < entries[index])) {
        return malformed(plan_name(plan) + " residual ledger is not in canonical order");
      }
    }
  }

  // 10. Requests are in canonical order with no duplicate id.
  for (const PlanRecord& plan : state.plans) {
    const std::vector<DrainRequest>& requests = plan.requests;
    for (std::size_t index = 1; index < requests.size(); ++index) {
      if (!(requests[index - 1] < requests[index])) {
        return malformed(plan_name(plan) + " requests are not in canonical order");
      }
    }
    if (requests.size() > 1) {
      // The canonical order is what a request is about, not its id, so a
      // duplicate id is found by sorting the ids rather than by comparing
      // neighbours. The id is the handle a caller looks a request up by, and
      // two records answering to one handle would be indistinguishable.
      std::vector<DrainRequestId> ids;
      ids.reserve(requests.size());
      for (const DrainRequest& request : requests) {
        ids.push_back(request.id);
      }
      std::sort(ids.begin(), ids.end());
      for (std::size_t index = 1; index < ids.size(); ++index) {
        if (!(ids[index - 1] < ids[index])) {
          return malformed(plan_name(plan) + " records a duplicate request id");
        }
      }
    }
  }

  // 11. History within the audit trail bound.
  for (const PlanRecord& plan : state.plans) {
    if (plan.history.size() > static_cast<std::size_t>(limits.max_history_per_plan)) {
      return too_many(plan_name(plan) + " history", plan.history.size(), limits.max_history_per_plan);
    }
  }

  // 12. Requests within the per plan bound.
  for (const PlanRecord& plan : state.plans) {
    if (plan.requests.size() > static_cast<std::size_t>(limits.max_requests_per_plan)) {
      return too_many(plan_name(plan) + " requests", plan.requests.size(), limits.max_requests_per_plan);
    }
  }

  // 13. Every string satisfies the same text bounds the API enforces, so text
  //     that was refused at the boundary cannot enter through the store.
  for (const PlanRecord& plan : state.plans) {
    const Status status = validate_plan_texts(plan, limits);
    if (!status.ok()) {
      return status;
    }
  }

  // 14. Every enumeration field holds a value from its defined set.
  //
  //     This is the last line of defence against publishing a payload that
  //     cannot be read back. A value outside its set selects a branch and
  //     indexes tables, so a state carrying one is not merely odd: it is a state
  //     whose meaning is undefined, and a store that accepted it would be
  //     unreadable by the very build that wrote it.
  for (const PlanRecord& plan : state.plans) {
    if (!is_known_scope_kind(plan.spec.scope.kind)) {
      return malformed(plan_name(plan) + " target scope kind is outside the defined kinds");
    }
    for (const DrainScope& target : plan.spec.targets.targets()) {
      if (!is_known_scope_kind(target.kind)) {
        return malformed(plan_name(plan) + " target manifest holds an unknown scope kind");
      }
    }
    if (plan.fence.has_value() && !is_known_fence_reason(plan.fence->reason)) {
      return malformed(plan_name(plan) + " fence reason is outside the defined reasons");
    }
    for (const PlanHistoryEntry& entry : plan.history) {
      if (!is_known_drain_state(entry.from_state) || !is_known_drain_state(entry.to_state)) {
        return malformed(plan_name(plan) + " history records an unknown drain state");
      }
      if (!is_known_plan_operation(entry.cause)) {
        return malformed(plan_name(plan) + " history records an unknown operation");
      }
    }
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      for (const ConsumerRecord& consumer : plan.planning_consumers[index]) {
        if (!is_known_category(consumer.category) || !is_known_strength(consumer.strength)) {
          return malformed(plan_name(plan) + " planning manifest holds an unknown consumer category");
        }
      }
      for (const EnumerationRecord& enumeration : plan.enumerations[index]) {
        if (!is_known_domain(enumeration.evidence.domain) || !is_known_coverage(enumeration.evidence.coverage)) {
          return malformed(plan_name(plan) + " holds an enumeration with an unknown domain or coverage");
        }
        for (const ConsumerRecord& consumer : enumeration.consumers) {
          if (!is_known_category(consumer.category) || !is_known_strength(consumer.strength)) {
            return malformed(plan_name(plan) + " enumeration holds an unknown consumer category");
          }
        }
      }
      for (const CompletionEvidence& completion : plan.completions[index]) {
        if (!is_known_domain(completion.domain) || !is_known_completion_state(completion.state)) {
          return malformed(plan_name(plan) + " holds a completion with an unknown domain or state");
        }
      }
    }
    for (const ResidualEntry& entry : plan.residuals.entries) {
      if (!is_known_domain(entry.domain) || !is_known_residual_kind(entry.kind) ||
          !is_known_residual_state(entry.state)) {
        return malformed(plan_name(plan) + " residual ledger holds an unknown domain, kind or state");
      }
    }
    for (const DrainRequest& request : plan.requests) {
      if (!is_known_domain(request.key.domain) || !is_known_scope_kind(request.key.scope.kind) ||
          !is_known_request_state(request.state)) {
        return malformed(plan_name(plan) + " holds a request with an unknown domain, scope or state");
      }
    }
  }

  return Status::success();
}

}  // namespace detail
}  // namespace facilitydrain
