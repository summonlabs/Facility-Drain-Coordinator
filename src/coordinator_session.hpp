// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SRC_COORDINATOR_SESSION_HPP
#define FACILITYDRAIN_SRC_COORDINATOR_SESSION_HPP

#include "coordinator_internal.hpp"
#include "facilitydrain/clock.hpp"
#include "facilitydrain/coordinator.hpp"
#include "store.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace facilitydrain {

// The process local state of one coordinator. It is defined here rather than in
// a translation unit because the mutation and query implementations live in
// separate files and must agree on it exactly.
struct Coordinator::Impl {
  /// The single copy of the truth. Every mutation builds the next state, asks
  /// the store to publish it, and only adopts it once publication succeeded, so
  /// the in memory state can never be ahead of the durable one.
  detail::CoordinatorState state{};
  detail::StoreSession store{};
  RecoveryReport recovery{};
  ClockPtr clock{};
  std::string store_root{};
  bool durable = false;
  /// One mutex for the whole coordinator. No callback is ever invoked while it
  /// is held: the caller receives the requests it must deliver and delivers
  /// them after the operation returns.
  mutable std::mutex mutex{};
};

namespace detail {

/// Validates a mutation context against the facts. Fixed order, first violation
/// wins: context shape, incarnation, epoch, plan existence, revision,
/// observation ordering. A caller that is behind on any of them is told
/// exactly which one, and the same stale request always reports the same code.
[[nodiscard]] Status validate_context(const Coordinator::Impl& impl, const MutationContext& context,
                                      const PlanRecord* plan, bool require_plan, const Limits& limits);

/// The timestamp for a mutation: the caller's value when it supplied one, the
/// clock otherwise. Time is diagnostic and never authority.
[[nodiscard]] std::int64_t resolve_milliseconds(const ClockPtr& clock, std::int64_t requested);

/// The commit sequence the next published generation will carry.
[[nodiscard]] CommitSequence next_sequence(const CoordinatorState& state);

/// Publishes the state and adopts it. On failure the coordinator's state is
/// left exactly as it was, which is what makes a rejected mutation have no
/// effect at all.
[[nodiscard]] Status publish_locked(Coordinator::Impl& impl, CoordinatorState&& next);

/// The identifier of the newest evidence generation recorded for a domain.
/// Enumeration and completion reports share one counter per domain, so an
/// owner's sequence is a single ordered stream.
[[nodiscard]] EvidenceGeneration latest_evidence_generation(const PlanRecord& plan, OwnerDomain domain) noexcept;

/// A deterministic, never zero evidence identifier derived from the plan, the
/// domain and the evidence generation.
[[nodiscard]] EvidenceId evidence_id_for(PlanId plan, OwnerDomain domain, EvidenceGeneration generation) noexcept;

/// Appends or replaces a residual entry, keyed by identity. Returns true when a
/// previously relinquished entry was re-opened, which is a new obligation
/// appearing and therefore fences removal authority.
bool upsert_residual(PlanRecord& plan, ResidualEntry entry, const Limits& limits);

/// Records the transition caused by a mutation and trims the history. Both
/// states are passed in by the caller: the previous one it derived before the
/// mutation and the current one it derived after, so history cannot disagree
/// with the derivation rule.
void note_transition(PlanRecord& plan, DrainState previous, DrainState current, PlanOperation cause,
                     ObservationSequence observation, CommitSequence commit, std::int64_t milliseconds,
                     const Limits& limits, std::string detail);

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_SRC_COORDINATOR_SESSION_HPP
