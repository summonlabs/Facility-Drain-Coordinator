// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using facilitydrain::ConsumerCategory;
using facilitydrain::ConsumerRecord;
using facilitydrain::DomainMask;
using facilitydrain::ErrorCode;
using facilitydrain::Limits;
using facilitydrain::ObligationGeneration;
using facilitydrain::ObligationId;
using facilitydrain::ObligationStrength;
using facilitydrain::OwnerDomain;
using facilitydrain::ReservationGeneration;
using facilitydrain::Status;
using facilitydrain::consumer_category_from_token;
using facilitydrain::consumer_manifest_digest;
using facilitydrain::domain_index;
using facilitydrain::domain_of;
using facilitydrain::obligation_strength_from_token;
using facilitydrain::owner_domain_at;
using facilitydrain::owner_domain_from_token;
using facilitydrain::required_domains_for;
using facilitydrain::to_token;
using facilitydrain::validate_domain_manifest;

constexpr std::array<OwnerDomain, 4> kAllDomains{
    OwnerDomain::kAsi, OwnerDomain::kDfi, OwnerDomain::kFacility, OwnerDomain::kMonitoring};

constexpr std::array<ConsumerCategory, 8> kAllCategories{
    ConsumerCategory::kWorkload,
    ConsumerCategory::kAcceleratorReservation,
    ConsumerCategory::kNetworkPath,
    ConsumerCategory::kNetworkAttachment,
    ConsumerCategory::kServiceClass,
    ConsumerCategory::kMaintenanceProtection,
    ConsumerCategory::kFacilityReservation,
    ConsumerCategory::kMonitoringDependency};

[[nodiscard]] ConsumerRecord make_record(std::uint64_t obligation, ConsumerCategory category,
                                         std::uint64_t generation, ObligationStrength strength,
                                         std::string_view label = "label",
                                         std::string_view source = "source") {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = category;
  record.generation = ObligationGeneration{generation};
  record.reservation = ReservationGeneration{0};
  record.strength = strength;
  record.label = std::string{label};
  record.source = std::string{source};
  return record;
}

[[nodiscard]] bool detail_contains(const Status& status, std::string_view text) {
  return std::string_view{status.error().detail()}.find(text) != std::string_view::npos;
}

[[nodiscard]] std::string repeat(char character, std::size_t count) {
  return std::string(count, character);
}

}  // namespace

// ---------------------------------------------------------------------------
// Token round trips
// ---------------------------------------------------------------------------

FDC_TEST(consumer, domain_tokens_round_trip) {
  FDC_CHECK_EQ(facilitydrain::kOwnerDomainCount, 4U);
  FDC_CHECK_EQ(to_token(OwnerDomain::kAsi), std::string_view{"asi"});
  FDC_CHECK_EQ(to_token(OwnerDomain::kDfi), std::string_view{"dfi"});
  FDC_CHECK_EQ(to_token(OwnerDomain::kFacility), std::string_view{"facility"});
  FDC_CHECK_EQ(to_token(OwnerDomain::kMonitoring), std::string_view{"monitoring"});

  for (const OwnerDomain domain : kAllDomains) {
    const std::optional<OwnerDomain> parsed = owner_domain_from_token(to_token(domain));
    FDC_CHECK(parsed.has_value());
    FDC_CHECK(parsed.has_value() && *parsed == domain);
  }

  constexpr std::array<std::string_view, 6> kRejected{"", "ASI", "asi ", " asi", "fac", "facilities"};
  for (const std::string_view text : kRejected) {
    FDC_CHECK(!owner_domain_from_token(text).has_value());
  }
}

FDC_TEST(consumer, category_tokens_round_trip) {
  FDC_CHECK_EQ(to_token(ConsumerCategory::kWorkload), std::string_view{"workload"});
  FDC_CHECK_EQ(to_token(ConsumerCategory::kAcceleratorReservation),
               std::string_view{"accelerator-reservation"});
  FDC_CHECK_EQ(to_token(ConsumerCategory::kNetworkPath), std::string_view{"network-path"});
  FDC_CHECK_EQ(to_token(ConsumerCategory::kNetworkAttachment), std::string_view{"network-attachment"});
  FDC_CHECK_EQ(to_token(ConsumerCategory::kServiceClass), std::string_view{"service-class"});
  FDC_CHECK_EQ(to_token(ConsumerCategory::kMaintenanceProtection),
               std::string_view{"maintenance-protection"});
  FDC_CHECK_EQ(to_token(ConsumerCategory::kFacilityReservation), std::string_view{"facility-reservation"});
  FDC_CHECK_EQ(to_token(ConsumerCategory::kMonitoringDependency),
               std::string_view{"monitoring-dependency"});

  for (const ConsumerCategory category : kAllCategories) {
    const std::optional<ConsumerCategory> parsed = consumer_category_from_token(to_token(category));
    FDC_CHECK(parsed.has_value());
    FDC_CHECK(parsed.has_value() && *parsed == category);
  }

  FDC_CHECK(!consumer_category_from_token("").has_value());
  FDC_CHECK(!consumer_category_from_token("Workload").has_value());
  FDC_CHECK(!consumer_category_from_token("workload ").has_value());
  FDC_CHECK(!consumer_category_from_token("accelerator reservation").has_value());
  FDC_CHECK(!consumer_category_from_token("accelerator_reservation").has_value());
  FDC_CHECK(to_token(static_cast<ConsumerCategory>(0)).empty());
  FDC_CHECK(to_token(static_cast<ConsumerCategory>(255)).empty());
}

FDC_TEST(consumer, strength_tokens_round_trip) {
  FDC_CHECK_EQ(to_token(ObligationStrength::kMandatory), std::string_view{"mandatory"});
  FDC_CHECK_EQ(to_token(ObligationStrength::kAdvisory), std::string_view{"advisory"});
  FDC_CHECK(obligation_strength_from_token("mandatory") == ObligationStrength::kMandatory);
  FDC_CHECK(obligation_strength_from_token("advisory") == ObligationStrength::kAdvisory);
  FDC_CHECK(!obligation_strength_from_token("").has_value());
  FDC_CHECK(!obligation_strength_from_token("Mandatory").has_value());
  FDC_CHECK(!obligation_strength_from_token("required").has_value());
  FDC_CHECK(to_token(static_cast<ObligationStrength>(0)).empty());
  FDC_CHECK(to_token(static_cast<ObligationStrength>(255)).empty());
}

FDC_TEST(consumer, domain_of_is_total_and_fixed) {
  FDC_CHECK(domain_of(ConsumerCategory::kWorkload) == OwnerDomain::kAsi);
  FDC_CHECK(domain_of(ConsumerCategory::kAcceleratorReservation) == OwnerDomain::kAsi);
  FDC_CHECK(domain_of(ConsumerCategory::kServiceClass) == OwnerDomain::kAsi);
  FDC_CHECK(domain_of(ConsumerCategory::kNetworkPath) == OwnerDomain::kDfi);
  FDC_CHECK(domain_of(ConsumerCategory::kNetworkAttachment) == OwnerDomain::kDfi);
  FDC_CHECK(domain_of(ConsumerCategory::kMaintenanceProtection) == OwnerDomain::kFacility);
  FDC_CHECK(domain_of(ConsumerCategory::kFacilityReservation) == OwnerDomain::kFacility);
  FDC_CHECK(domain_of(ConsumerCategory::kMonitoringDependency) == OwnerDomain::kMonitoring);

  // The mapping is fixed: asking twice never yields two answers, and every
  // category maps into the four known domains.
  for (const ConsumerCategory category : kAllCategories) {
    const OwnerDomain first = domain_of(category);
    FDC_CHECK(domain_of(category) == first);
    const std::optional<OwnerDomain> known = owner_domain_from_token(to_token(first));
    FDC_CHECK(known.has_value());
    FDC_CHECK(known.has_value() && *known == first);
  }

  // Total even outside the enumeration, so no caller has to guess.
  FDC_CHECK(domain_of(static_cast<ConsumerCategory>(0)) == OwnerDomain::kAsi);
  FDC_CHECK(domain_of(static_cast<ConsumerCategory>(9)) == OwnerDomain::kAsi);
  FDC_CHECK(domain_of(static_cast<ConsumerCategory>(255)) == OwnerDomain::kAsi);
}

FDC_TEST(consumer, domain_index_and_owner_domain_at_are_inverses) {
  FDC_CHECK_EQ(domain_index(OwnerDomain::kAsi), 0U);
  FDC_CHECK_EQ(domain_index(OwnerDomain::kDfi), 1U);
  FDC_CHECK_EQ(domain_index(OwnerDomain::kFacility), 2U);
  FDC_CHECK_EQ(domain_index(OwnerDomain::kMonitoring), 3U);

  for (const OwnerDomain domain : kAllDomains) {
    FDC_CHECK(owner_domain_at(domain_index(domain)) == domain);
  }
  // An index outside the domain order has no domain; the documented fallback is
  // ASI, and no caller may rely on it.
  FDC_CHECK(owner_domain_at(4U) == OwnerDomain::kAsi);
  FDC_CHECK(owner_domain_at(99U) == OwnerDomain::kAsi);
}

// ---------------------------------------------------------------------------
// DomainMask
// ---------------------------------------------------------------------------

FDC_TEST(consumer, domain_mask_bits_and_set_operations) {
  FDC_CHECK_EQ(DomainMask::none().bits(), 0U);
  FDC_CHECK(DomainMask::none().empty());
  FDC_CHECK_EQ(DomainMask::none().count(), 0U);

  FDC_CHECK_EQ(DomainMask::of(OwnerDomain::kAsi).bits(), 1U);
  FDC_CHECK_EQ(DomainMask::of(OwnerDomain::kDfi).bits(), 2U);
  FDC_CHECK_EQ(DomainMask::of(OwnerDomain::kFacility).bits(), 4U);
  FDC_CHECK_EQ(DomainMask::of(OwnerDomain::kMonitoring).bits(), 8U);
  FDC_CHECK_EQ(DomainMask::all().bits(), 0x0FU);
  FDC_CHECK_EQ(DomainMask::all().count(), 4U);
  FDC_CHECK(!DomainMask::all().empty());

  for (const OwnerDomain domain : kAllDomains) {
    FDC_CHECK(DomainMask::all().contains(domain));
    FDC_CHECK(DomainMask::of(domain).contains(domain));
    FDC_CHECK_EQ(DomainMask::of(domain).count(), 1U);
    for (const OwnerDomain other : kAllDomains) {
      if (other != domain) {
        FDC_CHECK(!DomainMask::of(domain).contains(other));
      }
    }
  }

  const DomainMask asi_and_monitoring = DomainMask::none().with(OwnerDomain::kAsi).with(OwnerDomain::kMonitoring);
  FDC_CHECK_EQ(asi_and_monitoring.bits(), 0x09U);
  FDC_CHECK_EQ(asi_and_monitoring.count(), 2U);
  FDC_CHECK(asi_and_monitoring.contains(OwnerDomain::kAsi));
  FDC_CHECK(asi_and_monitoring.contains(OwnerDomain::kMonitoring));
  FDC_CHECK(!asi_and_monitoring.contains(OwnerDomain::kDfi));
  FDC_CHECK(!asi_and_monitoring.empty());

  const DomainMask without_asi = asi_and_monitoring.without(OwnerDomain::kAsi);
  FDC_CHECK_EQ(without_asi.bits(), 0x08U);
  FDC_CHECK_EQ(without_asi.count(), 1U);
  FDC_CHECK(!without_asi.contains(OwnerDomain::kAsi));
  FDC_CHECK_EQ(without_asi.without(OwnerDomain::kMonitoring).bits(), 0U);
  FDC_CHECK(without_asi.without(OwnerDomain::kMonitoring).empty());

  // Setting a bit twice is still one bit.
  FDC_CHECK(DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kAsi) == DomainMask::of(OwnerDomain::kAsi));
  FDC_CHECK(DomainMask::all().without(OwnerDomain::kFacility).count() == 3U);

  const std::optional<DomainMask> none_bits = DomainMask::from_bits(0U);
  FDC_CHECK(none_bits.has_value());
  FDC_CHECK(none_bits.has_value() && *none_bits == DomainMask::none());
  const std::optional<DomainMask> all_bits = DomainMask::from_bits(0x0FU);
  FDC_CHECK(all_bits.has_value());
  FDC_CHECK(all_bits.has_value() && *all_bits == DomainMask::all());
  // A bit outside the four known domains is an unknown requirement, which is
  // never silently dropped.
  FDC_CHECK(!DomainMask::from_bits(0x10U).has_value());
  FDC_CHECK(!DomainMask::from_bits(0x80U).has_value());
  FDC_CHECK(!DomainMask::from_bits(0xFFU).has_value());

  FDC_CHECK(DomainMask::none() != DomainMask::all());
  FDC_CHECK(DomainMask::none() < DomainMask::all());
  FDC_CHECK(DomainMask::of(OwnerDomain::kAsi) != DomainMask::of(OwnerDomain::kDfi));
  FDC_CHECK(DomainMask::of(OwnerDomain::kAsi) < DomainMask::of(OwnerDomain::kDfi));
}

FDC_TEST(consumer, domain_mask_canonical_text) {
  FDC_CHECK_EQ(DomainMask::none().to_canonical(), std::string{"none"});
  FDC_CHECK_EQ(DomainMask::all().to_canonical(), std::string{"asi,dfi,facility,monitoring"});
  FDC_CHECK_EQ(DomainMask::of(OwnerDomain::kAsi).to_canonical(), std::string{"asi"});
  FDC_CHECK_EQ(DomainMask::of(OwnerDomain::kFacility).to_canonical(), std::string{"facility"});
  // Canonical order, whatever order the bits were set in.
  FDC_CHECK_EQ(DomainMask::none().with(OwnerDomain::kMonitoring).with(OwnerDomain::kAsi).to_canonical(),
               std::string{"asi,monitoring"});
  FDC_CHECK_EQ(DomainMask::none().with(OwnerDomain::kMonitoring).with(OwnerDomain::kDfi).to_canonical(),
               std::string{"dfi,monitoring"});
}

FDC_TEST(consumer, domain_mask_parse_accepts_the_canonical_form) {
  const std::array<DomainMask, 6> masks{DomainMask::none(),
                                        DomainMask::of(OwnerDomain::kAsi),
                                        DomainMask::of(OwnerDomain::kDfi),
                                        DomainMask::of(OwnerDomain::kMonitoring),
                                        DomainMask::all(),
                                        DomainMask::none().with(OwnerDomain::kAsi).with(OwnerDomain::kFacility)};
  for (const DomainMask mask : masks) {
    const facilitydrain::Result<DomainMask> parsed = DomainMask::parse(mask.to_canonical());
    FDC_REQUIRE_OK(parsed);
    FDC_CHECK(parsed.value() == mask);
    FDC_CHECK_EQ(parsed.value().to_canonical(), mask.to_canonical());
  }

  const facilitydrain::Result<DomainMask> none = DomainMask::parse("none");
  FDC_REQUIRE_OK(none);
  FDC_CHECK(none.value() == DomainMask::none());
  const facilitydrain::Result<DomainMask> all = DomainMask::parse("all");
  FDC_REQUIRE_OK(all);
  FDC_CHECK(all.value() == DomainMask::all());
  const facilitydrain::Result<DomainMask> single = DomainMask::parse("monitoring");
  FDC_REQUIRE_OK(single);
  FDC_CHECK(single.value() == DomainMask::of(OwnerDomain::kMonitoring));

  // A list in another order names the same set of domains.
  const facilitydrain::Result<DomainMask> reversed = DomainMask::parse("monitoring,facility,dfi,asi");
  FDC_REQUIRE_OK(reversed);
  FDC_CHECK(reversed.value() == DomainMask::all());
}

FDC_TEST(consumer, domain_mask_parse_rejects_bad_text) {
  FDC_CHECK_CODE(DomainMask::parse(""), ErrorCode::kInvalidArgument);
  FDC_CHECK_CODE(DomainMask::parse("asi,"), ErrorCode::kInvalidText);
  FDC_CHECK_CODE(DomainMask::parse(",asi"), ErrorCode::kInvalidText);
  FDC_CHECK_CODE(DomainMask::parse("asi,,dfi"), ErrorCode::kInvalidText);
  FDC_CHECK_CODE(DomainMask::parse(" asi"), ErrorCode::kInvalidText);
  FDC_CHECK_CODE(DomainMask::parse("asi "), ErrorCode::kInvalidText);
  FDC_CHECK_CODE(DomainMask::parse("asi, dfi"), ErrorCode::kInvalidText);
  FDC_CHECK_CODE(DomainMask::parse("asi,asi"), ErrorCode::kDuplicateIdentifier);
  FDC_CHECK_CODE(DomainMask::parse("unknown"), ErrorCode::kInvalidEnumValue);
  FDC_CHECK_CODE(DomainMask::parse("ALL"), ErrorCode::kInvalidEnumValue);
  FDC_CHECK_CODE(DomainMask::parse("none,asi"), ErrorCode::kInvalidEnumValue);
  FDC_CHECK_CODE(DomainMask::parse("asi;dfi"), ErrorCode::kInvalidEnumValue);

  const facilitydrain::Result<DomainMask> duplicate = DomainMask::parse("dfi,asi,dfi");
  FDC_CHECK(!duplicate.has_value());
  if (!duplicate.has_value()) {
    FDC_CHECK(detail_contains(duplicate.error(), "dfi"));
  }
}

// ---------------------------------------------------------------------------
// ConsumerRecord
// ---------------------------------------------------------------------------

FDC_TEST(consumer, record_validation_and_canonical_text) {
  const Limits limits;
  const ConsumerRecord record =
      make_record(1001ULL, ConsumerCategory::kWorkload, 7ULL, ObligationStrength::kMandatory, "web", "asi.db");
  const Status status = record.validate(limits);
  FDC_CHECK(status.ok());
  FDC_CHECK_EQ(record.domain(), OwnerDomain::kAsi);
  FDC_CHECK_EQ(record.to_canonical(),
               std::string{"asi:1001 workload gen=7 res=0 mandatory label=web source=asi.db"});

  // A record with a reservation names it.
  ConsumerRecord reserved =
      make_record(1002ULL, ConsumerCategory::kAcceleratorReservation, 3ULL, ObligationStrength::kAdvisory, "gpu", "asi");
  reserved.reservation = ReservationGeneration{44ULL};
  FDC_CHECK(reserved.validate(limits).ok());
  FDC_CHECK_EQ(reserved.to_canonical(),
               std::string{"asi:1002 accelerator-reservation gen=3 res=44 advisory label=gpu source=asi"});
  FDC_CHECK_EQ(reserved.domain(), OwnerDomain::kAsi);

  // The documented validation order decides the primary code.
  const ConsumerRecord no_obligation =
      make_record(0ULL, ConsumerCategory::kWorkload, 7ULL, ObligationStrength::kMandatory);
  FDC_CHECK_STATUS(no_obligation.validate(limits), ErrorCode::kInvalidIdentity);

  const ConsumerRecord no_generation =
      make_record(1ULL, ConsumerCategory::kWorkload, 0ULL, ObligationStrength::kMandatory);
  FDC_CHECK_STATUS(no_generation.validate(limits), ErrorCode::kMissingRequiredField);

  ConsumerRecord bad_category = make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory);
  bad_category.category = static_cast<ConsumerCategory>(255);
  FDC_CHECK_STATUS(bad_category.validate(limits), ErrorCode::kInvalidEnumValue);
  // The category is checked before the generation.
  bad_category.generation = ObligationGeneration{0};
  FDC_CHECK_STATUS(bad_category.validate(limits), ErrorCode::kInvalidEnumValue);

  ConsumerRecord bad_strength = make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory);
  bad_strength.strength = static_cast<ObligationStrength>(255);
  FDC_CHECK_STATUS(bad_strength.validate(limits), ErrorCode::kInvalidEnumValue);

  ConsumerRecord long_label = make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory);
  long_label.label = repeat('x', static_cast<std::size_t>(limits.max_text_bytes) + 1U);
  FDC_CHECK_STATUS(long_label.validate(limits), ErrorCode::kFieldTooLong);

  ConsumerRecord long_source = make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory);
  long_source.source = repeat('x', static_cast<std::size_t>(limits.max_text_bytes) + 1U);
  FDC_CHECK_STATUS(long_source.validate(limits), ErrorCode::kFieldTooLong);

  ConsumerRecord control_character =
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "web\nnode", "asi");
  FDC_CHECK_STATUS(control_character.validate(limits), ErrorCode::kInvalidText);
  // The identity is checked before the text fields.
  control_character.obligation = ObligationId{0};
  FDC_CHECK_STATUS(control_character.validate(limits), ErrorCode::kInvalidIdentity);
}

// ---------------------------------------------------------------------------
// consumer_manifest_digest
// ---------------------------------------------------------------------------

FDC_TEST(consumer, manifest_digest_ignores_listing_order) {
  const std::vector<ConsumerRecord> listed{
      make_record(3ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "c", "asi"),
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "a", "asi"),
      make_record(2ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "b", "asi")};
  const std::vector<ConsumerRecord> reversed{listed[2], listed[1], listed[0]};
  const std::vector<ConsumerRecord> shuffled{listed[1], listed[2], listed[0]};

  const facilitydrain::ContentDigest listed_digest = consumer_manifest_digest(OwnerDomain::kAsi, listed);
  FDC_CHECK(listed_digest == consumer_manifest_digest(OwnerDomain::kAsi, reversed));
  FDC_CHECK(listed_digest == consumer_manifest_digest(OwnerDomain::kAsi, shuffled));
  FDC_CHECK(!listed_digest.is_zero());

  // Only the requested domain participates: another domain's records are not
  // part of this manifest at all.
  std::vector<ConsumerRecord> with_foreign = listed;
  with_foreign.push_back(
      make_record(9ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kMandatory, "path", "dfi"));
  FDC_CHECK(consumer_manifest_digest(OwnerDomain::kAsi, with_foreign) == listed_digest);
  FDC_CHECK(consumer_manifest_digest(OwnerDomain::kDfi, with_foreign) !=
            consumer_manifest_digest(OwnerDomain::kDfi, listed));

  // An empty manifest still has a digest, and it is not the digest of a
  // manifest that happens to contain one record.
  const std::span<const ConsumerRecord> empty_manifest{};
  const facilitydrain::ContentDigest empty_digest = consumer_manifest_digest(OwnerDomain::kAsi, empty_manifest);
  FDC_CHECK(!empty_digest.is_zero());
  FDC_CHECK(empty_digest != listed_digest);
  // The domain is part of the separation tag, so the same (even empty)
  // membership under two domains is two different statements.
  FDC_CHECK(empty_digest != consumer_manifest_digest(OwnerDomain::kDfi, empty_manifest));
  FDC_CHECK(empty_digest != consumer_manifest_digest(OwnerDomain::kFacility, empty_manifest));
  FDC_CHECK(empty_digest !=
            consumer_manifest_digest(OwnerDomain::kMonitoring, empty_manifest));
}

FDC_TEST(consumer, manifest_digest_covers_every_field) {
  const std::vector<ConsumerRecord> base{
      make_record(1001ULL, ConsumerCategory::kWorkload, 7ULL, ObligationStrength::kMandatory, "label", "source")};
  const facilitydrain::ContentDigest base_digest = consumer_manifest_digest(OwnerDomain::kAsi, base);
  FDC_CHECK(!base_digest.is_zero());

  // Every field the record is compared and hashed by changes the digest.
  const std::array<std::vector<ConsumerRecord>, 7> variants{
      std::vector<ConsumerRecord>{make_record(1002ULL, ConsumerCategory::kWorkload, 7ULL,
                                              ObligationStrength::kMandatory, "label", "source")},
      std::vector<ConsumerRecord>{make_record(1001ULL, ConsumerCategory::kServiceClass, 7ULL,
                                              ObligationStrength::kMandatory, "label", "source")},
      std::vector<ConsumerRecord>{make_record(1001ULL, ConsumerCategory::kWorkload, 8ULL,
                                              ObligationStrength::kMandatory, "label", "source")},
      std::vector<ConsumerRecord>{make_record(1001ULL, ConsumerCategory::kWorkload, 7ULL,
                                              ObligationStrength::kAdvisory, "label", "source")},
      std::vector<ConsumerRecord>{make_record(1001ULL, ConsumerCategory::kWorkload, 7ULL,
                                              ObligationStrength::kMandatory, "other", "source")},
      std::vector<ConsumerRecord>{make_record(1001ULL, ConsumerCategory::kWorkload, 7ULL,
                                              ObligationStrength::kMandatory, "label", "other")},
      // A manifest with an extra record is a different manifest.
      std::vector<ConsumerRecord>{make_record(1001ULL, ConsumerCategory::kWorkload, 7ULL,
                                              ObligationStrength::kMandatory, "label", "source"),
                                  make_record(2002ULL, ConsumerCategory::kWorkload, 7ULL,
                                              ObligationStrength::kMandatory, "label", "source")}};
  for (const std::vector<ConsumerRecord>& variant : variants) {
    FDC_CHECK(consumer_manifest_digest(OwnerDomain::kAsi, variant) != base_digest);
  }

  // The reservation generation is part of the digest even when it is zero in
  // the base record.
  ConsumerRecord reserved =
      make_record(1001ULL, ConsumerCategory::kWorkload, 7ULL, ObligationStrength::kMandatory, "label", "source");
  reserved.reservation = ReservationGeneration{5ULL};
  const std::vector<ConsumerRecord> reserved_manifest{reserved};
  FDC_CHECK(consumer_manifest_digest(OwnerDomain::kAsi, reserved_manifest) != base_digest);

  // Deterministic: the same manifest always produces the same digest.
  FDC_CHECK(consumer_manifest_digest(OwnerDomain::kAsi, base) == base_digest);
}

// ---------------------------------------------------------------------------
// validate_domain_manifest
// ---------------------------------------------------------------------------

FDC_TEST(consumer, validate_domain_manifest_accepts_a_well_formed_manifest) {
  const Limits limits;
  const std::vector<ConsumerRecord> manifest{
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "a", "asi"),
      make_record(2ULL, ConsumerCategory::kAcceleratorReservation, 1ULL, ObligationStrength::kAdvisory, "b", "asi")};
  FDC_CHECK(validate_domain_manifest(OwnerDomain::kAsi, manifest, limits).ok());
  // An empty manifest is well formed: absence of records is a statement.
  FDC_CHECK(validate_domain_manifest(OwnerDomain::kAsi, std::span<const ConsumerRecord>{}, limits).ok());
}

FDC_TEST(consumer, validate_domain_manifest_rejects_duplicates) {
  const Limits limits;
  const std::vector<ConsumerRecord> duplicates{
      make_record(7ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "a", "asi"),
      make_record(7ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "b", "asi")};
  const Status status = validate_domain_manifest(OwnerDomain::kAsi, duplicates, limits);
  FDC_CHECK_STATUS(status, ErrorCode::kDuplicateIdentifier);
  FDC_CHECK(detail_contains(status, "7"));

  const std::vector<ConsumerRecord> three{
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "a", "asi"),
      make_record(2ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "b", "asi"),
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "a", "asi")};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kAsi, three, limits), ErrorCode::kDuplicateIdentifier);
}

FDC_TEST(consumer, validate_domain_manifest_rejects_a_foreign_category) {
  const Limits limits;
  const ConsumerRecord dfi_record =
      make_record(1ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kMandatory, "path", "dfi");
  // The record is valid on its own; it is the manifest's domain that disagrees.
  FDC_CHECK(dfi_record.validate(limits).ok());
  const std::vector<ConsumerRecord> wrong_domain{dfi_record};
  const Status status = validate_domain_manifest(OwnerDomain::kAsi, wrong_domain, limits);
  FDC_CHECK_STATUS(status, ErrorCode::kConflictingField);
  FDC_CHECK(detail_contains(status, "network-path"));
  FDC_CHECK(detail_contains(status, "dfi"));

  const std::vector<ConsumerRecord> asi_record{
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "web", "asi")};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, asi_record, limits),
                   ErrorCode::kConflictingField);

  // Ownership is decided before duplicates are reported.
  const std::vector<ConsumerRecord> foreign_duplicates{
      make_record(7ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kMandatory, "p", "dfi"),
      make_record(7ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kMandatory, "p", "dfi")};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kAsi, foreign_duplicates, limits),
                   ErrorCode::kConflictingField);
}

FDC_TEST(consumer, validate_domain_manifest_rejects_oversized_text) {
  const Limits limits;
  ConsumerRecord record =
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "label", "asi");
  record.label = repeat('x', static_cast<std::size_t>(limits.max_text_bytes) + 1U);
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kAsi, std::vector<ConsumerRecord>{record}, limits),
                   ErrorCode::kFieldTooLong);

  ConsumerRecord control =
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "label\n", "asi");
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kAsi, std::vector<ConsumerRecord>{control}, limits),
                   ErrorCode::kInvalidText);

  // A smaller text bound turns a previously accepted label into a rejection.
  Limits small = limits;
  small.max_text_bytes = 8U;
  const std::vector<ConsumerRecord> nine_bytes{
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "123456789", "asi")};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kAsi, nine_bytes, small), ErrorCode::kFieldTooLong);
  FDC_CHECK(validate_domain_manifest(OwnerDomain::kAsi, nine_bytes, limits).ok());
}

FDC_TEST(consumer, validate_domain_manifest_rejects_an_over_limit_list) {
  Limits limits;
  limits.max_consumers_per_domain = 2U;
  const std::vector<ConsumerRecord> two{
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "a", "asi"),
      make_record(2ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "b", "asi")};
  FDC_CHECK(validate_domain_manifest(OwnerDomain::kAsi, two, limits).ok());

  std::vector<ConsumerRecord> three = two;
  three.push_back(make_record(3ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "c", "asi"));
  const Status status = validate_domain_manifest(OwnerDomain::kAsi, three, limits);
  FDC_CHECK_STATUS(status, ErrorCode::kTooManyEntries);

  // The bound is checked before any record is examined, so an over long list
  // of invalid records is still reported as an over long list.
  std::vector<ConsumerRecord> three_invalid{
      make_record(0ULL, ConsumerCategory::kWorkload, 0ULL, ObligationStrength::kMandatory, "a", "asi"),
      make_record(0ULL, ConsumerCategory::kWorkload, 0ULL, ObligationStrength::kMandatory, "b", "asi"),
      make_record(0ULL, ConsumerCategory::kWorkload, 0ULL, ObligationStrength::kMandatory, "c", "asi")};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kAsi, three_invalid, limits),
                   ErrorCode::kTooManyEntries);
}

FDC_TEST(consumer, validate_domain_manifest_primary_code_is_listing_order_independent) {
  const Limits limits;
  // Obligation 5 carries an over long label, obligation 7 belongs to DFI.
  ConsumerRecord long_label =
      make_record(5ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "label", "asi");
  long_label.label = repeat('x', static_cast<std::size_t>(limits.max_text_bytes) + 1U);
  const ConsumerRecord foreign =
      make_record(7ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kMandatory, "path", "dfi");

  const std::vector<ConsumerRecord> ascending{long_label, foreign};
  const std::vector<ConsumerRecord> descending{foreign, long_label};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, ascending, limits), ErrorCode::kFieldTooLong);
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, descending, limits), ErrorCode::kFieldTooLong);

  // A record level failure outranks an ownership disagreement whichever order
  // the records were listed in, because every record is validated before any
  // ownership question is asked.
  const std::vector<ConsumerRecord> lower_foreign_first{make_record(2ULL, ConsumerCategory::kNetworkPath, 1ULL,
                                                                    ObligationStrength::kMandatory, "path", "dfi"),
                                                        long_label};
  const std::vector<ConsumerRecord> lower_foreign_last{long_label,
                                                       make_record(2ULL, ConsumerCategory::kNetworkPath, 1ULL,
                                                                   ObligationStrength::kMandatory, "path", "dfi")};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, lower_foreign_first, limits),
                   ErrorCode::kFieldTooLong);
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, lower_foreign_last, limits),
                   ErrorCode::kFieldTooLong);

  // A manifest whose only problem is ownership reports the ownership code in
  // both listing orders, whatever the obligation ids are.
  const ConsumerRecord valid_asi =
      make_record(9ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "web", "asi");
  const ConsumerRecord foreign_low =
      make_record(2ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kMandatory, "path", "dfi");
  const std::vector<ConsumerRecord> foreign_first{foreign_low, valid_asi};
  const std::vector<ConsumerRecord> foreign_last{valid_asi, foreign_low};
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, foreign_first, limits),
                   ErrorCode::kConflictingField);
  FDC_CHECK_STATUS(validate_domain_manifest(OwnerDomain::kDfi, foreign_last, limits),
                   ErrorCode::kConflictingField);
}

// ---------------------------------------------------------------------------
// required_domains_for
// ---------------------------------------------------------------------------

FDC_TEST(consumer, required_domains_widen_with_mandatory_obligations_only) {
  const std::vector<ConsumerRecord> records{
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kMandatory, "a", "asi"),
      make_record(2ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kAdvisory, "b", "dfi"),
      make_record(3ULL, ConsumerCategory::kMonitoringDependency, 1ULL, ObligationStrength::kMandatory, "c", "mon")};

  const DomainMask declared = DomainMask::of(OwnerDomain::kDfi);
  const DomainMask required = required_domains_for(declared, records);
  FDC_CHECK(required.contains(OwnerDomain::kDfi));
  FDC_CHECK(required.contains(OwnerDomain::kAsi));
  FDC_CHECK(required.contains(OwnerDomain::kMonitoring));
  FDC_CHECK(!required.contains(OwnerDomain::kFacility));
  FDC_CHECK_EQ(required.count(), 3U);

  // An advisory obligation never widens the requirement.
  const std::vector<ConsumerRecord> advisory_only{
      make_record(1ULL, ConsumerCategory::kWorkload, 1ULL, ObligationStrength::kAdvisory, "a", "asi"),
      make_record(2ULL, ConsumerCategory::kNetworkPath, 1ULL, ObligationStrength::kAdvisory, "b", "dfi")};
  FDC_CHECK(required_domains_for(DomainMask::none(), advisory_only) == DomainMask::none());
  FDC_CHECK(required_domains_for(DomainMask::of(OwnerDomain::kFacility), advisory_only) ==
            DomainMask::of(OwnerDomain::kFacility));

  // A declared requirement is never narrowed.
  FDC_CHECK(required_domains_for(DomainMask::all(), std::span<const ConsumerRecord>{}) == DomainMask::all());
  FDC_CHECK(required_domains_for(DomainMask::none(), std::span<const ConsumerRecord>{}) == DomainMask::none());

  // A mandatory obligation adds its owner, and nothing else.
  const std::vector<ConsumerRecord> mandatory_facility{
      make_record(4ULL, ConsumerCategory::kMaintenanceProtection, 1ULL, ObligationStrength::kMandatory, "m", "fac")};
  const DomainMask widened = required_domains_for(DomainMask::of(OwnerDomain::kAsi), mandatory_facility);
  FDC_CHECK(widened.contains(OwnerDomain::kAsi));
  FDC_CHECK(widened.contains(OwnerDomain::kFacility));
  FDC_CHECK(!widened.contains(OwnerDomain::kDfi));
  FDC_CHECK(!widened.contains(OwnerDomain::kMonitoring));
  FDC_CHECK_EQ(widened.count(), 2U);
}
