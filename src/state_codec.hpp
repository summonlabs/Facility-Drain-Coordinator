// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SRC_STATE_CODEC_HPP
#define FACILITYDRAIN_SRC_STATE_CODEC_HPP

#include "coordinator_internal.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/limits.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace facilitydrain {
namespace detail {

// ---------------------------------------------------------------------------
// Canonical state encoding, version 1
// ---------------------------------------------------------------------------
//
// The encoding is fixed width little endian and self describing enough to be
// rejected precisely. Integers are little endian, strings are a u32 length
// followed by exactly that many bytes, booleans and enumerations are a single
// byte, and an absent optional is a single zero byte followed by nothing at all.
//
// Order of fields, exactly:
//
//   u32 payload_version          must equal kStateFormatVersion
//   u32 reserved                 must be zero
//   u32 x9  limits: max_plans, max_targets_per_plan, max_consumers_per_domain,
//           max_evidence_per_domain, max_requests_per_plan, max_residuals_per_plan,
//           max_history_per_plan, max_text_bytes, max_annotation_bytes
//   u64 x2  limits: max_state_bytes, max_document_bytes
//   u32     limits: max_attempts_per_key
//   u64 control_epoch, u64 incarnation, u64 commit_sequence, u64 observation_sequence
//   i64 created_at_milliseconds, i64 updated_at_milliseconds
//   u32 plan_count, then plan_count plan records in strictly increasing plan id order
//
// Plan record:
//   u64 id, u8 scope kind, u64 scope id
//   u32 target count, then per target: u8 kind, u64 id
//   u64 facility_epoch
//   GenerationSet: u64 scope, dependency, reservation, obligation, policy, topology,
//                  maintenance, capacity, hardware, firmware
//   u64 policy_id
//   32 byte policy_digest
//   per domain (four, in canonical order): u8 present, then 32 bytes when present
//   u64 revision
//   u8  declared_required_domains bits (upper bits must be zero)
//   string label, string requested_by, i64 created_at_milliseconds
//   per domain (four, in canonical order):
//     u32 planning_consumer_count, then that many consumer records
//     u32 enumeration_count, then that many enumeration records
//     u32 completion_count, then that many completion records
//   residual ledger: u32 count, then entries
//   requests: u32 count, then requests
//   grant: u8 present, then the grant when present
//   fence: u8 present, then the fence when present
//   history: u32 count, then entries
//   u8 cancelled, string cancellation_detail
//   u8 failed, string failure_detail
//   u64 last_observation, u64 last_commit
//
// Consumer record:
//   u64 obligation, u8 category, u64 generation, u64 reservation, u8 strength,
//   string label, string source
//
// Enumeration record:
//   u8 domain, u8 coverage, u64 generation, u64 observed_at, 32 byte manifest_digest,
//   32 byte scope_manifest_digest, GenerationSet, string source, string annotation,
//   i64 observed_at_milliseconds,
//   u32 consumer count, then that many consumer records
//
// Completion record:
//   u64 id, u8 domain, u8 state, u64 generation, u64 observed_at, 32 byte payload_digest,
//   32 byte manifest_digest, 32 byte scope_manifest_digest, GenerationSet,
//   u8 residual_count_known, u64 residual_count, string source, string annotation,
//   i64 observed_at_milliseconds
//
// Residual entry:
//   u64 obligation, u8 domain, u8 kind, u8 state, u64 generation,
//   u64 resolution_evidence_generation, u64 recorded_at, u64 resolved_at,
//   32 byte detail_digest, string detail
//
// Request:
//   u64 id, u64 key.plan, u8 key.domain, u8 key.scope.kind, u64 key.scope.id,
//   32 byte key.obligation_digest, u64 key.policy_generation, u64 key.attempt,
//   32 byte idempotency_key, u8 state, u64 staged_at, u64 issued_at, u32 bound_operations,
//   string target_system, string instruction, i64 staged_at_milliseconds,
//   i64 issued_at_milliseconds, u64 acknowledged_at, string acknowledgement_source,
//   u64 settled_at, string settlement_detail
//
// Grant:
//   u64 plan, u64 revision, u64 epoch, GenerationSet, 32 byte evidence_digest,
//   32 byte manifest_digest, u64 observation_floor, u64 granted_commit,
//   i64 granted_at_milliseconds, string granted_by
//
// Fence:
//   u8 reason, u64 floor, u64 revision, u64 epoch, u64 commit,
//   i64 recorded_at_milliseconds, string detail
//
// History entry:
//   u8 from_state, u8 to_state, u8 cause, u64 observed_at, u64 commit,
//   i64 recorded_at_milliseconds, string detail
//
// Rejection rules, all of which are exercised by the corruption tests:
//   * payload_version != kStateFormatVersion          -> kStoreVersionUnsupported
//   * reserved field not zero                         -> kReservedNotZero
//   * unknown enumeration value in any u8 enum field  -> kInvalidEnumValue
//     (including domain bits above the four known domains)
//   * declared string length above its bound          -> kPayloadTooLarge
//   * declared string length beyond the input         -> kStoreTruncated
//   * declared collection count above its bound       -> kTooManyEntries
//   * input ends before a fixed field                 -> kStoreTruncated
//   * bytes remain after the last field               -> kStoreTrailingBytes
//   * invalid UTF-8 or control characters in text     -> kInvalidText
//   * any semantic invariant violation                -> kMalformedRecord
//   * the stored limits are below the requested limits-> kLimitExceeded

/// Encodes the state. The limits supply the bounds used for every length and
/// count check. The encoder validates the state first and refuses to encode an
/// inconsistent state: a store must never contain a payload that cannot be
/// decoded by the same build.
[[nodiscard]] Status encode_state(const CoordinatorState& state, const Limits& limits,
                                  std::vector<std::byte>& out);

/// Decodes a payload. Every rejection above is applied; the returned state is
/// complete or the call fails.
[[nodiscard]] Result<CoordinatorState> decode_state(std::span<const std::byte> bytes, const Limits& limits);

/// The semantic invariants a state must satisfy, checked independently of the
/// byte form. Used by the encoder before publishing and by the decoder after
/// reading, so a state that violates them can neither be written nor adopted.
///
/// Invariants, in this order:
///   1. payload_version is kStateFormatVersion                       -> kStoreVersionUnsupported
///   2. limits validate                                              -> whatever validate returns
///   3. plan ids strictly increasing                                 -> kMalformedRecord
///   4. every plan revision >= 1                                     -> kMalformedRecord
///   5. every plan's declared_required_domains is non empty          -> kMalformedRecord
///   6. every plan's target manifest is non empty, canonical, and contains the scope
///                                                                   -> kMalformedRecord
///   7. every bound consumer manifest digest is non zero              -> kMalformedRecord
///   8. per domain, enumeration generations strictly increasing and
///      completion generations strictly increasing                   -> kMalformedRecord
///   9. the residual ledger is in canonical order with no duplicate identities
///                                                                   -> kMalformedRecord
///  10. requests are in canonical order with no duplicate id         -> kMalformedRecord
///  11. history size within limits.max_history_per_plan              -> kTooManyEntries
///  12. request count within limits.max_requests_per_plan            -> kTooManyEntries
///  13. a record's strings satisfy the same text bounds the API enforces
///                                                                   -> kFieldTooLong/kInvalidText
[[nodiscard]] Status validate_state(const CoordinatorState& state, const Limits& limits);

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_SRC_STATE_CODEC_HPP
