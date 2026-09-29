// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/digest.hpp"

#include "sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace facilitydrain {
namespace {

// Domain separation labels. Each is a fixed ASCII literal, so the bytes hashed
// never depend on a caller's buffer, a locale or a platform.
constexpr std::string_view kCombineDomainLabel = "FDCD-DIGEST";
constexpr std::byte kCombineSeparator{0x1F};

// Returns 0..15 for a lowercase hexadecimal digit and 16 for anything else.
// Uppercase is deliberately "anything else": the canonical spelling is the only
// one that parses, so two texts that look alike cannot yield equal digests.
[[nodiscard]] constexpr std::uint8_t hex_nibble(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return static_cast<std::uint8_t>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(character - 'a') + 10U);
  }
  return 16U;
}

}  // namespace

ContentDigest ContentDigest::from_bytes(std::span<const std::byte, kSize> bytes) noexcept {
  ContentDigest digest;
  for (std::size_t index = 0; index < kSize; ++index) {
    digest.bytes_[index] = bytes[index];
  }
  return digest;
}

std::optional<ContentDigest> ContentDigest::from_hex(std::string_view text) noexcept {
  if (text.size() != kSize * 2U) {
    return std::nullopt;
  }
  std::array<std::byte, kSize> bytes{};
  for (std::size_t index = 0; index < kSize; ++index) {
    const std::uint8_t high = hex_nibble(text[index * 2U]);
    const std::uint8_t low = hex_nibble(text[index * 2U + 1U]);
    if (high > 0x0FU || low > 0x0FU) {
      return std::nullopt;
    }
    bytes[index] = static_cast<std::byte>(static_cast<std::uint8_t>((high << 4U) | low));
  }
  return ContentDigest::from_bytes(std::span<const std::byte, kSize>(bytes.data(), bytes.size()));
}

std::string ContentDigest::to_hex() const {
  constexpr char kHexDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(kSize * 2U);
  for (const std::byte value : bytes_) {
    const std::uint8_t byte = std::to_integer<std::uint8_t>(value);
    text.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
    text.push_back(kHexDigits[byte & 0x0FU]);
  }
  return text;
}

bool ContentDigest::is_zero() const noexcept {
  for (const std::byte value : bytes_) {
    if (std::to_integer<std::uint8_t>(value) != 0U) {
      return false;
    }
  }
  return true;
}

ContentDigest digest_bytes(std::span<const std::byte> bytes) noexcept {
  detail::Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
}

ContentDigest digest_text(std::string_view text) noexcept {
  detail::Sha256 hasher;
  hasher.update(text);
  return hasher.finish();
}

ContentDigest combine_digests(std::string_view domain,
                              std::span<const ContentDigest> digests) noexcept {
  // The construction is, in order:
  //
  //   "FDCD-DIGEST" | 0x1F | domain bytes | 0x1F | count (u32 LE) | digest 0 | digest 1 | ...
  //
  // Every part is either a fixed width field or separated by 0x1F, a byte that
  // cannot occur in the ASCII labels this library uses. That removes both
  // ambiguities that make naive concatenation unsafe:
  //
  //   * A count is present, so hashing one entry can never collide with hashing
  //     the two entries whose concatenated bytes happen to equal it.
  //   * A domain tag is present, so a digest computed for one purpose can never
  //     be replayed as the digest of another purpose, even when the same
  //     payloads are combined in the same order.
  //
  // Only the 32 raw bytes of each digest are absorbed; the hexadecimal form is
  // a rendering, never an input.
  detail::Sha256 hasher;
  hasher.update(kCombineDomainLabel);
  const std::array<std::byte, 1> separator{kCombineSeparator};
  hasher.update(std::span<const std::byte>(separator));
  hasher.update(domain);
  hasher.update(std::span<const std::byte>(separator));

  // A sequence longer than 2^32 entries would need more address space than the
  // process has, and every caller is bounded by Limits far below that.
  const std::uint32_t count = static_cast<std::uint32_t>(digests.size());
  const std::array<std::byte, 4> count_bytes{static_cast<std::byte>(count & 0xFFU),
                                             static_cast<std::byte>((count >> 8U) & 0xFFU),
                                             static_cast<std::byte>((count >> 16U) & 0xFFU),
                                             static_cast<std::byte>((count >> 24U) & 0xFFU)};
  hasher.update(std::span<const std::byte>(count_bytes));

  for (const ContentDigest& digest : digests) {
    hasher.update(digest.bytes());
  }
  return hasher.finish();
}

}  // namespace facilitydrain
