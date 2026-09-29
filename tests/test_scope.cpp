// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using facilitydrain::ContentDigest;
using facilitydrain::DrainScope;
using facilitydrain::DrainTargetManifest;
using facilitydrain::ErrorCode;
using facilitydrain::Limits;
using facilitydrain::Result;
using facilitydrain::ScopeKind;
using facilitydrain::format_scope;
using facilitydrain::format_strong;
using facilitydrain::is_drainable_scope_kind;
using facilitydrain::parse_scope;
using facilitydrain::scope_kind_from_token;
using facilitydrain::to_token;

constexpr std::uint64_t kUint64Maximum = (std::numeric_limits<std::uint64_t>::max)();

constexpr std::array<ScopeKind, 5> kAllKinds{
    ScopeKind::kAsset, ScopeKind::kRack, ScopeKind::kZone, ScopeKind::kSubscope, ScopeKind::kSite};

constexpr std::array<std::uint64_t, 6> kIds{1ULL, 2ULL, 17ULL, 99ULL, 1000ULL, kUint64Maximum};

[[nodiscard]] DrainScope scope_of(ScopeKind kind, std::uint64_t id) {
  return DrainScope{kind, id};
}

[[nodiscard]] bool detail_contains(const Result<DrainScope>& result, std::string_view text) {
  if (result.has_value()) {
    return false;
  }
  return std::string_view{result.error().detail()}.find(text) != std::string_view::npos;
}

[[nodiscard]] Result<DrainTargetManifest> make_manifest(std::vector<DrainScope> targets, const Limits& limits) {
  return DrainTargetManifest::create(std::move(targets), limits);
}

}  // namespace

// ---------------------------------------------------------------------------
// DrainScope text form
// ---------------------------------------------------------------------------

FDC_TEST(scope, format_and_parse_round_trip_for_every_kind) {
  for (const ScopeKind kind : kAllKinds) {
    for (const std::uint64_t id : kIds) {
      const DrainScope scope = scope_of(kind, id);
      const std::string text = format_scope(scope);
      FDC_CHECK_EQ(text, std::string{to_token(kind)} + ":" + format_strong(id));

      const Result<DrainScope> parsed = parse_scope(text);
      FDC_REQUIRE_OK(parsed);
      FDC_CHECK(parsed.value() == scope);
      FDC_CHECK_EQ(parsed.value().kind, kind);
      FDC_CHECK_EQ(parsed.value().id, id);
      // Exactly one spelling round trips: rendering the parsed value produces
      // the same bytes that were parsed.
      FDC_CHECK_EQ(format_scope(parsed.value()), text);
      FDC_CHECK(!parsed.value().is_default());
    }
  }
}

FDC_TEST(scope, kind_tokens_round_trip) {
  FDC_CHECK_EQ(to_token(ScopeKind::kAsset), std::string_view{"asset"});
  FDC_CHECK_EQ(to_token(ScopeKind::kRack), std::string_view{"rack"});
  FDC_CHECK_EQ(to_token(ScopeKind::kZone), std::string_view{"zone"});
  FDC_CHECK_EQ(to_token(ScopeKind::kSubscope), std::string_view{"subscope"});
  FDC_CHECK_EQ(to_token(ScopeKind::kSite), std::string_view{"site"});

  for (const ScopeKind kind : kAllKinds) {
    const std::string_view token = to_token(kind);
    FDC_CHECK(!token.empty());
    const std::optional<ScopeKind> parsed = scope_kind_from_token(token);
    FDC_CHECK(parsed.has_value());
    FDC_CHECK(parsed.has_value() && *parsed == kind);
  }

  constexpr std::array<std::string_view, 7> kRejected{
      "", "Asset", "asset ", " asset", "ASSET", "racks", "asset:"};
  for (const std::string_view text : kRejected) {
    FDC_CHECK(!scope_kind_from_token(text).has_value());
  }
}

FDC_TEST(scope, every_documented_kind_is_drainable) {
  for (const ScopeKind kind : kAllKinds) {
    FDC_CHECK(is_drainable_scope_kind(kind));
  }
  // An out of range level has no token and is not a drain target.
  FDC_CHECK(!is_drainable_scope_kind(static_cast<ScopeKind>(0)));
  FDC_CHECK(!is_drainable_scope_kind(static_cast<ScopeKind>(6)));
  FDC_CHECK(!is_drainable_scope_kind(static_cast<ScopeKind>(255)));
  FDC_CHECK(to_token(static_cast<ScopeKind>(0)).empty());
  FDC_CHECK(to_token(static_cast<ScopeKind>(6)).empty());
  FDC_CHECK(to_token(static_cast<ScopeKind>(255)).empty());
  FDC_CHECK(!scope_kind_from_token(to_token(static_cast<ScopeKind>(0))).has_value());
}

FDC_TEST(scope, parse_rejects_unknown_kinds) {
  constexpr std::array<std::string_view, 5> kTexts{"disk:4", "Asset:4", ":4", "0:4", "asset s:4"};
  for (const std::string_view text : kTexts) {
    FDC_CHECK_CODE(parse_scope(text), ErrorCode::kInvalidScope);
  }
  const Result<DrainScope> unknown = parse_scope("disk:4");
  FDC_CHECK(detail_contains(unknown, "disk"));
}

FDC_TEST(scope, parse_rejects_missing_or_doubled_separators) {
  constexpr std::array<std::string_view, 6> kTexts{"", "asset", "asset:1:2", "asset::", "asset:", ":"};
  for (const std::string_view text : kTexts) {
    FDC_CHECK_CODE(parse_scope(text), ErrorCode::kInvalidScope);
  }
  FDC_CHECK(detail_contains(parse_scope("asset"), "separator"));
  FDC_CHECK(detail_contains(parse_scope("asset:1:2"), "more than one"));
  FDC_CHECK(detail_contains(parse_scope(""), "empty"));
  const Result<DrainScope> empty_id = parse_scope("asset:");
  FDC_CHECK(detail_contains(empty_id, "id is empty"));
}

FDC_TEST(scope, parse_rejects_zero_ids) {
  for (const ScopeKind kind : kAllKinds) {
    const std::string text = std::string{to_token(kind)} + ":0";
    FDC_CHECK_CODE(parse_scope(text), ErrorCode::kInvalidScope);
  }
  const Result<DrainScope> zero = parse_scope("asset:0");
  FDC_CHECK(detail_contains(zero, "scope 0 does not exist"));
}

FDC_TEST(scope, parse_rejects_signs_leading_zeros_and_whitespace) {
  constexpr std::array<std::string_view, 10> kTexts{"asset:+1",  "asset:-1",  "asset:01", "asset:00",
                                                    "asset: 1",  "asset:1 ",  " asset:1", "asset:1\n",
                                                    "asset:0x10", "asset:1:"};
  for (const std::string_view text : kTexts) {
    FDC_CHECK_CODE(parse_scope(text), ErrorCode::kInvalidScope);
  }
  FDC_CHECK(detail_contains(parse_scope("asset:01"), "decimal-leading-zero"));
  FDC_CHECK(detail_contains(parse_scope("asset:1 "), "trailing-whitespace"));
  FDC_CHECK(detail_contains(parse_scope("asset:+1"), "invalid-character"));
  FDC_CHECK(detail_contains(parse_scope("asset:-1"), "negative-sign"));
  FDC_CHECK(detail_contains(parse_scope("asset:18446744073709551616"), "overflow"));
}

// ---------------------------------------------------------------------------
// DrainTargetManifest
// ---------------------------------------------------------------------------

FDC_TEST(scope, manifest_canonicalises_the_membership) {
  const Limits limits;
  Result<DrainTargetManifest> created = make_manifest(
      std::vector<DrainScope>{scope_of(ScopeKind::kRack, 9ULL), scope_of(ScopeKind::kAsset, 2ULL),
                              scope_of(ScopeKind::kAsset, 1ULL)},
      limits);
  FDC_REQUIRE_OK(created);
  FDC_CHECK_EQ(created.value().size(), 3U);
  FDC_CHECK_EQ(created.value().to_canonical(), std::string{"asset:1|asset:2|rack:9"});
  const std::vector<DrainScope>& targets = created.value().targets();
  FDC_CHECK_EQ(targets.size(), 3U);
  FDC_CHECK(targets[0] == scope_of(ScopeKind::kAsset, 1ULL));
  FDC_CHECK(targets[1] == scope_of(ScopeKind::kAsset, 2ULL));
  FDC_CHECK(targets[2] == scope_of(ScopeKind::kRack, 9ULL));

  // The same membership in any input order produces the same manifest.
  const Result<DrainTargetManifest> reordered = make_manifest(
      std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL), scope_of(ScopeKind::kRack, 9ULL),
                              scope_of(ScopeKind::kAsset, 2ULL)},
      limits);
  FDC_REQUIRE_OK(reordered);
  FDC_CHECK_EQ(reordered.value().to_canonical(), created.value().to_canonical());
  FDC_CHECK(reordered.value().digest() == created.value().digest());

  // A single target is a manifest too.
  const Result<DrainTargetManifest> single = make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kSite, 1ULL)}, limits);
  FDC_REQUIRE_OK(single);
  FDC_CHECK_EQ(single.value().size(), 1U);
  FDC_CHECK_EQ(single.value().to_canonical(), std::string{"site:1"});
}

FDC_TEST(scope, manifest_rejects_duplicates) {
  const Limits limits;
  const DrainScope asset_one = scope_of(ScopeKind::kAsset, 1ULL);
  FDC_CHECK_CODE(make_manifest(std::vector<DrainScope>{asset_one, asset_one}, limits),
                 ErrorCode::kDuplicateIdentifier);
  FDC_CHECK_CODE(make_manifest(std::vector<DrainScope>{asset_one, scope_of(ScopeKind::kRack, 2ULL), asset_one},
                               limits),
                 ErrorCode::kDuplicateIdentifier);

  // The same id at two different levels is two different targets.
  const Result<DrainTargetManifest> two_levels =
      make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL), scope_of(ScopeKind::kRack, 1ULL)},
                    limits);
  FDC_REQUIRE_OK(two_levels);
  FDC_CHECK_EQ(two_levels.value().size(), 2U);
  FDC_CHECK_EQ(two_levels.value().to_canonical(), std::string{"asset:1|rack:1"});
}

FDC_TEST(scope, manifest_rejects_an_empty_set) {
  const Limits limits;
  FDC_CHECK_CODE(make_manifest(std::vector<DrainScope>{}, limits), ErrorCode::kMissingRequiredField);
}

FDC_TEST(scope, manifest_rejects_an_over_limit_set) {
  Limits limits;
  limits.max_targets_per_plan = 2U;
  const Result<DrainTargetManifest> at_the_limit =
      make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL), scope_of(ScopeKind::kAsset, 2ULL)},
                    limits);
  FDC_CHECK(at_the_limit.has_value());

  const Result<DrainTargetManifest> over_the_limit = make_manifest(
      std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL), scope_of(ScopeKind::kAsset, 2ULL),
                              scope_of(ScopeKind::kAsset, 3ULL)},
      limits);
  FDC_CHECK_CODE(over_the_limit, ErrorCode::kTooManyEntries);
  if (!over_the_limit.has_value()) {
    const std::string_view detail{over_the_limit.error().detail()};
    FDC_CHECK(detail.find("3") != std::string_view::npos);
    FDC_CHECK(detail.find("2") != std::string_view::npos);
  }
}

FDC_TEST(scope, manifest_rejects_targets_that_are_not_real_scopes) {
  const Limits limits;
  // Identity zero does not exist, at any level.
  FDC_CHECK_CODE(make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 0ULL)}, limits),
                 ErrorCode::kInvalidScope);
  FDC_CHECK_CODE(make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kSite, 0ULL)}, limits),
                 ErrorCode::kInvalidScope);
  // A level that is not drainable cannot be demanded of any owner.
  FDC_CHECK_CODE(make_manifest(std::vector<DrainScope>{DrainScope{static_cast<ScopeKind>(0), 5ULL}}, limits),
                 ErrorCode::kInvalidScope);
  FDC_CHECK_CODE(make_manifest(std::vector<DrainScope>{DrainScope{static_cast<ScopeKind>(99), 5ULL}}, limits),
                 ErrorCode::kInvalidScope);
}

FDC_TEST(scope, manifest_contains_is_exact) {
  const Limits limits;
  const Result<DrainTargetManifest> manifest = make_manifest(
      std::vector<DrainScope>{scope_of(ScopeKind::kRack, 9ULL), scope_of(ScopeKind::kAsset, 1ULL)}, limits);
  FDC_REQUIRE_OK(manifest);
  FDC_CHECK(manifest.value().contains(scope_of(ScopeKind::kAsset, 1ULL)));
  FDC_CHECK(manifest.value().contains(scope_of(ScopeKind::kRack, 9ULL)));
  FDC_CHECK(!manifest.value().contains(scope_of(ScopeKind::kAsset, 2ULL)));
  FDC_CHECK(!manifest.value().contains(scope_of(ScopeKind::kRack, 1ULL)));
  FDC_CHECK(!manifest.value().contains(DrainScope{}));

  const Result<DrainTargetManifest> single = make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kZone, 4ULL)}, limits);
  FDC_REQUIRE_OK(single);
  FDC_CHECK(single.value().contains(scope_of(ScopeKind::kZone, 4ULL)));
  FDC_CHECK(!single.value().contains(scope_of(ScopeKind::kSubscope, 4ULL)));
}

FDC_TEST(scope, manifest_digest_is_membership_sensitive) {
  const Limits limits;
  const Result<DrainTargetManifest> two = make_manifest(
      std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL), scope_of(ScopeKind::kRack, 9ULL)}, limits);
  const Result<DrainTargetManifest> two_reordered = make_manifest(
      std::vector<DrainScope>{scope_of(ScopeKind::kRack, 9ULL), scope_of(ScopeKind::kAsset, 1ULL)}, limits);
  const Result<DrainTargetManifest> one =
      make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL)}, limits);
  const Result<DrainTargetManifest> other_id =
      make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL), scope_of(ScopeKind::kRack, 8ULL)},
                    limits);
  const Result<DrainTargetManifest> other_kind =
      make_manifest(std::vector<DrainScope>{scope_of(ScopeKind::kAsset, 1ULL), scope_of(ScopeKind::kAsset, 9ULL)},
                    limits);
  FDC_REQUIRE_OK(two);
  FDC_REQUIRE_OK(two_reordered);
  FDC_REQUIRE_OK(one);
  FDC_REQUIRE_OK(other_id);
  FDC_REQUIRE_OK(other_kind);

  // Order independent, content dependent.
  FDC_CHECK(two.value().digest() == two_reordered.value().digest());
  FDC_CHECK(two.value().digest() != one.value().digest());
  FDC_CHECK(two.value().digest() != other_id.value().digest());
  FDC_CHECK(two.value().digest() != other_kind.value().digest());
  FDC_CHECK(other_id.value().digest() != other_kind.value().digest());
  // Deterministic, and never the missing value.
  FDC_CHECK(two.value().digest() == two.value().digest());
  FDC_CHECK(!two.value().digest().is_zero());
  FDC_CHECK(two.value().digest() != ContentDigest{});
  // A membership digest is not the digest of the rendered text.
  FDC_CHECK(two.value().digest() != facilitydrain::digest_text(two.value().to_canonical()));
}

// ---------------------------------------------------------------------------
// DrainScope value semantics
// ---------------------------------------------------------------------------

FDC_TEST(scope, ordering_and_the_default_value) {
  const DrainScope none{};
  FDC_CHECK(none.is_default());
  FDC_CHECK(scope_of(ScopeKind::kAsset, 0ULL).is_default());
  FDC_CHECK(!scope_of(ScopeKind::kAsset, 1ULL).is_default());
  FDC_CHECK_EQ(none.id, 0ULL);
  FDC_CHECK_EQ(none.kind, ScopeKind::kAsset);

  // Ordering is by level first, then by identity.
  FDC_CHECK(scope_of(ScopeKind::kAsset, 2ULL) < scope_of(ScopeKind::kAsset, 10ULL));
  FDC_CHECK(scope_of(ScopeKind::kAsset, kUint64Maximum) < scope_of(ScopeKind::kRack, 1ULL));
  FDC_CHECK(scope_of(ScopeKind::kRack, 5ULL) < scope_of(ScopeKind::kZone, 1ULL));
  FDC_CHECK(scope_of(ScopeKind::kZone, 1ULL) < scope_of(ScopeKind::kSubscope, 1ULL));
  FDC_CHECK(scope_of(ScopeKind::kSubscope, 1ULL) < scope_of(ScopeKind::kSite, 1ULL));
  FDC_CHECK(!(scope_of(ScopeKind::kAsset, 1ULL) < scope_of(ScopeKind::kAsset, 1ULL)));

  FDC_CHECK(scope_of(ScopeKind::kAsset, 1ULL) == scope_of(ScopeKind::kAsset, 1ULL));
  FDC_CHECK(scope_of(ScopeKind::kAsset, 1ULL) != scope_of(ScopeKind::kRack, 1ULL));
  FDC_CHECK(scope_of(ScopeKind::kAsset, 1ULL) != scope_of(ScopeKind::kAsset, 2ULL));

  std::unordered_set<DrainScope> scopes;
  scopes.insert(scope_of(ScopeKind::kAsset, 1ULL));
  scopes.insert(scope_of(ScopeKind::kRack, 1ULL));
  scopes.insert(scope_of(ScopeKind::kAsset, 1ULL));
  FDC_CHECK_EQ(scopes.size(), 2U);
  FDC_CHECK(scopes.count(scope_of(ScopeKind::kAsset, 1ULL)) == 1U);
  FDC_CHECK(scopes.count(scope_of(ScopeKind::kZone, 1ULL)) == 0U);
}
