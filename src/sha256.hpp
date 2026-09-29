// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_DETAIL_SHA256_HPP
#define FACILITYDRAIN_DETAIL_SHA256_HPP

#include "facilitydrain/digest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace facilitydrain {
namespace detail {

// FIPS 180-4 SHA-256, streaming.
//
// A digest is the only equality primitive this library has for externally
// authored payloads, so this is deliberately the plain, unextended algorithm
// from the standard: no truncation, no keying, no alternative spelling of the
// same content.
//
// Known answer tests (FIPS 180-4 examples, also in the NIST CAVP set) that the
// integrator's suite pins:
//   SHA-256("")    = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
//   SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
class Sha256 {
 public:
  Sha256() noexcept;

  /// Absorbs bytes. Input may arrive in arbitrarily sized pieces: where the
  /// call boundaries fall never changes the resulting digest.
  void update(std::span<const std::byte> bytes) noexcept;

  /// Absorbs a string's bytes exactly as stored. No terminator is included, no
  /// normalisation is performed, and no locale is consulted.
  void update(std::string_view text) noexcept;

  /// Finalises and returns the digest. Idempotent: a second call returns the
  /// same value, and updates after a finish are ignored, so a hasher can be
  /// completed from more than one code path without a second state machine.
  [[nodiscard]] ContentDigest finish() noexcept;

 private:
  static constexpr std::size_t kBlockSize = 64;
  static constexpr std::size_t kStateWords = 8;
  static constexpr std::size_t kScheduleWords = 64;

  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, kStateWords> state_{};  // chaining value h0..h7
  std::array<std::uint8_t, kBlockSize> buffer_{};   // staging for a partial block
  std::size_t buffered_ = 0;                        // valid bytes in buffer_
  std::uint64_t total_bytes_ = 0;                   // length of the whole message
  ContentDigest digest_{};                          // written once, by finish()
  bool finished_ = false;
};

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_DETAIL_SHA256_HPP
