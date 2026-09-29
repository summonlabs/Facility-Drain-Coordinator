// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_REQUESTS_HPP
#define FACILITYDRAIN_REQUESTS_HPP

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/scope.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Bounded drain requests
// ---------------------------------------------------------------------------
//
// The coordinator does not drain anything. It issues a bounded request to the
// system that owns the effect, and that system decides and then reports. The
// request is externally consequential, so re-issuing one that was already
// issued is a defect even when the acknowledgement was lost in a restart.
//
// The defence is the idempotency key: a deterministic digest over the plan, the
// domain, the scope, the obligations being drained, the policy generation and
// the attempt. The key deliberately excludes the plan revision, because a
// revision is a bookkeeping change inside the coordinator and must not turn one
// physical drain into two. Only an explicit supersede, which records who
// decided and why, advances the attempt and therefore the key.

enum class RequestState : std::uint8_t {
  /// Durable intent: the request exists, has an idempotency key, and has not
  /// yet been handed to the owner. A restart re-offers exactly this record, and
  /// because the key is unchanged the owner sees a replay rather than a second
  /// physical drain.
  kStaged = 1,
  /// The caller confirmed the request was handed to the owner. Still not an effect.
  kIssued = 2,
  /// The owner confirmed receipt. Not an effect.
  kAcknowledged = 3,
  /// The owner reported the request drained. The evidence that says so is
  /// ingested separately and is what a verdict actually uses.
  kCompleted = 4,
  kRefused = 5,
  kFailed = 6,
  /// Replaced by an explicit later attempt.
  kSuperseded = 7,
  /// The plan was cancelled while the request was outstanding.
  kCancelled = 8,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(RequestState state) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<RequestState> request_state_from_token(std::string_view token) noexcept;
/// True for states in which the owner has confirmed receipt.
[[nodiscard]] FACILITYDRAIN_API bool is_request_acknowledged(RequestState state) noexcept;
/// True while the record still expects something to happen to it, which is the
/// condition under which a restart must re-offer it rather than forget it.
[[nodiscard]] FACILITYDRAIN_API bool is_request_outstanding(RequestState state) noexcept;
/// True for states that no longer expect anything.
[[nodiscard]] FACILITYDRAIN_API bool is_request_settled(RequestState state) noexcept;

struct DrainRequestKey {
  PlanId plan{};
  OwnerDomain domain = OwnerDomain::kAsi;
  DrainScope scope{};
  /// Digest of the obligations this request asks the owner to drain. The
  /// request is about exactly this obligation set.
  ContentDigest obligation_digest{};
  PolicyGeneration policy_generation{};
  /// 1 for the first attempt. Only an explicit supersede advances it.
  AttemptId attempt{};
};

/// The deterministic identity of an externally consequential request. Stable
/// across processes, restarts and rebuilds.
[[nodiscard]] FACILITYDRAIN_API ContentDigest request_idempotency_key(const DrainRequestKey& key) noexcept;

/// A stable handle derived from the key, so a replay of the same key produces
/// the same handle even when it is the first time this process has seen it.
/// Never zero for a valid key.
[[nodiscard]] FACILITYDRAIN_API DrainRequestId request_id_for(const DrainRequestKey& key) noexcept;

struct DrainRequest {
  DrainRequestId id{};
  DrainRequestKey key{};
  ContentDigest idempotency_key{};
  RequestState state = RequestState::kStaged;
  /// When the record was staged. Set on the first accepted issue.
  ObservationSequence staged_at{};
  /// When delivery to the owner was confirmed by the caller.
  ObservationSequence issued_at{};
  /// Hard upper bound on the number of obligations this request may act on. A
  /// request is bounded or it is not issued.
  std::uint32_t bound_operations = 0;
  /// The owning system the request is addressed to.
  std::string target_system{};
  /// The canonical instruction text. Generated deterministically from the key
  /// and the bound, so a replay produces byte identical text.
  std::string instruction{};
  std::int64_t staged_at_milliseconds = 0;
  std::int64_t issued_at_milliseconds = 0;
  ObservationSequence acknowledged_at{};
  std::string acknowledgement_source{};
  ObservationSequence settled_at{};
  std::string settlement_detail{};

  [[nodiscard]] FACILITYDRAIN_API Status validate(const Limits& limits) const;
  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
  [[nodiscard]] bool operator<(const DrainRequest& other) const noexcept;
};

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_REQUESTS_HPP
