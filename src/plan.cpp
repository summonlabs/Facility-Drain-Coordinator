// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/plan.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace facilitydrain {

std::string_view to_token(DrainState state) noexcept {
  switch (state) {
    case DrainState::kProposed:
      return "proposed";
    case DrainState::kEnumerating:
      return "enumerating";
    case DrainState::kRequested:
      return "requested";
    case DrainState::kDraining:
      return "draining";
    case DrainState::kResidualsPresent:
      return "residuals-present";
    case DrainState::kDrained:
      return "drained";
    case DrainState::kSafeToRemove:
      return "safe-to-remove";
    case DrainState::kCancelled:
      return "cancelled";
    case DrainState::kFailed:
      return "failed";
  }
  return std::string_view{};
}

std::optional<DrainState> drain_state_from_token(std::string_view token) noexcept {
  if (token == "proposed") {
    return DrainState::kProposed;
  }
  if (token == "enumerating") {
    return DrainState::kEnumerating;
  }
  if (token == "requested") {
    return DrainState::kRequested;
  }
  if (token == "draining") {
    return DrainState::kDraining;
  }
  if (token == "residuals-present") {
    return DrainState::kResidualsPresent;
  }
  if (token == "drained") {
    return DrainState::kDrained;
  }
  if (token == "safe-to-remove") {
    return DrainState::kSafeToRemove;
  }
  if (token == "cancelled") {
    return DrainState::kCancelled;
  }
  if (token == "failed") {
    return DrainState::kFailed;
  }
  return std::nullopt;
}

bool is_terminal_drain_state(DrainState state) noexcept {
  // Both states are recorded facts, not verdicts: no later operation moves a
  // plan out of them except a revision that creates the next attempt.
  return state == DrainState::kCancelled || state == DrainState::kFailed;
}

bool is_authority_drain_state(DrainState state) noexcept {
  // SafeToRemove is the only state that carries removal authority. Drained is a
  // fact about the obligations, not permission to remove anything.
  return state == DrainState::kSafeToRemove;
}

std::string_view to_token(PlanOperation operation) noexcept {
  switch (operation) {
    case PlanOperation::kRevise:
      return "revise";
    case PlanOperation::kRecordEnumeration:
      return "record-enumeration";
    case PlanOperation::kIssueRequests:
      return "issue-requests";
    case PlanOperation::kRecordAcknowledgement:
      return "record-acknowledgement";
    case PlanOperation::kIngestCompletion:
      return "ingest-completion";
    case PlanOperation::kRecordResidual:
      return "record-residual";
    case PlanOperation::kResolveResidual:
      return "resolve-residual";
    case PlanOperation::kEvaluate:
      return "evaluate";
    case PlanOperation::kGrant:
      return "grant";
    case PlanOperation::kFence:
      return "fence";
    case PlanOperation::kCancel:
      return "cancel";
    case PlanOperation::kFail:
      return "fail";
    case PlanOperation::kSupersedeRequest:
      return "supersede-request";
  }
  return std::string_view{};
}

Status check_plan_operation(DrainState state, PlanOperation operation) {
  // An evaluation is admissible in every state without exception. It changes
  // nothing, and a denied verdict is still an answer: a cancelled or failed plan
  // must be able to explain why it is denied, and a safe to remove plan must be
  // able to re-confirm its verdict.
  if (operation == PlanOperation::kEvaluate) {
    return Status::success();
  }

  if (state == DrainState::kCancelled) {
    // A cancelled plan admits only its own termination path: a revision that
    // creates the next attempt, and the evaluation above. Everything else would
    // be work on a plan the operator has already stopped.
    if (operation == PlanOperation::kRevise) {
      return Status::success();
    }
    std::string detail = "plan state 'cancelled' admits only a revision and an evaluation, not '";
    detail += to_token(operation);
    detail += "'";
    return Status::failure(ErrorCode::kPlanCancelled, detail);
  }

  if (state == DrainState::kFailed) {
    // The same rule as cancelled, plus the explicit cancellation that closes the
    // attempt out in the audit trail.
    if (operation == PlanOperation::kRevise || operation == PlanOperation::kCancel) {
      return Status::success();
    }
    std::string detail = "plan state 'failed' admits only a revision, an evaluation and a cancellation, not '";
    detail += to_token(operation);
    detail += "'";
    return Status::failure(ErrorCode::kPlanFailed, detail);
  }

  if (state == DrainState::kSafeToRemove) {
    // Removal authority is already held. Issuing another request would ask an
    // owner to drain a scope that is already authorised for removal, turning a
    // completed drain into a new external effect; superseding one would advance
    // the attempt for the same reason. The plan must be fenced first, which is
    // what makes the existing answer stop being live.
    if (operation == PlanOperation::kIssueRequests || operation == PlanOperation::kSupersedeRequest) {
      return Status::failure(ErrorCode::kInvalidStateTransition,
                             "the scope already carries removal authority; fence it before requesting further drains");
    }
    return Status::success();
  }

  // Every other state admits every operation at the state level. What is
  // admissible is decided by the recorded facts, which is what produces the
  // precise blocking codes (an incomplete enumeration, an unbound manifest, a
  // stale generation) instead of a generic state refusal.
  return Status::success();
}

std::string_view to_token(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::kNone:
      return "none";
    case FenceReason::kRestart:
      return "restart";
    case FenceReason::kControlEpochChanged:
      return "control-epoch-changed";
    case FenceReason::kNewObligation:
      return "new-obligation";
    case FenceReason::kPlanRevised:
      return "plan-revised";
    case FenceReason::kEvidenceSuperseded:
      return "evidence-superseded";
    case FenceReason::kScopeManifestChanged:
      return "scope-manifest-changed";
    case FenceReason::kOperatorFence:
      return "operator-fence";
    case FenceReason::kPlanCancelled:
      return "plan-cancelled";
    case FenceReason::kPlanFailed:
      return "plan-failed";
    case FenceReason::kDependencyChange:
      return "dependency-change";
  }
  return std::string_view{};
}

std::optional<FenceReason> fence_reason_from_token(std::string_view token) noexcept {
  if (token == "none") {
    return FenceReason::kNone;
  }
  if (token == "restart") {
    return FenceReason::kRestart;
  }
  if (token == "control-epoch-changed") {
    return FenceReason::kControlEpochChanged;
  }
  if (token == "new-obligation") {
    return FenceReason::kNewObligation;
  }
  if (token == "plan-revised") {
    return FenceReason::kPlanRevised;
  }
  if (token == "evidence-superseded") {
    return FenceReason::kEvidenceSuperseded;
  }
  if (token == "scope-manifest-changed") {
    return FenceReason::kScopeManifestChanged;
  }
  if (token == "operator-fence") {
    return FenceReason::kOperatorFence;
  }
  if (token == "plan-cancelled") {
    return FenceReason::kPlanCancelled;
  }
  if (token == "plan-failed") {
    return FenceReason::kPlanFailed;
  }
  if (token == "dependency-change") {
    return FenceReason::kDependencyChange;
  }
  return std::nullopt;
}

}  // namespace facilitydrain
