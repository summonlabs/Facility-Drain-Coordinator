// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_IDENTITY_HPP
#define FACILITYDRAIN_IDENTITY_HPP

#include "facilitydrain/export.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Strong value types
// ---------------------------------------------------------------------------
//
// Every identity, counter and generation in this system gets a distinct type.
// The doctrine is blunt: observation is not authority, requested state is not
// observed state, and a revision is not a generation. Silently swapping two of
// those at a call site is exactly the class of defect this system exists to
// prevent, so the compiler is enlisted to make it impossible rather than merely
// discouraged.
//
// Tags are *never* interchangeable: each tag names its own type, so two strong
// types with the same underlying representation do not implicitly convert.

template <typename Tag>
class StrongValue {
 public:
  using value_type = std::uint64_t;

  constexpr StrongValue() noexcept = default;
  constexpr explicit StrongValue(std::uint64_t raw) noexcept : raw_(raw) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return raw_; }

  // The identity provided by default construction. Every domain type states
  // explicitly whether this value is meaningful, because "zero" means different
  // things for an identifier than it does for a generation.
  [[nodiscard]] constexpr bool is_default() const noexcept { return raw_ == 0; }

  friend constexpr bool operator==(StrongValue a, StrongValue b) noexcept {
    return a.raw_ == b.raw_;
  }
  friend constexpr bool operator!=(StrongValue a, StrongValue b) noexcept {
    return a.raw_ != b.raw_;
  }
  friend constexpr bool operator<(StrongValue a, StrongValue b) noexcept {
    return a.raw_ < b.raw_;
  }
  friend constexpr bool operator>(StrongValue a, StrongValue b) noexcept {
    return a.raw_ > b.raw_;
  }
  friend constexpr bool operator<=(StrongValue a, StrongValue b) noexcept {
    return a.raw_ <= b.raw_;
  }
  friend constexpr bool operator>=(StrongValue a, StrongValue b) noexcept {
    return a.raw_ >= b.raw_;
  }

  // Generations and sequences are advanced, never assigned arbitrarily. The
  // guard against wrapping is deliberate: a wrapped counter would silently
  // resurrect stale authority.
  [[nodiscard]] constexpr StrongValue next() const noexcept {
    return StrongValue(raw_ + 1U);
  }

  [[nodiscard]] static constexpr StrongValue max() noexcept {
    return StrongValue((std::numeric_limits<std::uint64_t>::max)());
  }

 private:
  std::uint64_t raw_ = 0;
};

// Domain-specific tag namespaces. Each is a complete type, which is what makes
// the strong types mutually incompatible rather than merely differently named.
namespace tags {
struct AssetIdTag {};
struct RackIdTag {};
struct ZoneIdTag {};
struct SiteIdTag {};
struct SubscopeIdTag {};
struct IncarnationIdTag {};
struct PlanIdTag {};
struct AttemptIdTag {};
struct ObligationIdTag {};
struct DrainRequestIdTag {};
struct EvidenceIdTag {};
struct PolicyIdTag {};

struct LifecycleGenerationTag {};
struct HardwareGenerationTag {};
struct FirmwareGenerationTag {};
struct ControlEpochTag {};
struct RevisionTag {};
struct CommitSequenceTag {};
struct ObservationSequenceTag {};
struct PolicyGenerationTag {};
struct DependencyGenerationTag {};
struct CapacityGenerationTag {};
struct TopologyGenerationTag {};
struct MaintenanceGenerationTag {};
struct ScopeGenerationTag {};
struct ObligationGenerationTag {};
struct ReservationGenerationTag {};
struct EvidenceGenerationTag {};
}  // namespace tags

// Physical and logical identities.
using AssetId = StrongValue<tags::AssetIdTag>;
using RackId = StrongValue<tags::RackIdTag>;
using ZoneId = StrongValue<tags::ZoneIdTag>;
using SiteId = StrongValue<tags::SiteIdTag>;
using SubscopeId = StrongValue<tags::SubscopeIdTag>;
using IncarnationId = StrongValue<tags::IncarnationIdTag>;
using PlanId = StrongValue<tags::PlanIdTag>;
using AttemptId = StrongValue<tags::AttemptIdTag>;
using ObligationId = StrongValue<tags::ObligationIdTag>;
using DrainRequestId = StrongValue<tags::DrainRequestIdTag>;
using EvidenceId = StrongValue<tags::EvidenceIdTag>;
using PolicyId = StrongValue<tags::PolicyIdTag>;

// Counters and generations. These advance in one direction only and are the
// basis for fencing: a stale generator can never out-rank a live one.
using LifecycleGeneration = StrongValue<tags::LifecycleGenerationTag>;
using HardwareGeneration = StrongValue<tags::HardwareGenerationTag>;
using FirmwareGeneration = StrongValue<tags::FirmwareGenerationTag>;
using ControlEpoch = StrongValue<tags::ControlEpochTag>;
using Revision = StrongValue<tags::RevisionTag>;
using CommitSequence = StrongValue<tags::CommitSequenceTag>;
using ObservationSequence = StrongValue<tags::ObservationSequenceTag>;
using PolicyGeneration = StrongValue<tags::PolicyGenerationTag>;
using DependencyGeneration = StrongValue<tags::DependencyGenerationTag>;
using CapacityGeneration = StrongValue<tags::CapacityGenerationTag>;
using TopologyGeneration = StrongValue<tags::TopologyGenerationTag>;
using MaintenanceGeneration = StrongValue<tags::MaintenanceGenerationTag>;
using ScopeGeneration = StrongValue<tags::ScopeGenerationTag>;
using ObligationGeneration = StrongValue<tags::ObligationGenerationTag>;
using ReservationGeneration = StrongValue<tags::ReservationGenerationTag>;
using EvidenceGeneration = StrongValue<tags::EvidenceGenerationTag>;

// ---------------------------------------------------------------------------
// Strict canonical text form
// ---------------------------------------------------------------------------
//
// Identities cross a process boundary as text in the CLI and as fixed-width
// little-endian bytes on the wire. Both forms are canonical: exactly one text
// spelling round-trips, and anything else is rejected rather than repaired.

enum class IdentityParseError {
  kOk = 0,
  kEmpty,
  kLeadingWhitespace,
  kTrailingWhitespace,
  kInvalidCharacter,
  kOverflow,
  kNegativeSign,
  kHexWithoutDigits,
  kDecimalLeadingZero,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_string(IdentityParseError error) noexcept;

// Decimal rendering of the raw value. Never zero-padded, never signed.
[[nodiscard]] FACILITYDRAIN_API std::string format_strong(std::uint64_t raw);

// Parses a canonical unsigned decimal form only.
[[nodiscard]] FACILITYDRAIN_API IdentityParseError parse_decimal(std::string_view text,
                                                                 std::uint64_t& out) noexcept;

// Parses either canonical decimal or "0x"-prefixed canonical lowercase hex.
[[nodiscard]] FACILITYDRAIN_API IdentityParseError parse_strong(std::string_view text,
                                                                std::uint64_t& out) noexcept;

// Generic typed helpers so call sites never juggle raw integers.
template <typename Tag>
[[nodiscard]] std::string to_string(StrongValue<Tag> value) {
  return format_strong(value.value());
}

// Explicit, failure-reporting parser. Deliberately not implicit: a malformed
// identity is an error, not a default-constructed value.
template <typename Tag>
[[nodiscard]] std::optional<StrongValue<Tag>> try_parse(std::string_view text,
                                                        IdentityParseError& error) noexcept {
  std::uint64_t raw = 0;
  error = parse_strong(text, raw);
  if (error != IdentityParseError::kOk) {
    return std::nullopt;
  }
  return StrongValue<Tag>(raw);
}

}  // namespace facilitydrain

// Hash support so strong types can key unordered containers without callers
// ever unwrapping the raw value by hand.
namespace std {
template <typename Tag>
struct hash<facilitydrain::StrongValue<Tag>> {
  [[nodiscard]] std::size_t operator()(facilitydrain::StrongValue<Tag> value) const noexcept {
    return std::hash<std::uint64_t>{}(value.value());
  }
};
}  // namespace std

#endif // FACILITYDRAIN_IDENTITY_HPP
