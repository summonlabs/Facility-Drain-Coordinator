// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "coordinator_internal.hpp"

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/report.hpp"
#include "facilitydrain/residual.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {
namespace {

/// A compact, deterministic rendering of the four domain verdicts. Built from
/// recorded values only, in canonical domain order.
std::string domain_summary(const SafeToRemoveEvaluation& evaluation) {
  std::string text;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const DomainAssessment& assessment = evaluation.domains[index];
    if (index != 0) {
      text.push_back(' ');
    }
    text.append(to_token(assessment.domain));
    text.push_back('=');
    text.append(to_token(assessment.verdict));
    if (assessment.blocking_code != ErrorCode::kOk) {
      text.push_back('(');
      text.append(to_token(assessment.blocking_code));
      text.push_back(')');
    }
  }
  return text;
}

}  // namespace

namespace detail {
namespace {

/// Counts the mandatory obligations of one domain inside the plan's manifest of
/// record. Used only for reporting: the requirement itself comes from the
/// manifest, not from this count.
std::uint32_t count_mandatory(const DrainPlanSnapshot& plan, OwnerDomain domain) {
  std::uint32_t total = 0;
  for (const auto& consumer : plan.consumers) {
    if (consumer.domain() == domain && consumer.strength == ObligationStrength::kMandatory) {
      ++total;
    }
  }
  return total;
}

bool has_open_protected(const ResidualLedger& ledger, OwnerDomain domain) {
  for (const auto& entry : ledger.entries) {
    if (entry.domain == domain && entry.state == ResidualState::kOpen &&
        entry.kind == ResidualKind::kProtectedObligation) {
      return true;
    }
  }
  return false;
}

/// The reason text is part of the deterministic output, so it is built from
/// recorded values only: no clock, no pointer, no iteration order that could
/// differ between runs.
std::string join_reason(std::string_view lead, std::string_view detail) {
  std::string text{lead};
  if (!detail.empty()) {
    text.append(": ");
    text.append(detail);
  }
  return text;
}

DomainAssessment assess_domain(const DrainPlanSnapshot& plan, OwnerDomain domain) {
  DomainAssessment assessment;
  assessment.domain = domain;
  assessment.required = plan.required_domains.contains(domain);

  const std::uint32_t index = domain_index(domain);
  const DomainEnumerationSnapshot& enumeration = plan.enumerations[index];
  const DomainCompletionSnapshot& completion = plan.completions[index];
  const ResidualLedger& ledger = plan.residuals;

  assessment.open_residuals = ledger.open_count(domain);
  assessment.unknown_residuals = ledger.unknown_count(domain);
  assessment.known_obligations = ledger.known_obligation_count(domain);
  assessment.relinquished_obligations = ledger.relinquished_obligation_count(domain);
  assessment.mandatory_obligations = count_mandatory(plan, domain);

  if (!assessment.required) {
    assessment.verdict = DomainVerdict::kNotRequired;
    assessment.reason = "the plan does not require this domain";
    return assessment;
  }

  // A protected obligation is a policy decision that no amount of evidence can
  // override, so it is checked before any evidence is consulted.
  if (has_open_protected(ledger, domain)) {
    assessment.verdict = DomainVerdict::kIncomplete;
    assessment.blocking_code = ErrorCode::kProtectedObligation;
    assessment.reason = "the ledger holds an open protected obligation for this domain";
    return assessment;
  }

  if (!enumeration.present) {
    assessment.verdict = DomainVerdict::kUnknown;
    assessment.blocking_code = ErrorCode::kIncompleteEnumeration;
    assessment.reason = "no enumeration evidence has been recorded for this domain";
    return assessment;
  }

  assessment.enumeration_present = true;
  assessment.enumeration_complete = enumeration.evidence.coverage == CoverageState::kComplete;
  assessment.enumeration_manifest_matches = enumeration.accepted;
  assessment.enumeration_after_floor = enumeration.rejection != ErrorCode::kStaleEvidence;
  assessment.enumeration_generation = enumeration.evidence.generation;

  if (!assessment.enumeration_complete) {
    assessment.verdict = DomainVerdict::kUnknown;
    assessment.blocking_code = ErrorCode::kIncompleteEnumeration;
    assessment.reason = join_reason("enumeration coverage is not complete",
                                    to_token(enumeration.evidence.coverage));
    return assessment;
  }
  if (enumeration.rejection != ErrorCode::kOk) {
    // A complete enumeration that does not describe this plan's manifest is not
    // evidence about this plan: it is evidence about a different world.
    assessment.verdict = DomainVerdict::kIncomplete;
    assessment.blocking_code = enumeration.rejection;
    assessment.reason = join_reason("the enumeration does not match the plan binding",
                                    to_token(enumeration.rejection));
    return assessment;
  }

  if (!completion.present) {
    assessment.verdict = DomainVerdict::kUnknown;
    assessment.blocking_code = ErrorCode::kEvidenceIncomplete;
    assessment.reason = "no completion report has been recorded for this domain";
    return assessment;
  }

  assessment.completion_present = true;
  assessment.completion_compatible = completion.compatible;
  assessment.completion_manifest_matches =
      completion.rejection != ErrorCode::kEvidenceMismatch &&
      completion.rejection != ErrorCode::kScopeManifestMismatch;
  assessment.completion_after_floor = completion.rejection != ErrorCode::kStaleEvidence;
  assessment.completion_generation = completion.evidence.generation;
  assessment.evidence_generations = completion.evidence.generations;
  assessment.residual_count_known = completion.evidence.residual_count_known;

  if (completion.rejection != ErrorCode::kOk) {
    assessment.verdict = DomainVerdict::kIncomplete;
    assessment.blocking_code = completion.rejection;
    assessment.reason = join_reason("the completion report does not match the plan binding",
                                    to_token(completion.rejection));
    return assessment;
  }

  const CompletionState state = completion.evidence.state;
  switch (state) {
    case CompletionState::kRequested:
      assessment.verdict = DomainVerdict::kIncomplete;
      assessment.blocking_code = ErrorCode::kAcknowledgementIsNotEffect;
      assessment.reason = "the owning system reports the request received but not started";
      return assessment;
    case CompletionState::kAcknowledged:
      assessment.verdict = DomainVerdict::kIncomplete;
      assessment.blocking_code = ErrorCode::kAcknowledgementIsNotEffect;
      assessment.reason = "an acknowledgement is not an effect";
      return assessment;
    case CompletionState::kDraining:
      assessment.verdict = DomainVerdict::kIncomplete;
      assessment.blocking_code = ErrorCode::kNotDrained;
      assessment.reason = "the owning system reports the drain still in progress";
      return assessment;
    case CompletionState::kRefused:
      assessment.verdict = DomainVerdict::kFailed;
      assessment.blocking_code = ErrorCode::kDomainFailed;
      assessment.reason = "the owning system refused the drain";
      return assessment;
    case CompletionState::kFailed:
      assessment.verdict = DomainVerdict::kFailed;
      assessment.blocking_code = ErrorCode::kDomainFailed;
      assessment.reason = "the owning system reported a failure";
      return assessment;
    case CompletionState::kDrained:
    case CompletionState::kDrainedWithResiduals:
      break;
    case CompletionState::kUnknown:
      // Impossible for a stored record: it is rejected at ingestion and again
      // by the decoder. Reported as an unknown rather than quietly tolerated.
      assessment.verdict = DomainVerdict::kUnknown;
      assessment.blocking_code = ErrorCode::kInvalidEnumValue;
      assessment.reason = "the completion report carries an unknown state";
      return assessment;
  }

  if (state == CompletionState::kDrained && !completion.evidence.residual_count_known) {
    assessment.verdict = DomainVerdict::kUnknown;
    assessment.blocking_code = ErrorCode::kUnknownResidualCount;
    assessment.reason =
        "the owning system reported the domain drained without stating how many obligations remain";
    return assessment;
  }

  assessment.residual_count = completion.evidence.residual_count;

  if (assessment.residual_count == 0) {
    if (assessment.open_residuals > 0) {
      // Somebody could not measure something. "Unmeasured" and "known to
      // remain" are different answers and are never collapsed into one.
      const bool unknown = assessment.unknown_residuals > 0;
      assessment.verdict = unknown ? DomainVerdict::kUnknown : DomainVerdict::kIncomplete;
      assessment.blocking_code = unknown ? ErrorCode::kUnknownObligation : ErrorCode::kResidualsPresent;
      assessment.reason = "the ledger holds " + std::to_string(assessment.open_residuals) +
                          " open residual entries for this domain";
      return assessment;
    }
    assessment.verdict = DomainVerdict::kProvenComplete;
    assessment.blocking_code = ErrorCode::kOk;
    assessment.reason = "the owning system proved the domain drained with no obligations remaining";
    return assessment;
  }

  // The owner says obligations remain. They must all be identified in the
  // ledger and all relinquished with evidence: a count nobody can break down is
  // an unknown, not a completion.
  if (assessment.open_residuals > 0) {
    const bool unknown = assessment.unknown_residuals > 0;
    assessment.verdict = unknown ? DomainVerdict::kUnknown : DomainVerdict::kIncomplete;
    assessment.blocking_code = unknown ? ErrorCode::kUnknownObligation : ErrorCode::kResidualsPresent;
    assessment.reason = "the ledger holds " + std::to_string(assessment.open_residuals) +
                        " open residual entries for this domain";
    return assessment;
  }
  if (assessment.known_obligations < assessment.residual_count) {
    assessment.verdict = DomainVerdict::kUnknown;
    assessment.blocking_code = ErrorCode::kUnknownObligation;
    assessment.reason = "the owning system reports " + std::to_string(assessment.residual_count) +
                        " obligations remaining but only " + std::to_string(assessment.known_obligations) +
                        " are identified in the ledger";
    return assessment;
  }
  if (assessment.relinquished_obligations < assessment.residual_count) {
    assessment.verdict = DomainVerdict::kIncomplete;
    assessment.blocking_code = ErrorCode::kResidualsPresent;
    assessment.reason = std::to_string(assessment.relinquished_obligations) + " of " +
                        std::to_string(assessment.residual_count) +
                        " reported obligations have been relinquished";
    return assessment;
  }

  assessment.verdict = DomainVerdict::kProvenComplete;
  assessment.blocking_code = ErrorCode::kOk;
  assessment.reason = "every reported obligation is relinquished with evidence";
  return assessment;
}

void append_unique(std::vector<ErrorCode>& codes, ErrorCode code) {
  if (code == ErrorCode::kOk) {
    return;
  }
  for (const ErrorCode existing : codes) {
    if (existing == code) {
      return;
    }
  }
  codes.push_back(code);
}

}  // namespace

}  // namespace detail

std::string_view to_token(DomainVerdict verdict) noexcept {
  switch (verdict) {
    case DomainVerdict::kProvenComplete:
      return "proven-complete";
    case DomainVerdict::kIncomplete:
      return "incomplete";
    case DomainVerdict::kUnknown:
      return "unknown";
    case DomainVerdict::kNotRequired:
      return "not-required";
    case DomainVerdict::kFailed:
      return "failed";
  }
  return "unknown";
}

std::string_view to_token(SafeToRemoveVerdict verdict) noexcept {
  switch (verdict) {
    case SafeToRemoveVerdict::kGranted:
      return "granted";
    case SafeToRemoveVerdict::kDenied:
      return "denied";
  }
  return "denied";
}

namespace detail {

DrainState derive_state_from_snapshot(const DrainPlanSnapshot& plan, ControlEpoch current_epoch) noexcept {
  // First match wins. The order is the contract documented in plan.hpp and is
  // what makes a state impossible to assert rather than derive.
  if (plan.cancelled) {
    return DrainState::kCancelled;
  }
  if (plan.failed) {
    return DrainState::kFailed;
  }
  if (plan.grant.has_value() && plan.grant->epoch == current_epoch &&
      plan.grant->revision == plan.spec.revision &&
      (!plan.fence.has_value() || plan.fence->commit <= plan.grant->granted_commit)) {
    return DrainState::kSafeToRemove;
  }

  bool all_required_proven = true;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!plan.required_domains.contains(domain)) {
      continue;
    }
    const DomainAssessment assessment = assess_domain(plan, domain);
    if (assessment.verdict != DomainVerdict::kProvenComplete) {
      all_required_proven = false;
      break;
    }
  }

  if (all_required_proven && !has_blocking_residuals(plan)) {
    return DrainState::kDrained;
  }
  if (has_blocking_residuals(plan)) {
    return DrainState::kResidualsPresent;
  }
  for (const auto& request : plan.requests) {
    if (is_request_acknowledged(request.state)) {
      return DrainState::kDraining;
    }
  }
  for (const auto& request : plan.requests) {
    if (request.state == RequestState::kIssued || request.state == RequestState::kStaged) {
      return DrainState::kRequested;
    }
  }
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    if (plan.enumerations[index].present) {
      return DrainState::kEnumerating;
    }
  }
  return DrainState::kProposed;
}

bool has_blocking_residuals(const DrainPlanSnapshot& plan) noexcept {
  for (const auto& entry : plan.residuals.entries) {
    if (entry.state != ResidualState::kOpen) {
      continue;
    }
    if (is_unknown_residual_kind(entry.kind)) {
      return true;
    }
    if (plan.required_domains.contains(entry.domain)) {
      return true;
    }
  }
  return false;
}

SafeToRemoveEvaluation evaluate_safe_to_remove(const DrainPlanSnapshot& plan, ControlEpoch current_epoch) {
  SafeToRemoveEvaluation evaluation;
  evaluation.plan = plan.spec.id;
  evaluation.revision = plan.spec.revision;
  evaluation.epoch = current_epoch;
  evaluation.generations = plan.spec.bindings.generations;
  evaluation.manifest_digest = plan.spec.targets.digest();
  evaluation.fence_floor = plan.fence.has_value() ? plan.fence->floor : ObservationSequence{};
  evaluation.fenced = plan.fence.has_value();
  evaluation.required_domain_count = plan.required_domains.count();
  evaluation.evaluated_commit = plan.last_commit;

  // The evidence digest names exactly the evidence the verdict was computed
  // from. Every domain contributes, present or not, so the digest changes when
  // a record appears, disappears or changes.
  std::array<ContentDigest, 2> per_domain{};
  std::vector<ContentDigest> parts;
  parts.reserve(kOwnerDomainCount * 2 + 2);
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const DomainEnumerationSnapshot& enumeration = plan.enumerations[index];
    per_domain[0] = enumeration.present ? digest_text(enumeration.evidence.to_canonical())
                                        : digest_text("no-enumeration");
    const DomainCompletionSnapshot& completion = plan.completions[index];
    per_domain[1] = completion.present ? digest_text(completion.evidence.to_canonical())
                                       : digest_text("no-completion");
    parts.push_back(per_domain[0]);
    parts.push_back(per_domain[1]);
  }
  parts.push_back(digest_text("required " + plan.required_domains.to_canonical()));
  parts.push_back(digest_text("generations " + plan.spec.bindings.generations.to_canonical()));
  evaluation.evidence_digest = combine_digests("evaluation-evidence", parts);

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    evaluation.domains[index] = assess_domain(plan, owner_domain_at(index));
  }

  std::vector<ErrorCode> codes;
  if (plan.cancelled) {
    append_unique(codes, ErrorCode::kPlanCancelled);
  } else if (plan.failed) {
    append_unique(codes, ErrorCode::kPlanFailed);
  } else {
    for (const DomainAssessment& assessment : evaluation.domains) {
      append_unique(codes, assessment.blocking_code);
    }
    // An unknown in a domain the plan does not require still withholds removal
    // authority: the plan may not need that domain, but nobody can claim the
    // scope is safe while an identified factor is unmeasured.
    for (const auto& entry : plan.residuals.entries) {
      if (entry.state == ResidualState::kOpen && is_unknown_residual_kind(entry.kind) &&
          !plan.required_domains.contains(entry.domain)) {
        append_unique(codes, ErrorCode::kUnknownObligation);
      }
    }
  }

  evaluation.blocking_codes = codes;
  evaluation.primary_blocking_code = codes.empty() ? ErrorCode::kOk : codes.front();
  evaluation.verdict = codes.empty() ? SafeToRemoveVerdict::kGranted : SafeToRemoveVerdict::kDenied;

  bool all_proven = true;
  bool enumeration_complete = true;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!plan.required_domains.contains(domain)) {
      continue;
    }
    const DomainAssessment& assessment = evaluation.domains[index];
    if (assessment.verdict != DomainVerdict::kProvenComplete) {
      all_proven = false;
    }
    if (!assessment.enumeration_complete || !assessment.enumeration_manifest_matches) {
      enumeration_complete = false;
    }
  }
  evaluation.all_required_proven = all_proven && evaluation.required_domain_count != 0;
  evaluation.enumeration_complete_for_required = enumeration_complete;
  evaluation.open_residuals = plan.residuals.open_count();
  evaluation.unknown_residuals = plan.residuals.unknown_count();

  evaluation.explanation = format_evaluation(evaluation);
  return evaluation;
}

}  // namespace detail

SafeToRemoveEvaluation evaluate_safe_to_remove(const DrainPlanSnapshot& plan, ControlEpoch current_epoch) {
  return detail::evaluate_safe_to_remove(plan, current_epoch);
}

std::string SafeToRemoveEvaluation::to_canonical() const {
  std::string text;
  text.append("plan=");
  text.append(to_string(plan));
  text.append(" revision=");
  text.append(to_string(revision));
  text.append(" epoch=");
  text.append(to_string(epoch));
  text.append(" verdict=");
  text.append(to_token(verdict));
  text.append(" primary=");
  text.append(to_token(primary_blocking_code));
  text.append(" required=");
  text.append("set");
  text.append(" open-residuals=");
  text.append(std::to_string(open_residuals));
  text.append(" unknown-residuals=");
  text.append(std::to_string(unknown_residuals));
  text.append(" evidence=");
  text.append(evidence_digest.to_hex());
  return text;
}

std::string format_evaluation(const SafeToRemoveEvaluation& evaluation) {
  std::string text;
  text.append("verdict ");
  text.append(to_token(evaluation.verdict));
  text.append(" plan=");
  text.append(to_string(evaluation.plan));
  text.append(" revision=");
  text.append(to_string(evaluation.revision));
  text.append(" epoch=");
  text.append(to_string(evaluation.epoch));
  text.append(" required-domains=");
  text.append(std::to_string(evaluation.required_domain_count));
  text.append(" primary=");
  text.append(to_token(evaluation.primary_blocking_code));
  text.push_back('\n');
  text.append("domains ");
  text.append(domain_summary(evaluation));
  text.push_back('\n');
  text.append("residuals open=");
  text.append(std::to_string(evaluation.open_residuals));
  text.append(" unknown=");
  text.append(std::to_string(evaluation.unknown_residuals));
  text.append(" fenced=");
  text.append(evaluation.fenced ? "yes" : "no");
  text.append(" floor=");
  text.append(to_string(evaluation.fence_floor));
  text.push_back('\n');
  text.append("generations ");
  text.append(evaluation.generations.to_canonical());
  text.push_back('\n');
  text.append("evidence ");
  text.append(evaluation.evidence_digest.to_hex());
  text.push_back('\n');
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const DomainAssessment& assessment = evaluation.domains[index];
    text.append("domain ");
    text.append(to_token(assessment.domain));
    text.append(" verdict=");
    text.append(to_token(assessment.verdict));
    text.append(" blocking=");
    text.append(to_token(assessment.blocking_code));
    text.append(" required=");
    text.append(assessment.required ? "yes" : "no");
    text.append(" reason=");
    text.append(assessment.reason);
    text.push_back('\n');
  }
  if (!evaluation.blocking_codes.empty()) {
    text.append("blocking");
    for (const ErrorCode code : evaluation.blocking_codes) {
      text.push_back(' ');
      text.append(to_token(code));
    }
    text.push_back('\n');
  }
  return text;
}

}  // namespace facilitydrain
