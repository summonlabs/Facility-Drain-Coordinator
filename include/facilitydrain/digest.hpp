// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_DIGEST_HPP
#define FACILITYDRAIN_DIGEST_HPP

#include "facilitydrain/export.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Content digests
// ---------------------------------------------------------------------------
//
// A digest is the only way this system compares two externally authored
// payloads: consumer enumerations, policy documents, scope manifests, evidence
// payloads and durable states. Two payloads are the same payload when, and only
// when, their digests match.

class ContentDigest {
 public:
  static constexpr std::size_t kSize = 32;

  constexpr ContentDigest() noexcept = default;

  [[nodiscard]] static ContentDigest from_bytes(std::span<const std::byte, kSize> bytes) noexcept;

  /// Parses exactly 64 lowercase hexadecimal characters. Uppercase, 0x
  /// prefixes, separators and short or long forms are all rejected: exactly one
  /// spelling is canonical.
  [[nodiscard]] static std::optional<ContentDigest> from_hex(std::string_view text) noexcept;

  [[nodiscard]] std::string to_hex() const;

  /// True when every byte is zero. Such a digest is never produced by hashing;
  /// it means no digest recorded, and is treated as missing, never as a match.
  [[nodiscard]] bool is_zero() const noexcept;

  [[nodiscard]] std::span<const std::byte, kSize> bytes() const noexcept {
    return std::span<const std::byte, kSize>{bytes_.data(), kSize};
  }

  friend bool operator==(const ContentDigest& lhs, const ContentDigest& rhs) noexcept {
    return lhs.bytes_ == rhs.bytes_;
  }
  friend bool operator!=(const ContentDigest& lhs, const ContentDigest& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const ContentDigest& lhs, const ContentDigest& rhs) noexcept {
    return lhs.bytes_ < rhs.bytes_;
  }

 private:
  std::array<std::byte, kSize> bytes_{};
};

/// SHA-256 of a byte range.
[[nodiscard]] FACILITYDRAIN_API ContentDigest digest_bytes(std::span<const std::byte> bytes) noexcept;

/// SHA-256 of a string's bytes. The string is hashed as-is; no terminator is
/// included and no normalisation is performed.
[[nodiscard]] FACILITYDRAIN_API ContentDigest digest_text(std::string_view text) noexcept;

/// A domain separated digest over an ordered sequence of digests.
///
/// The sequence is hashed with its length and a caller supplied domain tag, so
/// a digest of two entries can never collide with a digest of one concatenated
/// entry, and evidence for one purpose can never be replayed as evidence for
/// another.
[[nodiscard]] FACILITYDRAIN_API ContentDigest combine_digests(std::string_view domain,
                                                              std::span<const ContentDigest> digests) noexcept;

}  // namespace facilitydrain

namespace std {
template <>
struct hash<facilitydrain::ContentDigest> {
  [[nodiscard]] std::size_t operator()(const facilitydrain::ContentDigest& value) const noexcept {
    std::size_t seed = 1469598103934665603ULL;
    const auto bytes = value.bytes();
    for (std::size_t index = 0; index < facilitydrain::ContentDigest::kSize; ++index) {
      seed = seed * 1099511628211ULL + static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[index]));
    }
    return seed;
  }
};
}  // namespace std

#endif  // FACILITYDRAIN_DIGEST_HPP
