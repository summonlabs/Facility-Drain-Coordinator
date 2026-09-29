// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_CONSUMER_HPP
#define FACILITYDRAIN_CONSUMER_HPP

#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Consumers and obligations
// ---------------------------------------------------------------------------
//
// A consumer is something that currently depends on the physical scope: a
// workload, an accelerator reservation, a network path or attachment, a service
// class, a maintenance protection, a facility reservation, or a monitoring or
// control dependency. Each one is owned by exactly one domain, and that domain
// is the only authority that can say the consumer is gone.
//
// The coordinator never removes a consumer itself. It records what the owner
// declared, what the owner has proven, and what remains.

enum class OwnerDomain : std::uint8_t {
  kAsi = 1,
  kDfi = 2,
  kFacility = 3,
  kMonitoring = 4,
};

inline constexpr std::uint32_t kOwnerDomainCount = 4;
inline constexpr std::uint32_t kOwnerDomainIndexMask = 0xFFU;

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(OwnerDomain domain) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<OwnerDomain> owner_domain_from_token(std::string_view token) noexcept;
/// 0 based index in the canonical domain order; the index used by reports.
[[nodiscard]] constexpr std::uint32_t domain_index(OwnerDomain domain) noexcept {
  return static_cast<std::uint32_t>(domain) - 1U;
}
/// The inverse. An out of range index is ASI, which no caller may rely on: the
/// only callers pass an index produced by domain_index.
[[nodiscard]] FACILITYDRAIN_API OwnerDomain owner_domain_at(std::uint32_t index) noexcept;

enum class ConsumerCategory : std::uint8_t {
  kWorkload = 1,
  kAcceleratorReservation = 2,
  kNetworkPath = 3,
  kNetworkAttachment = 4,
  kServiceClass = 5,
  kMaintenanceProtection = 6,
  kFacilityReservation = 7,
  kMonitoringDependency = 8,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(ConsumerCategory category) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<ConsumerCategory> consumer_category_from_token(
    std::string_view token) noexcept;
/// The domain that owns consumers of this category. This mapping is total and
/// fixed: a workload can only ever be an ASI obligation.
[[nodiscard]] FACILITYDRAIN_API OwnerDomain domain_of(ConsumerCategory category) noexcept;

/// Whether an obligation must be relinquished before the scope can be declared
/// safe to remove. Advisory obligations are recorded and reported, but they do
/// not by themselves block: the policy that decides that is the plan policy,
/// and this flag is the operator's declaration inside that policy.
enum class ObligationStrength : std::uint8_t {
  kMandatory = 1,
  kAdvisory = 2,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(ObligationStrength strength) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<ObligationStrength> obligation_strength_from_token(
    std::string_view token) noexcept;

/// An unordered set of domains, used to declare which domains a plan must have
/// proven before it can be declared safe to remove.
class DomainMask {
 public:
  constexpr DomainMask() noexcept = default;

  [[nodiscard]] static constexpr DomainMask of(OwnerDomain domain) noexcept {
    return DomainMask{static_cast<std::uint8_t>(1U << domain_index(domain))};
  }
  [[nodiscard]] static constexpr DomainMask none() noexcept { return DomainMask{}; }
  [[nodiscard]] static constexpr DomainMask all() noexcept {
    return DomainMask{static_cast<std::uint8_t>((1U << kOwnerDomainCount) - 1U)};
  }

  /// Rejects any bit outside the four known domains: an unknown bit is an
  /// unknown requirement, and unknown requirements are never dropped.
  [[nodiscard]] static std::optional<DomainMask> from_bits(std::uint8_t bits) noexcept;

  [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }
  [[nodiscard]] constexpr bool contains(OwnerDomain domain) const noexcept {
    return (bits_ & static_cast<std::uint8_t>(1U << domain_index(domain))) != 0;
  }
  [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
  [[nodiscard]] constexpr std::uint32_t count() const noexcept {
    std::uint32_t total = 0;
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      if ((bits_ & static_cast<std::uint8_t>(1U << index)) != 0) {
        ++total;
      }
    }
    return total;
  }
  [[nodiscard]] constexpr DomainMask with(OwnerDomain domain) const noexcept {
    return DomainMask{static_cast<std::uint8_t>(bits_ | static_cast<std::uint8_t>(1U << domain_index(domain)))};
  }
  [[nodiscard]] constexpr DomainMask without(OwnerDomain domain) const noexcept {
    return DomainMask{static_cast<std::uint8_t>(bits_ & ~static_cast<std::uint8_t>(1U << domain_index(domain)))};
  }

  /// "asi,dfi" in canonical domain order, or "none".
  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
  /// Strict parse of the canonical form. Also accepts "all" and "none".
  [[nodiscard]] FACILITYDRAIN_API static Result<DomainMask> parse(std::string_view text);

  friend constexpr bool operator==(DomainMask lhs, DomainMask rhs) noexcept { return lhs.bits_ == rhs.bits_; }
  friend constexpr bool operator!=(DomainMask lhs, DomainMask rhs) noexcept { return lhs.bits_ != rhs.bits_; }
  friend constexpr bool operator<(DomainMask lhs, DomainMask rhs) noexcept { return lhs.bits_ < rhs.bits_; }

 private:
  explicit constexpr DomainMask(std::uint8_t bits) noexcept : bits_(bits) {}
  std::uint8_t bits_ = 0;
};

/// One declared obligation held by one consumer of the physical scope.
///
/// The record carries the owner's obligation generation and, when the
/// obligation is backed by a reservation, the reservation generation. Both are
/// part of the consumer manifest digest, so an obligation that is silently
/// re-generated under a new generation is a different obligation and cannot
/// satisfy evidence taken against the old one.
struct ConsumerRecord {
  ObligationId obligation{};
  ConsumerCategory category = ConsumerCategory::kWorkload;
  ObligationGeneration generation{};
  ReservationGeneration reservation{};
  ObligationStrength strength = ObligationStrength::kMandatory;
  std::string label{};
  std::string source{};

  [[nodiscard]] OwnerDomain domain() const noexcept { return domain_of(category); }
  [[nodiscard]] FACILITYDRAIN_API Status validate(const Limits& limits) const;
  /// "asi:1001 workload gen=7 res=0 mandatory label=..."
  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
  friend bool operator<(const ConsumerRecord& lhs, const ConsumerRecord& rhs) noexcept {
    return lhs.to_canonical() < rhs.to_canonical();
  }
};

/// The digest of one domain's consumer manifest.
///
/// Only records that belong to the requested domain participate, the domain tag
/// is mixed in, and the records are hashed in canonical order, so the digest is
/// independent of the order in which the owner listed them. No normalisation of
/// text is performed: the digest covers exactly the bytes the owner supplied.
[[nodiscard]] FACILITYDRAIN_API ContentDigest consumer_manifest_digest(
    OwnerDomain domain, std::span<const ConsumerRecord> records);

/// Rejects a domain manifest that contains a duplicate obligation id, an
/// obligation whose category belongs to a different domain, an out of range
/// text field, or more records than the limit allows. Ordered so that the same
/// invalid manifest always reports the same primary code.
[[nodiscard]] FACILITYDRAIN_API Status validate_domain_manifest(OwnerDomain domain,
                                                                std::span<const ConsumerRecord> records,
                                                                const Limits& limits);

/// The domains that must be proven complete for a plan: the declared
/// requirement plus every domain that owns at least one mandatory obligation.
/// A declared requirement can never be narrowed below what the obligations
/// demand; that combination is a conflicting field, not a silent widening.
[[nodiscard]] FACILITYDRAIN_API DomainMask required_domains_for(DomainMask declared,
                                                                std::span<const ConsumerRecord> records);

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_CONSUMER_HPP
