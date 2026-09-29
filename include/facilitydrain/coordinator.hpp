// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_COORDINATOR_HPP
#define FACILITYDRAIN_COORDINATOR_HPP

#include "facilitydrain/clock.hpp"
#include "facilitydrain/consumer.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/persistence.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/report.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Mutation context
// ---------------------------------------------------------------------------
//
// Every mutation states the exact plan, revision, incarnation and control epoch
// it was planned against, plus the caller's observation sequence. A mutation
// whose context does not match the current facts is rejected: there is no path
// by which a stale intent is applied to a newer world.

struct MutationContext {
  /// The plan the caller believes it is mutating.
  PlanId plan{};
  /// The revision the caller believes is current.
  Revision expected_revision{};
  /// The writer incarnation the caller believes is current.
  IncarnationId incarnation{};
  /// The control epoch the caller believes is current.
  ControlEpoch expected_epoch{};
  /// The caller's own observation sequence. It must be strictly greater than
  /// the plan's last accepted observation, so replayed or reordered intents are
  /// rejected rather than applied twice.
  ObservationSequence observation{};
  /// Who is asking. Diagnostic and auditable, never authorising on its own.
  std::string principal{};
  /// Zero means use the coordinator clock.
  std::int64_t requested_at_milliseconds = 0;

  [[nodiscard]] FACILITYDRAIN_API Status validate(const Limits& limits) const;
};

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

struct CoordinatorOpenRequest {
  std::filesystem::path root{};
  Limits limits{};
  PublishFaultHooks faults{};
  ClockPtr clock{};
  /// Recorded in the writer lock file so an operator can see who holds a store.
  std::string writer_label{};
  /// When false, opening a path that is not an existing store fails with
  /// kStoreNotFound instead of creating one.
  bool create_if_missing = true;
  /// Read only sessions never take the writer lock, never recover and never
  /// publish. A read only session over an empty directory fails.
  bool read_only = false;
};

struct EphemeralOptions {
  Limits limits{};
  ClockPtr clock{};
  /// The incarnation for a store that has no durable identity of its own.
  IncarnationId incarnation{};
};

// ---------------------------------------------------------------------------
// Operation requests and outcomes
// ---------------------------------------------------------------------------

struct CreatePlanRequest {
  MutationContext context{};
  PlanId id{};
  DrainScope scope{};
  /// The physical membership. Must contain the scope.
  std::vector<DrainScope> targets{};
  /// The domains that must be proven. Must be non empty.
  DomainMask declared_required_domains{};
  GenerationSet generations{};
  PolicyId policy_id{};
  ContentDigest policy_digest{};
  /// The consumer manifest known at planning time. When
  /// bind_consumer_manifests is true these records decide the bound manifest
  /// digest of every domain, including domains with no consumers.
  std::vector<ConsumerRecord> consumers{};
  bool bind_consumer_manifests = true;
  std::string label{};
  std::string requested_by{};
};

struct CreatePlanOutcome {
  DrainPlanSnapshot plan{};
};

struct RevisePlanRequest {
  MutationContext context{};
  std::optional<std::vector<DrainScope>> targets{};
  std::optional<GenerationSet> generations{};
  std::optional<PolicyId> policy_id{};
  std::optional<ContentDigest> policy_digest{};
  std::optional<std::vector<ConsumerRecord>> consumers{};
  std::optional<DomainMask> declared_required_domains{};
  FenceReason reason = FenceReason::kPlanRevised;
  std::string detail{};
};

struct RevisePlanOutcome {
  DrainPlanSnapshot plan{};
  bool fenced = false;
  std::optional<FenceRecord> fence{};
};

struct RecordEnumerationRequest {
  MutationContext context{};
  OwnerDomain domain = OwnerDomain::kAsi;
  CoverageState coverage = CoverageState::kComplete;
  EvidenceGeneration generation{};
  ObservationSequence observed_at{};
  /// The physical membership the owner enumerated against. It must equal the
  /// plan's manifest digest: an owner looking at a different physical world is
  /// not evidence about this one.
  ContentDigest scope_manifest_digest{};
  /// The consumer records the owner reported.
  std::vector<ConsumerRecord> consumers{};
  GenerationSet generations{};
  std::string source{};
  std::string annotation{};
};

struct RecordEnumerationOutcome {
  EnumerationEvidence evidence{};
  bool accepted = false;
  bool bound_manifest = false;
  bool fenced = false;
  std::optional<FenceRecord> fence{};
  std::uint32_t consumers_recorded = 0;
};

struct IssueRequestsRequest {
  MutationContext context{};
  /// Which domains to request. Must be non empty.
  DomainMask domains{};
  /// Hard bound on the obligations each request may act on. Zero means derive
  /// it from the domain's mandatory obligation count, which is still a bound.
  std::uint32_t bound_operations = 0;
};

struct IssueItem {
  DrainRequest request{};
  /// True when this key was already recorded, in which case nothing new was
  /// staged and the caller must not deliver anything.
  bool duplicate = false;
  /// True when the caller must hand this request to the owner. A record that is
  /// still staged after a restart is offered again, with the same idempotency
  /// key, so the owner can recognise the replay.
  bool deliver = false;
  std::string detail{};
};

struct IssueRequestsOutcome {
  std::vector<IssueItem> items{};
  /// Convenience view of the items the caller must deliver, in canonical order.
  std::vector<DrainRequest> to_deliver{};
  std::uint32_t newly_staged = 0;
  std::uint32_t duplicates = 0;
};

struct ConfirmDeliveryRequest {
  MutationContext context{};
  DrainRequestId request{};
  std::string delivery_reference{};
};

struct ConfirmDeliveryOutcome {
  DrainRequest request{};
};

struct RecordAcknowledgementRequest {
  MutationContext context{};
  DrainRequestId request{};
  std::string acknowledging_system{};
};

struct RecordAcknowledgementOutcome {
  DrainRequest request{};
};

struct IngestCompletionRequest {
  MutationContext context{};
  OwnerDomain domain = OwnerDomain::kAsi;
  CompletionState state = CompletionState::kUnknown;
  EvidenceGeneration generation{};
  ObservationSequence observed_at{};
  /// Digest of the owner's report payload. Required.
  ContentDigest payload_digest{};
  /// The consumer manifest the owner acted on. It must equal the enumeration
  /// the plan accepted for this domain.
  ContentDigest manifest_digest{};
  ContentDigest scope_manifest_digest{};
  GenerationSet generations{};
  bool residual_count_known = false;
  std::uint64_t residual_count = 0;
  /// Residuals the owner named. Recorded as open entries in the same commit.
  std::vector<ResidualEntry> residuals{};
  std::string source{};
  std::string annotation{};
};

struct IngestCompletionOutcome {
  CompletionEvidence evidence{};
  /// True when the report agrees with the plan's generation set, the accepted
  /// manifest and the fence floor.
  bool compatible = false;
  /// Why it is not compatible, when it is not. kOk when compatible.
  ErrorCode rejection = ErrorCode::kOk;
  bool fenced = false;
  std::optional<FenceRecord> fence{};
  std::uint32_t residuals_recorded = 0;
};

struct RecordResidualRequest {
  MutationContext context{};
  /// The entry to record. Its state is forced to open: a residual cannot be
  /// recorded as already resolved.
  ResidualEntry entry{};
};

struct RecordResidualOutcome {
  ResidualEntry entry{};
  bool fenced = false;
  std::optional<FenceRecord> fence{};
};

struct ResolveResidualRequest {
  MutationContext context{};
  ObligationId obligation{};
  OwnerDomain domain = OwnerDomain::kAsi;
  ResidualKind kind = ResidualKind::kObligationActive;
  /// The evidence generation that resolved it. Required and non zero.
  EvidenceGeneration resolution_evidence_generation{};
  std::string detail{};
};

struct ResolveResidualOutcome {
  ResidualEntry entry{};
};

struct GrantSafeToRemoveRequest {
  MutationContext context{};
  std::string granted_by{};
};

struct GrantSafeToRemoveOutcome {
  SafeToRemoveEvaluation evaluation{};
  std::optional<SafeToRemoveGrant> grant{};
};

struct FenceSafeToRemoveRequest {
  MutationContext context{};
  FenceReason reason = FenceReason::kOperatorFence;
  std::string detail{};
};

struct FenceSafeToRemoveOutcome {
  FenceRecord fence{};
  bool had_live_grant = false;
};

struct CancelPlanRequest {
  MutationContext context{};
  std::string reason{};
};

struct CancelPlanOutcome {
  DrainPlanSnapshot plan{};
  std::uint32_t requests_cancelled = 0;
  bool fenced = false;
};

struct FailPlanRequest {
  MutationContext context{};
  std::string reason{};
};

struct FailPlanOutcome {
  DrainPlanSnapshot plan{};
  bool fenced = false;
};

struct SupersedeRequestRequest {
  MutationContext context{};
  DrainRequestId request{};
  std::string reason{};
};

struct SupersedeRequestOutcome {
  DrainRequest superseded{};
  /// The replacement, staged and returned for delivery in the same commit.
  DrainRequest replacement{};
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------
//
// The coordinator owns the durable state of every drain plan in one store, and
// is the only writer of that store. It performs no network I/O and no physical
// action: it records what was asked, what the owning systems reported, and what
// remains, and it decides when the evidence justifies removal authority.
//
// Concurrency model: one writer incarnation per store, enforced by an operating
// system file lock for the whole lifetime of a read write session. In process,
// every operation takes one mutex for its whole duration, no callback is ever
// invoked while holding it, and snapshots are copied under the lock and
// rendered outside it.

class FACILITYDRAIN_API Coordinator {
 public:
  /// Opens or creates a durable store. Returns kStoreLocked when another live
  /// process holds the store, kMissingGenerationFile or kStoreCorrupt when the
  /// published generation cannot be verified, and kStoreVersionUnsupported for
  /// a store written by an incompatible layout or payload version.
  [[nodiscard]] static Result<Coordinator> open(const CoordinatorOpenRequest& request);

  /// Opens a coordinator with no durability at all, for analysis and tests of
  /// the decision rules. An ephemeral coordinator refuses nothing it would
  /// otherwise accept; it simply forgets.
  [[nodiscard]] static Result<Coordinator> open_ephemeral(const EphemeralOptions& options = {});

  ~Coordinator();
  Coordinator(Coordinator&& other) noexcept;
  Coordinator& operator=(Coordinator&& other) noexcept;
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  // -- mutations -------------------------------------------------------------

  [[nodiscard]] Result<CreatePlanOutcome> create_plan(const CreatePlanRequest& request);
  [[nodiscard]] Result<RevisePlanOutcome> revise_plan(const RevisePlanRequest& request);
  [[nodiscard]] Result<RecordEnumerationOutcome> record_enumeration(const RecordEnumerationRequest& request);
  [[nodiscard]] Result<IssueRequestsOutcome> issue_requests(const IssueRequestsRequest& request);
  [[nodiscard]] Result<ConfirmDeliveryOutcome> confirm_delivery(const ConfirmDeliveryRequest& request);
  [[nodiscard]] Result<RecordAcknowledgementOutcome> record_acknowledgement(
      const RecordAcknowledgementRequest& request);
  [[nodiscard]] Result<IngestCompletionOutcome> ingest_completion(const IngestCompletionRequest& request);
  [[nodiscard]] Result<RecordResidualOutcome> record_residual(const RecordResidualRequest& request);
  [[nodiscard]] Result<ResolveResidualOutcome> resolve_residual(const ResolveResidualRequest& request);
  [[nodiscard]] Result<GrantSafeToRemoveOutcome> grant_safe_to_remove(const GrantSafeToRemoveRequest& request);
  [[nodiscard]] Result<FenceSafeToRemoveOutcome> fence_safe_to_remove(const FenceSafeToRemoveRequest& request);
  [[nodiscard]] Result<CancelPlanOutcome> cancel_plan(const CancelPlanRequest& request);
  [[nodiscard]] Result<FailPlanOutcome> fail_plan(const FailPlanRequest& request);
  [[nodiscard]] Result<SupersedeRequestOutcome> supersede_request(const SupersedeRequestRequest& request);

  // -- queries ---------------------------------------------------------------

  [[nodiscard]] Result<CoordinatorSnapshot> snapshot() const;
  [[nodiscard]] Result<DrainPlanSnapshot> plan(PlanId id) const;
  [[nodiscard]] Result<ResidualLedger> residuals(PlanId id) const;
  [[nodiscard]] Result<std::vector<DrainRequest>> requests(PlanId id) const;
  /// Pure: recomputes the verdict from the recorded facts without changing
  /// anything, so it can be asked as often as the caller likes.
  [[nodiscard]] Result<SafeToRemoveEvaluation> evaluate_safe_to_remove(PlanId id) const;
  /// The deterministic explanation of the current verdict for a plan.
  [[nodiscard]] Result<std::string> explain(PlanId id) const;

  [[nodiscard]] Result<std::string> export_text(const ReportOptions& options = {}) const;
  [[nodiscard]] Result<std::string> export_json(const ReportOptions& options = {}) const;
  [[nodiscard]] Result<std::string> export_plan_text(PlanId id, const ReportOptions& options = {}) const;
  [[nodiscard]] Result<std::string> export_plan_json(PlanId id, const ReportOptions& options = {}) const;

  /// Flushes the store's published generation and its directory to the storage
  /// device. Every accepted mutation is already durable on return; this exists
  /// for callers that want an explicit barrier.
  [[nodiscard]] Status flush();

  // -- facts -----------------------------------------------------------------

  [[nodiscard]] const RecoveryReport& recovery() const noexcept;
  [[nodiscard]] IncarnationId incarnation() const noexcept;
  [[nodiscard]] ControlEpoch control_epoch() const noexcept;
  [[nodiscard]] CommitSequence commit_sequence() const noexcept;
  [[nodiscard]] ObservationSequence observation_sequence() const noexcept;
  [[nodiscard]] Limits limits() const noexcept;
  [[nodiscard]] bool durable() const noexcept;

  /// Opaque implementation type. It is declared here because the library's
  /// translation units define it, and it is incomplete in this header: it has
  /// no public members and no caller can construct one.
  struct Impl;

 private:
  explicit Coordinator(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_{};
};

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_COORDINATOR_HPP
