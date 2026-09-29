// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/residual.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {

namespace {

// ---------------------------------------------------------------------------
// Distinct obligation counting
// ---------------------------------------------------------------------------
//
// The ledger queries are noexcept, so they must not allocate: a count is
// computed by walking the entries, never by building a set of ids. The ledger
// is normally held in canonical order, where the entries of one obligation are
// contiguous, so that case costs one linear pass. A ledger that is not in
// canonical order is still answered correctly by a scan that only counts an
// obligation the first time it appears, rather than reporting a number that
// depends on how the entries happened to be listed.
[[nodiscard]] bool is_in_canonical_order(const std::vector<ResidualEntry>& entries) noexcept {
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (entries[index] < entries[index - 1]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint32_t count_distinct_obligations(const std::vector<ResidualEntry>& entries,
                                                       OwnerDomain domain,
                                                       bool relinquished_only) noexcept {
  std::uint32_t total = 0;
  if (is_in_canonical_order(entries)) {
    std::size_t index = 0;
    while (index < entries.size()) {
      const ResidualEntry& first = entries[index];
      std::size_t end = index;
      bool qualifies = false;
      while (end < entries.size() && entries[end].domain == first.domain &&
             entries[end].obligation == first.obligation) {
        if (!relinquished_only || entries[end].state == ResidualState::kRelinquished) {
          qualifies = true;
        }
        ++end;
      }
      // Obligation 0 is the domain wide entry, not an obligation, so it is
      // never counted as one.
      if (first.domain == domain && first.obligation.value() != 0 && qualifies) {
        ++total;
      }
      index = end;
    }
    return total;
  }
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const ResidualEntry& entry = entries[index];
    if (entry.domain != domain || entry.obligation.value() == 0) {
      continue;
    }
    if (relinquished_only && entry.state != ResidualState::kRelinquished) {
      continue;
    }
    // Only an earlier entry that itself qualifies makes this one a duplicate:
    // an entry that does not satisfy the filter says nothing about whether the
    // obligation reached the state being counted.
    bool already_counted = false;
    for (std::size_t earlier = 0; earlier < index && !already_counted; ++earlier) {
      if (relinquished_only && entries[earlier].state != ResidualState::kRelinquished) {
        continue;
      }
      already_counted = entries[earlier].domain == entry.domain && entries[earlier].obligation == entry.obligation;
    }
    if (!already_counted) {
      ++total;
    }
  }
  return total;
}

}  // namespace

std::string_view to_token(ResidualKind kind) noexcept {
  switch (kind) {
    case ResidualKind::kObligationActive:
      return "obligation-active";
    case ResidualKind::kObligationUnknown:
      return "obligation-unknown";
    case ResidualKind::kOwnerRefused:
      return "owner-refused";
    case ResidualKind::kEnumerationIncomplete:
      return "enumeration-incomplete";
    case ResidualKind::kEvidenceMissing:
      return "evidence-missing";
    case ResidualKind::kResidualCountUnknown:
      return "residual-count-unknown";
    case ResidualKind::kRequestUnacknowledged:
      return "request-unacknowledged";
    case ResidualKind::kDomainFailed:
      return "domain-failed";
    case ResidualKind::kEvidenceStale:
      return "evidence-stale";
    case ResidualKind::kProtectedObligation:
      return "protected-obligation";
  }
  return std::string_view{};
}

std::optional<ResidualKind> residual_kind_from_token(std::string_view token) noexcept {
  if (token == "obligation-active") {
    return ResidualKind::kObligationActive;
  }
  if (token == "obligation-unknown") {
    return ResidualKind::kObligationUnknown;
  }
  if (token == "owner-refused") {
    return ResidualKind::kOwnerRefused;
  }
  if (token == "enumeration-incomplete") {
    return ResidualKind::kEnumerationIncomplete;
  }
  if (token == "evidence-missing") {
    return ResidualKind::kEvidenceMissing;
  }
  if (token == "residual-count-unknown") {
    return ResidualKind::kResidualCountUnknown;
  }
  if (token == "request-unacknowledged") {
    return ResidualKind::kRequestUnacknowledged;
  }
  if (token == "domain-failed") {
    return ResidualKind::kDomainFailed;
  }
  if (token == "evidence-stale") {
    return ResidualKind::kEvidenceStale;
  }
  if (token == "protected-obligation") {
    return ResidualKind::kProtectedObligation;
  }
  return std::nullopt;
}

bool is_unknown_residual_kind(ResidualKind kind) noexcept {
  // These four mean "the answer is unknown", not "the answer is bad". An
  // unknown answer is counted separately and always blocks a safe to remove
  // verdict, because nobody has established that the obligation is gone.
  switch (kind) {
    case ResidualKind::kObligationUnknown:
    case ResidualKind::kEnumerationIncomplete:
    case ResidualKind::kEvidenceMissing:
    case ResidualKind::kResidualCountUnknown:
      return true;
    case ResidualKind::kObligationActive:
    case ResidualKind::kOwnerRefused:
    case ResidualKind::kRequestUnacknowledged:
    case ResidualKind::kDomainFailed:
    case ResidualKind::kEvidenceStale:
    case ResidualKind::kProtectedObligation:
      return false;
  }
  return false;
}

std::string_view to_token(ResidualState state) noexcept {
  switch (state) {
    case ResidualState::kOpen:
      return "open";
    case ResidualState::kRelinquished:
      return "relinquished";
    case ResidualState::kSuperseded:
      return "superseded";
  }
  return std::string_view{};
}

std::optional<ResidualState> residual_state_from_token(std::string_view token) noexcept {
  if (token == "open") {
    return ResidualState::kOpen;
  }
  if (token == "relinquished") {
    return ResidualState::kRelinquished;
  }
  if (token == "superseded") {
    return ResidualState::kSuperseded;
  }
  return std::nullopt;
}

std::string ResidualEntry::to_canonical() const {
  std::string text(to_token(domain));
  text += ':';
  // Obligation 0 is the domain wide entry: it is about the domain's coverage or
  // its evidence rather than one obligation, so it is rendered as the word
  // "scope" instead of an identity that no obligation may carry.
  if (obligation.value() == 0) {
    text += "scope";
  } else {
    text += to_string(obligation);
  }
  text += ' ';
  text += to_token(kind);
  text += " state=";
  text += to_token(state);
  text += " gen=";
  text += to_string(generation);
  text += " detail=";
  text += detail;
  return text;
}

bool ResidualEntry::operator<(const ResidualEntry& other) const noexcept {
  // Fixed field order: the domain first, so an entry's canonical position is
  // grouped by owner, then the obligation, the kind, the generation and finally
  // the state.
  const std::uint32_t domain_key = domain_index(domain);
  const std::uint32_t other_domain_key = domain_index(other.domain);
  if (domain_key != other_domain_key) {
    return domain_key < other_domain_key;
  }
  if (obligation != other.obligation) {
    return obligation < other.obligation;
  }
  if (kind != other.kind) {
    return static_cast<std::uint8_t>(kind) < static_cast<std::uint8_t>(other.kind);
  }
  if (generation != other.generation) {
    return generation < other.generation;
  }
  return static_cast<std::uint8_t>(state) < static_cast<std::uint8_t>(other.state);
}

bool ResidualEntry::operator==(const ResidualEntry& other) const noexcept {
  // This is an IDENTITY comparison, not a full record comparison. The ledger is
  // keyed by (domain, obligation, kind): one obligation has at most one entry
  // of a given kind per domain, so a second record for the same identity
  // REPLACES the existing entry (its state, generation, resolution and detail
  // are updated) rather than being appended as a duplicate. The other fields
  // are the entry's current values, not part of what makes it that entry.
  return domain == other.domain && obligation == other.obligation && kind == other.kind;
}

std::uint32_t ResidualLedger::open_count() const noexcept {
  std::uint32_t total = 0;
  for (const ResidualEntry& entry : entries) {
    if (entry.state == ResidualState::kOpen) {
      ++total;
    }
  }
  return total;
}

std::uint32_t ResidualLedger::open_count(OwnerDomain domain) const noexcept {
  std::uint32_t total = 0;
  for (const ResidualEntry& entry : entries) {
    if (entry.state == ResidualState::kOpen && entry.domain == domain) {
      ++total;
    }
  }
  return total;
}

std::uint32_t ResidualLedger::unknown_count() const noexcept {
  std::uint32_t total = 0;
  for (const ResidualEntry& entry : entries) {
    // Only open entries count: a resolved entry is an answer, even when the
    // kind it was recorded under was an unknown one.
    if (entry.state == ResidualState::kOpen && is_unknown_residual_kind(entry.kind)) {
      ++total;
    }
  }
  return total;
}

std::uint32_t ResidualLedger::unknown_count(OwnerDomain domain) const noexcept {
  std::uint32_t total = 0;
  for (const ResidualEntry& entry : entries) {
    if (entry.state == ResidualState::kOpen && entry.domain == domain && is_unknown_residual_kind(entry.kind)) {
      ++total;
    }
  }
  return total;
}

std::uint32_t ResidualLedger::known_obligation_count(OwnerDomain domain) const noexcept {
  return count_distinct_obligations(entries, domain, false);
}

std::uint32_t ResidualLedger::relinquished_obligation_count(OwnerDomain domain) const noexcept {
  return count_distinct_obligations(entries, domain, true);
}

bool ResidualLedger::has_open(OwnerDomain domain) const noexcept {
  for (const ResidualEntry& entry : entries) {
    if (entry.state == ResidualState::kOpen && entry.domain == domain) {
      return true;
    }
  }
  return false;
}

}  // namespace facilitydrain
