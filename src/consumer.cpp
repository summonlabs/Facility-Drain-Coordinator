// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/consumer.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {

namespace {

[[nodiscard]] bool is_ascii_space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

// Ordering used when a manifest is walked for validation. The obligation id is
// the primary key, which is what makes the reported primary error independent
// of the order the owner listed its records in. The remaining fields break ties
// (only possible with duplicate obligation ids, which are reported separately)
// so that even a duplicated manifest is walked in one fixed order rather than
// in listing order.
[[nodiscard]] bool obligation_order(const ConsumerRecord& lhs, const ConsumerRecord& rhs) noexcept {
  if (lhs.obligation != rhs.obligation) {
    return lhs.obligation < rhs.obligation;
  }
  if (lhs.category != rhs.category) {
    return static_cast<std::uint8_t>(lhs.category) < static_cast<std::uint8_t>(rhs.category);
  }
  if (lhs.generation != rhs.generation) {
    return lhs.generation < rhs.generation;
  }
  if (lhs.reservation != rhs.reservation) {
    return lhs.reservation < rhs.reservation;
  }
  if (lhs.strength != rhs.strength) {
    return static_cast<std::uint8_t>(lhs.strength) < static_cast<std::uint8_t>(rhs.strength);
  }
  if (lhs.label != rhs.label) {
    return lhs.label < rhs.label;
  }
  return lhs.source < rhs.source;
}

}  // namespace

std::string_view to_token(OwnerDomain domain) noexcept {
  switch (domain) {
    case OwnerDomain::kAsi:
      return "asi";
    case OwnerDomain::kDfi:
      return "dfi";
    case OwnerDomain::kFacility:
      return "facility";
    case OwnerDomain::kMonitoring:
      return "monitoring";
  }
  return std::string_view{};
}

std::optional<OwnerDomain> owner_domain_from_token(std::string_view token) noexcept {
  if (token == "asi") {
    return OwnerDomain::kAsi;
  }
  if (token == "dfi") {
    return OwnerDomain::kDfi;
  }
  if (token == "facility") {
    return OwnerDomain::kFacility;
  }
  if (token == "monitoring") {
    return OwnerDomain::kMonitoring;
  }
  return std::nullopt;
}

OwnerDomain owner_domain_at(std::uint32_t index) noexcept {
  // The index space is the canonical domain order. An index outside it names no
  // domain; ASI is returned because the function is total, and no caller relies
  // on that fallback: every caller passes a value produced by domain_index.
  if (index >= kOwnerDomainCount) {
    return OwnerDomain::kAsi;
  }
  return static_cast<OwnerDomain>(index + 1U);
}

std::string_view to_token(ConsumerCategory category) noexcept {
  switch (category) {
    case ConsumerCategory::kWorkload:
      return "workload";
    case ConsumerCategory::kAcceleratorReservation:
      return "accelerator-reservation";
    case ConsumerCategory::kNetworkPath:
      return "network-path";
    case ConsumerCategory::kNetworkAttachment:
      return "network-attachment";
    case ConsumerCategory::kServiceClass:
      return "service-class";
    case ConsumerCategory::kMaintenanceProtection:
      return "maintenance-protection";
    case ConsumerCategory::kFacilityReservation:
      return "facility-reservation";
    case ConsumerCategory::kMonitoringDependency:
      return "monitoring-dependency";
  }
  return std::string_view{};
}

std::optional<ConsumerCategory> consumer_category_from_token(std::string_view token) noexcept {
  if (token == "workload") {
    return ConsumerCategory::kWorkload;
  }
  if (token == "accelerator-reservation") {
    return ConsumerCategory::kAcceleratorReservation;
  }
  if (token == "network-path") {
    return ConsumerCategory::kNetworkPath;
  }
  if (token == "network-attachment") {
    return ConsumerCategory::kNetworkAttachment;
  }
  if (token == "service-class") {
    return ConsumerCategory::kServiceClass;
  }
  if (token == "maintenance-protection") {
    return ConsumerCategory::kMaintenanceProtection;
  }
  if (token == "facility-reservation") {
    return ConsumerCategory::kFacilityReservation;
  }
  if (token == "monitoring-dependency") {
    return ConsumerCategory::kMonitoringDependency;
  }
  return std::nullopt;
}

OwnerDomain domain_of(ConsumerCategory category) noexcept {
  // Total and fixed: a category has exactly one owner, and that mapping is what
  // turns a category/domain disagreement into a conflicting field instead of a
  // judgement call. An out of range category has no owner; ASI is returned
  // because the function is total, and every caller validates the category
  // before it consults the owner.
  switch (category) {
    case ConsumerCategory::kWorkload:
    case ConsumerCategory::kAcceleratorReservation:
    case ConsumerCategory::kServiceClass:
      return OwnerDomain::kAsi;
    case ConsumerCategory::kNetworkPath:
    case ConsumerCategory::kNetworkAttachment:
      return OwnerDomain::kDfi;
    case ConsumerCategory::kMaintenanceProtection:
    case ConsumerCategory::kFacilityReservation:
      return OwnerDomain::kFacility;
    case ConsumerCategory::kMonitoringDependency:
      return OwnerDomain::kMonitoring;
  }
  return OwnerDomain::kAsi;
}

std::string_view to_token(ObligationStrength strength) noexcept {
  switch (strength) {
    case ObligationStrength::kMandatory:
      return "mandatory";
    case ObligationStrength::kAdvisory:
      return "advisory";
  }
  return std::string_view{};
}

std::optional<ObligationStrength> obligation_strength_from_token(std::string_view token) noexcept {
  if (token == "mandatory") {
    return ObligationStrength::kMandatory;
  }
  if (token == "advisory") {
    return ObligationStrength::kAdvisory;
  }
  return std::nullopt;
}

std::optional<DomainMask> DomainMask::from_bits(std::uint8_t bits) noexcept {
  // An unknown bit is an unknown requirement, and an unknown requirement is
  // never dropped, so a mask that sets one is refused rather than masked off.
  constexpr std::uint8_t kKnownBits = static_cast<std::uint8_t>((1U << kOwnerDomainCount) - 1U);
  if ((bits & static_cast<std::uint8_t>(~kKnownBits)) != 0) {
    return std::nullopt;
  }
  return DomainMask{bits};
}

std::string DomainMask::to_canonical() const {
  if (bits_ == 0) {
    return "none";
  }
  std::string text;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    if ((bits_ & static_cast<std::uint8_t>(1U << index)) == 0) {
      continue;
    }
    if (!text.empty()) {
      text += ',';
    }
    text += to_token(owner_domain_at(index));
  }
  return text;
}

Result<DomainMask> DomainMask::parse(std::string_view text) {
  if (text.empty()) {
    return make_error<DomainMask>(ErrorCode::kInvalidArgument, "domain mask text is empty");
  }
  // The two shorthands a mask cannot spell as a token list: nothing required,
  // and everything required.
  if (text == "none") {
    return DomainMask::none();
  }
  if (text == "all") {
    return DomainMask::all();
  }
  DomainMask mask = DomainMask::none();
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = text.find(',', start);
    const std::string_view element =
        comma == std::string_view::npos ? text.substr(start) : text.substr(start, comma - start);
    if (element.empty()) {
      return make_error<DomainMask>(ErrorCode::kInvalidText, "domain mask contains an empty domain token");
    }
    // Whitespace is never trimmed and never ignored: " asi" and "asi" would
    // otherwise be two spellings of one mask, and a canonical form has exactly
    // one spelling.
    if (is_ascii_space(element.front()) || is_ascii_space(element.back())) {
      std::string detail = "domain token '";
      detail += element;
      detail += "' has surrounding whitespace";
      return make_error<DomainMask>(ErrorCode::kInvalidText, detail);
    }
    const std::optional<OwnerDomain> domain = owner_domain_from_token(element);
    if (!domain.has_value()) {
      std::string detail = "unknown domain token '";
      detail += element;
      detail += "'";
      return make_error<DomainMask>(ErrorCode::kInvalidEnumValue, detail);
    }
    if (mask.contains(*domain)) {
      std::string detail = "duplicate domain token '";
      detail += element;
      detail += "'";
      return make_error<DomainMask>(ErrorCode::kDuplicateIdentifier, detail);
    }
    mask = mask.with(*domain);
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return mask;
}

Status ConsumerRecord::validate(const Limits& limits) const {
  // Fixed order: identity, category, generation, strength, then the text
  // fields. Validation stops at the first violation, so a record with several
  // problems always reports the same primary code.
  if (obligation.value() == 0) {
    return Status::failure(ErrorCode::kInvalidIdentity, "obligation id 0 is not an identity");
  }
  if (to_token(category).empty()) {
    std::string detail = "unknown consumer category ";
    detail += format_strong(static_cast<std::uint64_t>(category));
    return Status::failure(ErrorCode::kInvalidEnumValue, detail);
  }
  if (generation.value() == 0) {
    // A default generation means never observed. Treating it as a real
    // generation would let an unobserved obligation satisfy evidence.
    return Status::failure(ErrorCode::kMissingRequiredField, "obligation generation 0 means never observed");
  }
  if (to_token(strength).empty()) {
    std::string detail = "unknown obligation strength ";
    detail += format_strong(static_cast<std::uint64_t>(strength));
    return Status::failure(ErrorCode::kInvalidEnumValue, detail);
  }
  const Status label_status = detail::validate_text(label, "label", limits.max_text_bytes);
  if (!label_status.ok()) {
    return label_status;
  }
  return detail::validate_text(source, "source", limits.max_text_bytes);
}

std::string ConsumerRecord::to_canonical() const {
  // Every field the record is compared and hashed by appears here, in one fixed
  // order, and the label and source are included as the exact bytes the owner
  // supplied: no normalisation, because the digest must cover what was said.
  std::string text(to_token(domain_of(category)));
  text += ':';
  text += to_string(obligation);
  text += ' ';
  text += to_token(category);
  text += " gen=";
  text += to_string(generation);
  text += " res=";
  text += to_string(reservation);
  text += ' ';
  text += to_token(strength);
  text += " label=";
  text += label;
  text += " source=";
  text += source;
  return text;
}

ContentDigest consumer_manifest_digest(OwnerDomain domain, std::span<const ConsumerRecord> records) {
  // Only this domain's records participate, and they are hashed in canonical
  // text order rather than listing order, so two owners that describe the same
  // obligations in different orders produce the same digest. An empty set still
  // has a digest: absence of records is a statement, not a missing answer.
  std::vector<std::string> canonical;
  canonical.reserve(records.size());
  for (const ConsumerRecord& record : records) {
    if (domain_of(record.category) != domain) {
      continue;
    }
    canonical.push_back(record.to_canonical());
  }
  std::sort(canonical.begin(), canonical.end());
  std::vector<ContentDigest> digests;
  digests.reserve(canonical.size());
  for (const std::string& text : canonical) {
    digests.push_back(digest_text(text));
  }
  // The domain is part of the domain separation tag. Without it, an
  // enumeration for one owning system would have the same digest as an
  // identical-looking enumeration for another, and evidence could be replayed
  // across systems that never agreed about anything.
  std::string tag = "consumer-manifest:";
  tag += to_token(domain);
  return combine_digests(tag, digests);
}

Status validate_domain_manifest(OwnerDomain domain, std::span<const ConsumerRecord> records, const Limits& limits) {
  // 1. The bound is checked first: rejecting an over large manifest needs no
  //    knowledge of its contents.
  if (records.size() > static_cast<std::size_t>(limits.max_consumers_per_domain)) {
    std::string detail = "domain manifest has ";
    detail += format_strong(static_cast<std::uint64_t>(records.size()));
    detail += " records, limit is ";
    detail += format_strong(static_cast<std::uint64_t>(limits.max_consumers_per_domain));
    return Status::failure(ErrorCode::kTooManyEntries, detail);
  }
  // 2. Everything after this walk is done in the manifest's own fixed order, so
  //    a given set of records reports the same primary error however the owner
  //    happened to list them.
  std::vector<const ConsumerRecord*> ordered;
  ordered.reserve(records.size());
  for (const ConsumerRecord& record : records) {
    ordered.push_back(&record);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const ConsumerRecord* lhs, const ConsumerRecord* rhs) noexcept { return obligation_order(*lhs, *rhs); });
  for (const ConsumerRecord* record : ordered) {
    const Status status = record->validate(limits);
    if (!status.ok()) {
      return status;
    }
  }
  // 3. A category that belongs to another domain is a disagreement about who
  //    owns the obligation, which is never resolved by preferring one side.
  for (const ConsumerRecord* record : ordered) {
    const OwnerDomain owner = domain_of(record->category);
    if (owner != domain) {
      std::string detail = "obligation ";
      detail += to_string(record->obligation);
      detail += " has category '";
      detail += to_token(record->category);
      detail += "' owned by ";
      detail += to_token(owner);
      detail += ", but this manifest is for ";
      detail += to_token(domain);
      return Status::failure(ErrorCode::kConflictingField, detail);
    }
  }
  // 4. Duplicates are adjacent in the sorted walk. One obligation cannot be
  //    declared twice: the second record would silently replace the first in
  //    every digest and verdict that follows.
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    if (ordered[index]->obligation == ordered[index - 1]->obligation) {
      std::string detail = "duplicate obligation ";
      detail += to_string(ordered[index]->obligation);
      detail += " in the ";
      detail += to_token(domain);
      detail += " manifest";
      return Status::failure(ErrorCode::kDuplicateIdentifier, detail);
    }
  }
  return Status::success();
}

DomainMask required_domains_for(DomainMask declared, std::span<const ConsumerRecord> records) {
  // The declared requirement can never be narrowed by what the records happen
  // to say: a mandatory obligation adds its owner to the requirement, and
  // nothing removes a domain the operator declared.
  DomainMask required = declared;
  for (const ConsumerRecord& record : records) {
    if (record.strength == ObligationStrength::kMandatory) {
      required = required.with(domain_of(record.category));
    }
  }
  return required;
}

}  // namespace facilitydrain
