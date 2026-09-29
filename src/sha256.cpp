// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "sha256.hpp"

namespace facilitydrain {
namespace detail {
namespace {

// FIPS 180-4 section 4.2.2: the first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U};

// Rotations rather than shifts: SHA-256 is defined on 32 bit words with
// wraparound, and count is always in 1..31 here, so no shift is undefined.
[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t count) noexcept {
  return (value >> count) | (value << (32U - count));
}

// FIPS 180-4 section 4.1.2: the upper case sigma functions.
[[nodiscard]] constexpr std::uint32_t upper_sigma0(std::uint32_t word) noexcept {
  return rotr(word, 2U) ^ rotr(word, 13U) ^ rotr(word, 22U);
}
[[nodiscard]] constexpr std::uint32_t upper_sigma1(std::uint32_t word) noexcept {
  return rotr(word, 6U) ^ rotr(word, 11U) ^ rotr(word, 25U);
}
// FIPS 180-4 section 4.1.2: the lower case sigma functions.
[[nodiscard]] constexpr std::uint32_t lower_sigma0(std::uint32_t word) noexcept {
  return rotr(word, 7U) ^ rotr(word, 18U) ^ (word >> 3U);
}
[[nodiscard]] constexpr std::uint32_t lower_sigma1(std::uint32_t word) noexcept {
  return rotr(word, 17U) ^ rotr(word, 19U) ^ (word >> 10U);
}

[[nodiscard]] constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y,
                                             std::uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}
[[nodiscard]] constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y,
                                               std::uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

}  // namespace

Sha256::Sha256() noexcept
    // FIPS 180-4 section 5.3.3: the first 32 bits of the fractional parts of
    // the square roots of the first eight primes.
    : state_{0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU, 0x510E527FU, 0x9B05688CU,
             0x1F83D9ABU, 0x5BE0CD19U} {}

void Sha256::update(std::span<const std::byte> bytes) noexcept {
  // A finished instance is frozen: absorbing more afterwards would make the
  // already returned digest stale, which is exactly the kind of silent
  // disagreement this library must not have.
  if (finished_) {
    return;
  }

  total_bytes_ += static_cast<std::uint64_t>(bytes.size());

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    // Stage input into the fixed block buffer so compress() always sees 64
    // contiguous bytes and never has to reason about caller memory, alignment
    // or the size of the incoming span.
    const std::size_t space = kBlockSize - buffered_;
    const std::size_t available = bytes.size() - offset;
    const std::size_t take = available < space ? available : space;
    for (std::size_t index = 0; index < take; ++index) {
      buffer_[buffered_ + index] = std::to_integer<std::uint8_t>(bytes[offset + index]);
    }
    buffered_ += take;
    offset += take;

    if (buffered_ == kBlockSize) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::update(std::string_view text) noexcept {
  if (text.empty()) {
    // Nothing to absorb, and an empty view may carry a null data pointer that
    // must not be turned into a span.
    return;
  }
  update(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

ContentDigest Sha256::finish() noexcept {
  if (finished_) {
    return digest_;
  }

  // FIPS 180-4 section 5.1.1: append a single 0x80 byte, then zero bytes, then
  // the message length in bits as a 64 bit big-endian integer. The length
  // covers every byte ever absorbed, not just the final block.
  const std::uint64_t bit_length = total_bytes_ * 8U;

  std::array<std::uint8_t, kBlockSize> block{};
  for (std::size_t index = 0; index < buffered_; ++index) {
    block[index] = buffer_[index];
  }
  block[buffered_] = static_cast<std::uint8_t>(0x80U);

  // The length occupies the last eight bytes. When the terminator leaves less
  // than eight bytes of room in this block, the padding spills into one more.
  if (buffered_ >= kBlockSize - 8U) {
    compress(block.data());
    block.fill(std::uint8_t{0});
  }
  for (std::size_t index = 0; index < 8U; ++index) {
    const std::uint32_t shift = static_cast<std::uint32_t>(56U - 8U * index);
    block[56U + index] = static_cast<std::uint8_t>((bit_length >> shift) & 0xFFU);
  }
  compress(block.data());

  std::array<std::byte, ContentDigest::kSize> bytes{};
  for (std::size_t index = 0; index < kStateWords; ++index) {
    const std::uint32_t word = state_[index];
    const std::size_t base = index * 4U;
    bytes[base] = static_cast<std::byte>((word >> 24U) & 0xFFU);
    bytes[base + 1U] = static_cast<std::byte>((word >> 16U) & 0xFFU);
    bytes[base + 2U] = static_cast<std::byte>((word >> 8U) & 0xFFU);
    bytes[base + 3U] = static_cast<std::byte>(word & 0xFFU);
  }

  digest_ = ContentDigest::from_bytes(
      std::span<const std::byte, ContentDigest::kSize>(bytes.data(), bytes.size()));
  finished_ = true;
  return digest_;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  // Assemble the first sixteen schedule words big-endian by hand: reading them
  // as a 32 bit integer would be unaligned and host endianness dependent.
  std::array<std::uint32_t, kScheduleWords> schedule{};
  for (std::size_t index = 0; index < 16U; ++index) {
    const std::size_t base = index * 4U;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24U) |
                      (static_cast<std::uint32_t>(block[base + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(block[base + 2U]) << 8U) |
                      static_cast<std::uint32_t>(block[base + 3U]);
  }
  for (std::size_t index = 16U; index < kScheduleWords; ++index) {
    schedule[index] = lower_sigma1(schedule[index - 2U]) + schedule[index - 7U] +
                      lower_sigma0(schedule[index - 15U]) + schedule[index - 16U];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < kScheduleWords; ++index) {
    const std::uint32_t t1 =
        h + upper_sigma1(e) + choose(e, f, g) + kRoundConstants[index] + schedule[index];
    const std::uint32_t t2 = upper_sigma0(a) + majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  // Unsigned arithmetic wraps, which is the defined behaviour SHA-256 wants.
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

}  // namespace detail
}  // namespace facilitydrain
