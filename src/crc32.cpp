// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "crc32.hpp"

#include <array>

namespace facilitydrain {
namespace detail {
namespace {

// The 256 entry table is generated at compile time rather than at first use:
// no lazy initialisation exists, so there is no initialisation race, no
// static mutable state, and identical behaviour in every translation unit and
// on every run.
[[nodiscard]] constexpr std::array<std::uint32_t, 256> build_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::size_t index = 0; index < table.size(); ++index) {
    std::uint32_t value = static_cast<std::uint32_t>(index);
    for (unsigned bit = 0; bit < 8U; ++bit) {
      // Reflected form: shift right and xor the reversed polynomial whenever
      // the bit leaving the register is set.
      value = (value & 1U) != 0U ? (value >> 1U) ^ 0xEDB88320U : (value >> 1U);
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32Table = build_crc32_table();

}  // namespace

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
  std::uint32_t remainder = 0xFFFFFFFFU;
  for (const std::byte value : bytes) {
    const std::uint32_t low = (remainder ^ std::to_integer<std::uint32_t>(value)) & 0xFFU;
    remainder = (remainder >> 8U) ^ kCrc32Table[static_cast<std::size_t>(low)];
  }
  return remainder ^ 0xFFFFFFFFU;
}

}  // namespace detail
}  // namespace facilitydrain
