// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_DETAIL_CRC32_HPP
#define FACILITYDRAIN_DETAIL_CRC32_HPP

#include <cstddef>
#include <cstdint>
#include <span>

namespace facilitydrain {
namespace detail {

/// IEEE 802.3 CRC-32, the checksum used by zlib, gzip and PNG.
///
/// Parameters, stated exactly:
///   width            32 bits
///   polynomial       0x04C11DB7, used reflected as 0xEDB88320
///   initial value    0xFFFFFFFF
///   input reflected  yes, each byte is consumed least significant bit first
///   output reflected yes, which is what using the reflected polynomial means
///   final xor        0xFFFFFFFF
///   check value      0xCBF43926 over the ASCII bytes "123456789"
///   empty input      0x00000000
///
/// The checksum only detects accidental corruption. It is never an authority
/// or an identity: anything that must not be forgeable is compared by digest.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_DETAIL_CRC32_HPP
