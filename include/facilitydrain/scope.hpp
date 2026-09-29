// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SCOPE_HPP
#define FACILITYDRAIN_SCOPE_HPP

#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Physical scope
// ---------------------------------------------------------------------------
//
// A drain is always about one physical scope, named at one level of the
// facility hierarchy. The level is part of the identity: asset 17 and rack 17
// are different things, and no code path may compare them as equal.

enum class ScopeKind : std::uint8_t {
  kAsset = 1,
  kRack = 2,
  kZone = 3,
  kSubscope = 4,
  kSite = 5,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(ScopeKind kind) noexcept;
[[nodiscard]] FACILITYDRAIN_API std::optional<ScopeKind> scope_kind_from_token(std::string_view token) noexcept;
/// True for the levels a drain plan may target directly on its own.
[[nodiscard]] FACILITYDRAIN_API bool is_drainable_scope_kind(ScopeKind kind) noexcept;

struct DrainScope {
  ScopeKind kind = ScopeKind::kAsset;
  std::uint64_t id = 0;

  [[nodiscard]] bool is_default() const noexcept { return id == 0; }

  friend bool operator==(const DrainScope& lhs, const DrainScope& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.id == rhs.id;
  }
  friend bool operator!=(const DrainScope& lhs, const DrainScope& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const DrainScope& lhs, const DrainScope& rhs) noexcept {
    if (lhs.kind != rhs.kind) {
      return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
    }
    return lhs.id < rhs.id;
  }
};

/// "asset:17", "rack:4", "zone:2", "subscope:88", "site:1".
[[nodiscard]] FACILITYDRAIN_API std::string format_scope(const DrainScope& scope);

/// Strict canonical parse. Rejects unknown kinds, missing colons, leading
/// zeros, signs, whitespace, empty ids, and a zero id: scope 0 does not exist.
[[nodiscard]] FACILITYDRAIN_API Result<DrainScope> parse_scope(std::string_view text);

/// The ordered, duplicate free set of physical targets a plan covers.
///
/// The manifest is the physical membership the plan was planned against. It is
/// hashed into the plan binding and into every piece of evidence, so a member
/// that appears or disappears after planning fences the evidence that was taken
/// against the old membership instead of being silently absorbed.
class DrainTargetManifest {
 public:
  /// Canonicalises the input: sorts by (kind, id) and rejects duplicates, an
  /// empty set, more entries than the limit allows, and any target that is not
  /// a drainable level. The plan scope must be one of the targets.
  [[nodiscard]] static Result<DrainTargetManifest> create(std::vector<DrainScope> targets, const Limits& limits);

  [[nodiscard]] const std::vector<DrainScope>& targets() const noexcept { return targets_; }
  [[nodiscard]] std::size_t size() const noexcept { return targets_.size(); }
  [[nodiscard]] bool contains(const DrainScope& scope) const noexcept;

  /// Domain separated digest over the canonical membership.
  [[nodiscard]] ContentDigest digest() const;

  /// "asset:1|asset:2|rack:9" (the canonical membership order).
  [[nodiscard]] std::string to_canonical() const;

 private:
  std::vector<DrainScope> targets_{};
};

}  // namespace facilitydrain

namespace std {
template <>
struct hash<facilitydrain::DrainScope> {
  [[nodiscard]] std::size_t operator()(const facilitydrain::DrainScope& value) const noexcept {
    return std::hash<std::uint64_t>{}(value.id * 1099511628211ULL +
                                      static_cast<std::uint64_t>(value.kind));
  }
};
}  // namespace std

#endif  // FACILITYDRAIN_SCOPE_HPP
