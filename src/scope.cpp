// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/scope.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace facilitydrain {

std::string_view to_token(ScopeKind kind) noexcept {
  // A complete switch with no default: adding a level to ScopeKind must force a
  // token to be chosen here rather than silently rendering as nothing.
  switch (kind) {
    case ScopeKind::kAsset:
      return "asset";
    case ScopeKind::kRack:
      return "rack";
    case ScopeKind::kZone:
      return "zone";
    case ScopeKind::kSubscope:
      return "subscope";
    case ScopeKind::kSite:
      return "site";
  }
  // An out of range value has no token. The empty token is the one spelling no
  // real level can produce, so callers can test for it instead of guessing.
  return std::string_view{};
}

std::optional<ScopeKind> scope_kind_from_token(std::string_view token) noexcept {
  // The exact inverse of to_token: only the five canonical tokens are accepted,
  // and every one of them names the level that produced it.
  if (token == "asset") {
    return ScopeKind::kAsset;
  }
  if (token == "rack") {
    return ScopeKind::kRack;
  }
  if (token == "zone") {
    return ScopeKind::kZone;
  }
  if (token == "subscope") {
    return ScopeKind::kSubscope;
  }
  if (token == "site") {
    return ScopeKind::kSite;
  }
  return std::nullopt;
}

bool is_drainable_scope_kind(ScopeKind kind) noexcept {
  // Every level of the facility hierarchy is a first class drain target: an
  // asset, a rack, a zone, a named subscope and a whole site each have an owning
  // system that can be asked to relinquish what it holds. The predicate is the
  // single place where a future level that is not drainable would be refused,
  // rather than every call site deciding for itself.
  switch (kind) {
    case ScopeKind::kAsset:
    case ScopeKind::kRack:
    case ScopeKind::kZone:
    case ScopeKind::kSubscope:
    case ScopeKind::kSite:
      return true;
  }
  return false;
}

std::string format_scope(const DrainScope& scope) {
  std::string text(to_token(scope.kind));
  text += ':';
  // The id is rendered through the canonical strong value formatter, so the
  // text format_scope produces and the text parse_scope accepts are the same
  // spelling: no padding, no sign, no separators.
  text += format_strong(scope.id);
  return text;
}

Result<DrainScope> parse_scope(std::string_view text) {
  // Every failure below is kInvalidScope, but each names the real reason, so a
  // caller can report why the text was refused without parsing it again.
  if (text.empty()) {
    return make_error<DrainScope>(ErrorCode::kInvalidScope, "scope text is empty");
  }
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return make_error<DrainScope>(ErrorCode::kInvalidScope, "scope text has no ':' separator");
  }
  // A scope names exactly one level and one id. A second separator means the
  // text is some other shape (a path, a composite key), which is refused rather
  // than partially understood.
  if (text.find(':', separator + 1) != std::string_view::npos) {
    return make_error<DrainScope>(ErrorCode::kInvalidScope, "scope text has more than one ':' separator");
  }
  const std::string_view kind_token = text.substr(0, separator);
  const std::string_view id_token = text.substr(separator + 1);
  const std::optional<ScopeKind> kind = scope_kind_from_token(kind_token);
  if (!kind.has_value()) {
    std::string detail = "unknown scope kind '";
    detail += kind_token;
    detail += "'";
    return make_error<DrainScope>(ErrorCode::kInvalidScope, detail);
  }
  if (id_token.empty()) {
    return make_error<DrainScope>(ErrorCode::kInvalidScope, "scope id is empty");
  }
  std::uint64_t id = 0;
  const IdentityParseError parse_error = parse_decimal(id_token, id);
  if (parse_error != IdentityParseError::kOk) {
    std::string detail = "scope id '";
    detail += id_token;
    detail += "' is not canonical decimal: ";
    detail += to_string(parse_error);
    return make_error<DrainScope>(ErrorCode::kInvalidScope, detail);
  }
  // Identity 0 is the default constructed value, not a physical scope, so it is
  // refused here rather than allowed to propagate as a real target.
  if (id == 0) {
    return make_error<DrainScope>(ErrorCode::kInvalidScope, "scope 0 does not exist");
  }
  return DrainScope{*kind, id};
}

Result<DrainTargetManifest> DrainTargetManifest::create(std::vector<DrainScope> targets, const Limits& limits) {
  // 1. Canonical order first. Duplicate detection, every later check and the
  //    stored manifest all walk this one order, so the same input always
  //    produces the same primary code and the same digest.
  std::sort(targets.begin(), targets.end());
  for (std::size_t index = 1; index < targets.size(); ++index) {
    if (targets[index] == targets[index - 1]) {
      std::string detail = "duplicate target ";
      detail += format_scope(targets[index]);
      return make_error<DrainTargetManifest>(ErrorCode::kDuplicateIdentifier, detail);
    }
  }
  // 2. A manifest that covers nothing covers no scope, and a plan over it could
  //    never be evaluated.
  if (targets.empty()) {
    return make_error<DrainTargetManifest>(ErrorCode::kMissingRequiredField,
                                           "a scope manifest must name at least one physical target");
  }
  // 3. The bound is a rejection, never a truncation: a manifest that was
  //    silently shortened would describe different physical membership from the
  //    one the plan was planned against.
  if (targets.size() > static_cast<std::size_t>(limits.max_targets_per_plan)) {
    std::string detail = "scope manifest has ";
    detail += format_strong(static_cast<std::uint64_t>(targets.size()));
    detail += " targets, limit is ";
    detail += format_strong(static_cast<std::uint64_t>(limits.max_targets_per_plan));
    return make_error<DrainTargetManifest>(ErrorCode::kTooManyEntries, detail);
  }
  // 4. Scope 0 does not exist, so it can never be a member of a manifest.
  for (const DrainScope& target : targets) {
    if (target.id == 0) {
      std::string detail = "target id 0 in scope kind '";
      detail += to_token(target.kind);
      detail += "' does not exist";
      return make_error<DrainTargetManifest>(ErrorCode::kInvalidScope, detail);
    }
  }
  // 5. Only levels that can actually be drained may be named, so a manifest can
  //    never demand an effect from a level that has no owner able to produce it.
  for (const DrainScope& target : targets) {
    if (!is_drainable_scope_kind(target.kind)) {
      std::string detail = "target id ";
      detail += format_strong(target.id);
      detail += " names a scope kind that is not drainable";
      return make_error<DrainTargetManifest>(ErrorCode::kInvalidScope, detail);
    }
  }
  DrainTargetManifest manifest;
  manifest.targets_ = std::move(targets);
  return manifest;
}

bool DrainTargetManifest::contains(const DrainScope& scope) const noexcept {
  // targets_ is stored in (kind, id) order, which is exactly DrainScope's
  // ordering, so this search is exact rather than an approximation over an
  // unordered list.
  return std::binary_search(targets_.begin(), targets_.end(), scope);
}

ContentDigest DrainTargetManifest::digest() const {
  // One digest per target over the text form that is rendered and parsed
  // elsewhere, combined under a domain tag, so a membership digest can never be
  // replayed as the digest of the same ids in another role.
  std::vector<ContentDigest> per_target;
  per_target.reserve(targets_.size());
  for (const DrainScope& target : targets_) {
    per_target.push_back(digest_text(format_scope(target)));
  }
  return combine_digests("scope-manifest", per_target);
}

std::string DrainTargetManifest::to_canonical() const {
  std::string text;
  for (std::size_t index = 0; index < targets_.size(); ++index) {
    if (index != 0) {
      text += '|';
    }
    text += format_scope(targets_[index]);
  }
  return text;
}

}  // namespace facilitydrain
