// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// This file tests the internal byte and state codecs. It includes them by
// relative path because they are not part of the installed public surface.

#include "test_harness.hpp"

#include "../src/byte_codec.hpp"
#include "../src/state_codec.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using facilitydrain::CommitSequence;
using facilitydrain::ContentDigest;
using facilitydrain::ControlEpoch;
using facilitydrain::DrainScope;
using facilitydrain::DrainTargetManifest;
using facilitydrain::ErrorCode;
using facilitydrain::GenerationSet;
using facilitydrain::IncarnationId;
using facilitydrain::Limits;
using facilitydrain::ObservationSequence;
using facilitydrain::OwnerDomain;
using facilitydrain::PlanId;
using facilitydrain::PolicyId;
using facilitydrain::Result;
using facilitydrain::Revision;
using facilitydrain::ScopeKind;
using facilitydrain::Status;
using facilitydrain::consumer_manifest_digest;
using facilitydrain::digest_text;
using facilitydrain::domain_index;
using facilitydrain::kStateFormatVersion;

namespace detail = facilitydrain::detail;

constexpr std::array<OwnerDomain, 4> kAllDomains{
    OwnerDomain::kAsi, OwnerDomain::kDfi, OwnerDomain::kFacility, OwnerDomain::kMonitoring};

// ---------------------------------------------------------------------------
// The documented byte layout
// ---------------------------------------------------------------------------
//
// The fixed part of a state payload, in order:
//   u32 payload_version, u32 reserved,
//   u32 x9 limits, u64 x2 limits, u32 max_attempts_per_key,
//   u64 control_epoch, incarnation, commit_sequence, observation_sequence,
//   i64 created_at, updated_at, u32 plan_count.

constexpr std::size_t kVersionOffset = 0U;
constexpr std::size_t kReservedOffset = 4U;
constexpr std::size_t kPlanCountOffset = 4U + 4U + (9U * 4U) + (2U * 8U) + 4U + (4U * 8U) + (2U * 8U);
constexpr std::size_t kHeaderBytes = kPlanCountOffset + 4U;
/// The first byte of the first plan record is its u64 id; the scope kind
/// follows immediately.
constexpr std::size_t kFirstPlanScopeKindOffset = kHeaderBytes + 8U;

static_assert(kHeaderBytes == 116U, "the documented state header is 116 bytes");
static_assert(kPlanCountOffset == 112U, "plan_count is the last field of the header");
static_assert(kFirstPlanScopeKindOffset == 124U, "the first plan scope kind is at byte 124");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::string hex_of(std::span<const std::byte> bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(bytes.size() * 2U);
  for (const std::byte value : bytes) {
    const std::uint8_t byte = std::to_integer<std::uint8_t>(value);
    text.push_back(kDigits[(byte >> 4U) & 0x0FU]);
    text.push_back(kDigits[byte & 0x0FU]);
  }
  return text;
}

[[nodiscard]] std::span<const std::byte> span_of(const std::vector<std::byte>& bytes) {
  return std::span<const std::byte>{bytes};
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> values) {
  std::vector<std::byte> bytes;
  bytes.reserve(values.size());
  for (const std::uint8_t value : values) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

void set_byte(std::vector<std::byte>& bytes, std::size_t offset, std::uint8_t value) {
  FDC_REQUIRE(offset < bytes.size());
  bytes[offset] = static_cast<std::byte>(value);
}

void set_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
  FDC_REQUIRE(offset + 4U <= bytes.size());
  for (std::size_t index = 0; index < 4U; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
  }
}

[[nodiscard]] GenerationSet complete_generations() {
  GenerationSet set;
  set.scope = facilitydrain::ScopeGeneration{1ULL};
  set.dependency = facilitydrain::DependencyGeneration{2ULL};
  set.reservation = facilitydrain::ReservationGeneration{3ULL};
  set.obligation = facilitydrain::ObligationGeneration{4ULL};
  set.policy = facilitydrain::PolicyGeneration{5ULL};
  set.topology = facilitydrain::TopologyGeneration{6ULL};
  set.maintenance = facilitydrain::MaintenanceGeneration{7ULL};
  set.capacity = facilitydrain::CapacityGeneration{8ULL};
  set.hardware = facilitydrain::HardwareGeneration{9ULL};
  set.firmware = facilitydrain::FirmwareGeneration{10ULL};
  return set;
}

/// A hand built state with exactly one plan and no evidence, requests,
/// residuals, history, grant or fence. It is built from the public types only,
/// so it does not depend on the encoder to be well formed.
[[nodiscard]] detail::CoordinatorState make_valid_state() {
  detail::CoordinatorState state;
  state.payload_version = kStateFormatVersion;
  state.limits = Limits{};
  state.control_epoch = ControlEpoch{1ULL};
  state.incarnation = IncarnationId{7ULL};
  state.commit_sequence = CommitSequence{3ULL};
  state.observation_sequence = ObservationSequence{5ULL};
  state.created_at_milliseconds = 1000;
  state.updated_at_milliseconds = 2000;

  detail::PlanRecord plan;
  plan.spec.id = PlanId{1ULL};
  plan.spec.scope = DrainScope{ScopeKind::kRack, 4ULL};
  const Result<DrainTargetManifest> manifest = DrainTargetManifest::create(
      std::vector<DrainScope>{DrainScope{ScopeKind::kRack, 4ULL}, DrainScope{ScopeKind::kAsset, 1ULL}},
      state.limits);
  FDC_REQUIRE(manifest.has_value());
  plan.spec.targets = manifest.value();
  plan.spec.bindings.facility_epoch = ControlEpoch{1ULL};
  plan.spec.bindings.generations = complete_generations();
  plan.spec.bindings.policy_id = PolicyId{11ULL};
  plan.spec.bindings.policy_digest = digest_text(std::string_view{"policy"});
  for (const OwnerDomain domain : kAllDomains) {
    plan.spec.bindings.set_manifest_digest(
        domain, consumer_manifest_digest(domain, std::span<const facilitydrain::ConsumerRecord>{}));
  }
  plan.spec.revision = Revision{1ULL};
  plan.spec.declared_required_domains = facilitydrain::DomainMask::all();
  plan.spec.label = "drain rack 4";
  plan.spec.requested_by = "operator";
  plan.spec.created_at_milliseconds = 1000;
  plan.last_observation = ObservationSequence{5ULL};
  plan.last_commit = CommitSequence{3ULL};
  state.plans.push_back(std::move(plan));
  return state;
}

/// Encodes the hand built state, requiring success.
[[nodiscard]] std::vector<std::byte> encode_valid_state() {
  const detail::CoordinatorState state = make_valid_state();
  std::vector<std::byte> encoded;
  const Status status = detail::encode_state(state, state.limits, encoded);
  FDC_REQUIRE(status.ok());
  return encoded;
}

}  // namespace

// ---------------------------------------------------------------------------
// ByteWriter / ByteReader
// ---------------------------------------------------------------------------

FDC_TEST(codec, byte_writer_writes_little_endian_fields) {
  detail::ByteWriter writer;
  FDC_CHECK_EQ(writer.size(), 0U);
  FDC_CHECK(writer.data().empty());

  writer.u8(0xABU);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"ab"});
  writer.u32(0x01020304U);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"ab04030201"});
  writer.u64(0x0102030405060708ULL);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"ab040302010807060504030201"});
  FDC_CHECK_EQ(writer.size(), 13U);

  writer.clear();
  FDC_CHECK_EQ(writer.size(), 0U);
  writer.u32(0U);
  writer.u32(0xFFFFFFFFU);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"00000000ffffffff"});

  writer.clear();
  writer.i64(-1);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"ffffffffffffffff"});
  writer.clear();
  writer.i64(-2);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"feffffffffffffff"});
  writer.clear();
  writer.i64(1);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"0100000000000000"});

  writer.clear();
  const std::array<std::byte, 3> raw{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}};
  writer.raw(raw);
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"deadbe"});

  // take() consumes the buffer and is byte identical to data().
  const std::vector<std::byte> taken = std::move(writer).take();
  FDC_CHECK_EQ(hex_of(taken), std::string{"deadbe"});
}

FDC_TEST(codec, byte_reader_reads_little_endian_fields) {
  detail::ByteWriter writer;
  writer.u32(0xDEADBEEFU);
  writer.i64(-2);
  writer.u8(7U);
  writer.u64(0x0102030405060708ULL);
  const std::vector<std::byte> encoded = std::move(writer).take();

  detail::ByteReader reader{span_of(encoded)};
  FDC_CHECK_EQ(encoded.size(), 21U);
  FDC_CHECK_EQ(reader.position(), 0U);
  FDC_CHECK_EQ(reader.remaining(), 21U);
  FDC_CHECK(!reader.at_end());
  FDC_CHECK(!reader.limit_exceeded());

  std::uint32_t small = 0U;
  FDC_CHECK(reader.u32(small));
  FDC_CHECK_EQ(small, 0xDEADBEEFU);
  FDC_CHECK_EQ(reader.position(), 4U);

  std::int64_t negative = 0;
  FDC_CHECK(reader.i64(negative));
  FDC_CHECK_EQ(negative, -2);
  FDC_CHECK_EQ(reader.position(), 12U);

  std::uint8_t byte = 0U;
  FDC_CHECK(reader.u8(byte));
  FDC_CHECK_EQ(byte, 7U);

  std::uint64_t wide = 0ULL;
  FDC_CHECK(reader.u64(wide));
  FDC_CHECK_EQ(wide, 0x0102030405060708ULL);
  FDC_CHECK(reader.at_end());
  FDC_CHECK_EQ(reader.remaining(), 0U);
  FDC_CHECK_EQ(reader.position(), 21U);
}

FDC_TEST(codec, byte_reader_failure_consumes_nothing) {
  const std::vector<std::byte> three = bytes_of({0x01U, 0x02U, 0x03U});
  detail::ByteReader reader{span_of(three)};

  std::uint32_t wide = 0xAAAAAAAAU;
  FDC_CHECK(!reader.u32(wide));
  FDC_CHECK_EQ(wide, 0xAAAAAAAAU);
  FDC_CHECK_EQ(reader.position(), 0U);

  std::uint8_t byte = 0xAAU;
  FDC_CHECK(reader.u8(byte));
  FDC_CHECK_EQ(byte, 0x01U);
  FDC_CHECK_EQ(reader.position(), 1U);

  FDC_CHECK(!reader.u32(wide));
  FDC_CHECK_EQ(reader.position(), 1U);
  FDC_CHECK_EQ(wide, 0xAAAAAAAAU);

  std::uint64_t big = 0xBBBBBBBBBBBBBBBBULL;
  FDC_CHECK(!reader.u64(big));
  FDC_CHECK_EQ(big, 0xBBBBBBBBBBBBBBBBULL);
  FDC_CHECK_EQ(reader.position(), 1U);

  std::int64_t signed_value = 0x7FFFFFFFFFFFFFFFLL;
  FDC_CHECK(!reader.i64(signed_value));
  FDC_CHECK_EQ(signed_value, 0x7FFFFFFFFFFFFFFFLL);
  FDC_CHECK_EQ(reader.position(), 1U);
  FDC_CHECK_EQ(reader.remaining(), 2U);

  // An empty reader is at its end and every read fails without consuming.
  detail::ByteReader empty_reader{std::span<const std::byte>{}};
  FDC_CHECK(empty_reader.at_end());
  FDC_CHECK_EQ(empty_reader.remaining(), 0U);
  FDC_CHECK(!empty_reader.u8(byte));
  FDC_CHECK(!empty_reader.u32(wide));
  FDC_CHECK(!empty_reader.u64(big));
  FDC_CHECK(!empty_reader.i64(signed_value));
  FDC_CHECK_EQ(empty_reader.position(), 0U);
}

FDC_TEST(codec, sized_string_round_trip) {
  detail::ByteWriter writer;
  writer.sized_string("hello");
  writer.sized_string("");
  // A four byte little endian length prefix, then exactly that many bytes.
  FDC_CHECK_EQ(hex_of(writer.data()), std::string{"0500000068656c6c6f00000000"});
  const std::vector<std::byte> encoded = std::move(writer).take();

  detail::ByteReader reader{span_of(encoded)};
  std::string text{"sentinel"};
  FDC_CHECK(reader.sized_string(5U, text));
  FDC_CHECK_EQ(text, std::string{"hello"});
  FDC_CHECK_EQ(reader.position(), 9U);
  FDC_CHECK(reader.sized_string(0U, text));
  FDC_CHECK_EQ(text, std::string{});
  FDC_CHECK(reader.at_end());
  FDC_CHECK(!reader.limit_exceeded());

  // The declared length is a byte count, not a character count, and an
  // embedded zero is part of the payload.
  detail::ByteWriter binary;
  binary.sized_string(std::string_view{"\xC3\xA9"});
  FDC_CHECK_EQ(hex_of(binary.data()), std::string{"02000000c3a9"});
  const std::string embedded{"a\0b", 3U};
  binary.clear();
  binary.sized_string(embedded);
  FDC_CHECK_EQ(hex_of(binary.data()), std::string{"03000000610062"});

  const std::vector<std::byte> binary_bytes = std::move(binary).take();
  detail::ByteReader binary_reader{span_of(binary_bytes)};
  std::string read_back{"sentinel"};
  FDC_CHECK(binary_reader.sized_string(3U, read_back));
  FDC_CHECK_EQ(read_back.size(), 3U);
  FDC_CHECK(read_back == embedded);
  FDC_CHECK_EQ(read_back[1], '\0');
}

FDC_TEST(codec, sized_string_above_the_bound_is_refused_without_reading) {
  detail::ByteWriter writer;
  writer.sized_string("0123456789");
  const std::vector<std::byte> encoded = std::move(writer).take();
  FDC_REQUIRE(encoded.size() == 14U);

  detail::ByteReader reader{span_of(encoded)};
  std::string text{"sentinel"};
  FDC_CHECK(!reader.sized_string(4U, text));
  FDC_CHECK(reader.limit_exceeded());
  // Only the four byte prefix was consumed and the output was never touched.
  FDC_CHECK_EQ(reader.position(), 4U);
  FDC_CHECK_EQ(text, std::string{"sentinel"});
  FDC_CHECK_EQ(reader.remaining(), 10U);

  // The very same bytes decode when the bound admits the field, so the failure
  // was the bound and nothing else.
  detail::ByteReader allowed{span_of(encoded)};
  std::string accepted{"sentinel"};
  FDC_CHECK(allowed.sized_string(10U, accepted));
  FDC_CHECK_EQ(accepted, std::string{"0123456789"});
  FDC_CHECK(!allowed.limit_exceeded());
  FDC_CHECK(allowed.at_end());

  // Zero is a bound too, and a non empty field is above it.
  detail::ByteReader zero_bound{span_of(encoded)};
  std::string refused{"sentinel"};
  FDC_CHECK(!zero_bound.sized_string(0U, refused));
  FDC_CHECK(zero_bound.limit_exceeded());
  FDC_CHECK_EQ(zero_bound.position(), 4U);
  FDC_CHECK_EQ(refused, std::string{"sentinel"});
}

FDC_TEST(codec, sized_string_detects_truncation) {
  detail::ByteWriter writer;
  writer.u32(10U);
  const std::vector<std::byte> short_payload = bytes_of({0x61U, 0x62U, 0x63U});
  writer.raw(short_payload);
  const std::vector<std::byte> truncated = std::move(writer).take();

  detail::ByteReader reader{span_of(truncated)};
  std::string text{"sentinel"};
  FDC_CHECK(!reader.sized_string(100U, text));
  // Truncation is not a limit failure.
  FDC_CHECK(!reader.limit_exceeded());
  // The reader stopped just after the length prefix.
  FDC_CHECK_EQ(reader.position(), 4U);
  FDC_CHECK_EQ(text, std::string{"sentinel"});

  // A prefix that cannot itself be read consumes nothing at all.
  const std::vector<std::byte> half_prefix = bytes_of({0x0AU, 0x00U});
  detail::ByteReader half_reader{span_of(half_prefix)};
  FDC_CHECK(!half_reader.sized_string(100U, text));
  FDC_CHECK_EQ(half_reader.position(), 0U);
  FDC_CHECK(!half_reader.limit_exceeded());

  // A declared length of zero on empty input succeeds.
  const std::vector<std::byte> zero_prefix = bytes_of({0x00U, 0x00U, 0x00U, 0x00U});
  detail::ByteReader zero_reader{span_of(zero_prefix)};
  FDC_CHECK(zero_reader.sized_string(0U, text));
  FDC_CHECK_EQ(text, std::string{});
  FDC_CHECK(zero_reader.at_end());
}

// ---------------------------------------------------------------------------
// State round trip
// ---------------------------------------------------------------------------

FDC_TEST(codec, state_round_trip_is_byte_identical) {
  const detail::CoordinatorState state = make_valid_state();
  std::vector<std::byte> encoded;
  const Status encoded_status = detail::encode_state(state, state.limits, encoded);
  FDC_REQUIRE(encoded_status.ok());
  FDC_CHECK(encoded.size() > kHeaderBytes);

  const Result<detail::CoordinatorState> decoded = detail::decode_state(encoded, state.limits);
  FDC_REQUIRE_OK(decoded);
  const detail::CoordinatorState& round_tripped = decoded.value();

  FDC_CHECK_EQ(round_tripped.payload_version, state.payload_version);
  FDC_CHECK_EQ(round_tripped.control_epoch.value(), state.control_epoch.value());
  FDC_CHECK_EQ(round_tripped.incarnation.value(), state.incarnation.value());
  FDC_CHECK_EQ(round_tripped.commit_sequence.value(), state.commit_sequence.value());
  FDC_CHECK_EQ(round_tripped.observation_sequence.value(), state.observation_sequence.value());
  FDC_CHECK_EQ(round_tripped.created_at_milliseconds, state.created_at_milliseconds);
  FDC_CHECK_EQ(round_tripped.updated_at_milliseconds, state.updated_at_milliseconds);
  FDC_CHECK_EQ(round_tripped.limits.max_plans, state.limits.max_plans);
  FDC_CHECK_EQ(round_tripped.limits.max_text_bytes, state.limits.max_text_bytes);
  FDC_CHECK_EQ(round_tripped.limits.max_state_bytes, state.limits.max_state_bytes);
  FDC_CHECK_EQ(round_tripped.limits.max_document_bytes, state.limits.max_document_bytes);
  FDC_CHECK_EQ(round_tripped.plans.size(), state.plans.size());
  FDC_REQUIRE(round_tripped.plans.size() == 1U);

  const detail::PlanRecord& plan = round_tripped.plans[0];
  FDC_CHECK_EQ(plan.spec.id.value(), 1ULL);
  // Braces do not protect a comma from the preprocessor, so the whole
  // comparison is parenthesised.
  FDC_CHECK((plan.spec.scope == DrainScope{ScopeKind::kRack, 4ULL}));
  FDC_CHECK_EQ(plan.spec.targets.size(), 2U);
  FDC_CHECK_EQ(plan.spec.targets.to_canonical(), std::string{"asset:1|rack:4"});
  FDC_CHECK(plan.spec.targets.contains(DrainScope{ScopeKind::kAsset, 1ULL}));
  FDC_CHECK(plan.spec.targets.digest() == state.plans[0].spec.targets.digest());
  FDC_CHECK_EQ(plan.spec.revision.value(), 1ULL);
  FDC_CHECK_EQ(plan.spec.declared_required_domains.bits(), facilitydrain::DomainMask::all().bits());
  FDC_CHECK_EQ(plan.spec.label, std::string{"drain rack 4"});
  FDC_CHECK_EQ(plan.spec.requested_by, std::string{"operator"});
  FDC_CHECK_EQ(plan.spec.created_at_milliseconds, state.plans[0].spec.created_at_milliseconds);
  FDC_CHECK(plan.spec.bindings.generations == state.plans[0].spec.bindings.generations);
  FDC_CHECK(plan.spec.bindings.generations.is_complete());
  FDC_CHECK_EQ(plan.spec.bindings.policy_id.value(), 11ULL);
  FDC_CHECK(plan.spec.bindings.policy_digest == digest_text(std::string_view{"policy"}));
  FDC_CHECK_EQ(plan.spec.bindings.facility_epoch.value(), 1ULL);
  for (const OwnerDomain domain : kAllDomains) {
    const std::optional<ContentDigest>& stored = plan.spec.bindings.manifest_digest(domain);
    FDC_CHECK(stored.has_value());
    if (stored.has_value()) {
      FDC_CHECK(!stored->is_zero());
    }
    FDC_CHECK(plan.spec.bindings.manifest_digest(domain) ==
              state.plans[0].spec.bindings.manifest_digest(domain));
  }

  FDC_CHECK(!plan.cancelled);
  FDC_CHECK(plan.cancellation_detail.empty());
  FDC_CHECK(!plan.failed);
  FDC_CHECK(plan.failure_detail.empty());
  FDC_CHECK(!plan.grant.has_value());
  FDC_CHECK(!plan.fence.has_value());
  FDC_CHECK(plan.residuals.empty());
  FDC_CHECK(plan.requests.empty());
  FDC_CHECK(plan.history.empty());
  FDC_CHECK_EQ(plan.last_observation.value(), 5ULL);
  FDC_CHECK_EQ(plan.last_commit.value(), 3ULL);

  // Re-encoding the decoded state reproduces the exact bytes.
  std::vector<std::byte> re_encoded;
  const Status re_status = detail::encode_state(round_tripped, state.limits, re_encoded);
  FDC_REQUIRE(re_status.ok());
  FDC_CHECK(re_encoded == encoded);
  FDC_CHECK_EQ(hex_of(re_encoded), hex_of(encoded));
}

// ---------------------------------------------------------------------------
// Rejections
// ---------------------------------------------------------------------------

FDC_TEST(codec, encode_rejects_an_unsupported_payload_version) {
  detail::CoordinatorState state = make_valid_state();
  state.payload_version = kStateFormatVersion + 1U;
  std::vector<std::byte> encoded;
  FDC_CHECK_STATUS(detail::encode_state(state, state.limits, encoded), ErrorCode::kStoreVersionUnsupported);

  state.payload_version = 0U;
  std::vector<std::byte> other;
  FDC_CHECK_STATUS(detail::encode_state(state, state.limits, other), ErrorCode::kStoreVersionUnsupported);
}

FDC_TEST(codec, decode_rejects_a_wrong_payload_version) {
  std::vector<std::byte> encoded = encode_valid_state();
  set_byte(encoded, kVersionOffset, 0x02U);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kStoreVersionUnsupported);

  encoded = encode_valid_state();
  set_byte(encoded, kVersionOffset, 0x00U);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kStoreVersionUnsupported);

  encoded = encode_valid_state();
  set_u32(encoded, kVersionOffset, 0xFFFFFFFFU);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kStoreVersionUnsupported);
}

FDC_TEST(codec, decode_rejects_a_non_zero_reserved_field) {
  std::vector<std::byte> encoded = encode_valid_state();
  // The reserved field is zero in a well formed payload.
  FDC_CHECK(encoded[kReservedOffset] == std::byte{0x00});

  set_byte(encoded, kReservedOffset, 0x01U);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kReservedNotZero);

  encoded = encode_valid_state();
  set_u32(encoded, kReservedOffset, 0xFFFFFFFFU);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kReservedNotZero);
}

FDC_TEST(codec, decode_rejects_an_unknown_enum_byte) {
  std::vector<std::byte> encoded = encode_valid_state();
  FDC_REQUIRE(encoded.size() > kFirstPlanScopeKindOffset);
  // The plan's scope kind is a single byte enumeration; zero and nine are not
  // scope kinds.
  set_byte(encoded, kFirstPlanScopeKindOffset, 0x00U);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kInvalidEnumValue);

  encoded = encode_valid_state();
  set_byte(encoded, kFirstPlanScopeKindOffset, 0x09U);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kInvalidEnumValue);

  encoded = encode_valid_state();
  set_byte(encoded, kFirstPlanScopeKindOffset, 0xFFU);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kInvalidEnumValue);

  // The unmodified payload still decodes, so the rejection is the patched byte.
  const std::vector<std::byte> untouched = encode_valid_state();
  FDC_CHECK(detail::decode_state(untouched, Limits{}).has_value());
}

FDC_TEST(codec, decode_rejects_a_trailing_byte) {
  std::vector<std::byte> encoded = encode_valid_state();
  const std::size_t exact_size = encoded.size();
  encoded.push_back(std::byte{0x00});
  FDC_CHECK_EQ(encoded.size(), exact_size + 1U);
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kStoreTrailingBytes);

  encoded.push_back(std::byte{0xFF});
  FDC_CHECK_CODE(detail::decode_state(encoded, Limits{}), ErrorCode::kStoreTrailingBytes);
}

FDC_TEST(codec, decode_rejects_a_truncated_payload) {
  const std::vector<std::byte> encoded = encode_valid_state();
  FDC_REQUIRE(encoded.size() > kHeaderBytes);

  // One byte short of the last field.
  std::vector<std::byte> short_by_one{encoded.begin(), encoded.end() - 1};
  FDC_CHECK_CODE(detail::decode_state(short_by_one, Limits{}), ErrorCode::kStoreTruncated);

  // Exactly the header, but the header declares one plan.
  std::vector<std::byte> header_only{encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes)};
  FDC_CHECK_EQ(header_only.size(), kHeaderBytes);
  FDC_CHECK_CODE(detail::decode_state(header_only, Limits{}), ErrorCode::kStoreTruncated);

  // Half of the header.
  std::vector<std::byte> half_header{encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes / 2U)};
  FDC_CHECK_CODE(detail::decode_state(half_header, Limits{}), ErrorCode::kStoreTruncated);

  // Nothing at all: the input ends before the first fixed field.
  FDC_CHECK_CODE(detail::decode_state(std::span<const std::byte>{}, Limits{}), ErrorCode::kStoreTruncated);
}

FDC_TEST(codec, decode_rejects_a_collection_count_above_the_limit) {
  const Limits limits;
  FDC_REQUIRE(limits.max_plans > 1U);

  std::vector<std::byte> encoded = encode_valid_state();
  set_u32(encoded, kPlanCountOffset, limits.max_plans + 1U);
  FDC_CHECK_CODE(detail::decode_state(encoded, limits), ErrorCode::kTooManyEntries);

  // A count far above every bound is refused the same way.
  encoded = encode_valid_state();
  set_u32(encoded, kPlanCountOffset, 0xFFFFFFFFU);
  FDC_CHECK_CODE(detail::decode_state(encoded, limits), ErrorCode::kTooManyEntries);

  // The declared count is checked before any record is read: the payload still
  // contains exactly one plan, and the unmodified count decodes.
  const std::vector<std::byte> untouched = encode_valid_state();
  FDC_CHECK(detail::decode_state(untouched, limits).has_value());
}
