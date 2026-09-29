// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "byte_codec.hpp"

#include <bit>

namespace facilitydrain {
namespace detail {
namespace {

// Appends width bytes of value, least significant first. width is at most
// eight, so every shift below stays inside the 64 bit value.
void append_little_endian(std::vector<std::byte>& out, std::uint64_t value, std::size_t width) {
  for (std::size_t index = 0; index < width; ++index) {
    const std::uint64_t shift = 8U * static_cast<std::uint64_t>(index);
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

// Reads width bytes, least significant first. On success the position advances;
// on failure nothing is written and the position is unchanged.
[[nodiscard]] bool read_little_endian(std::span<const std::byte> bytes, std::size_t& position,
                                      std::uint64_t& out, std::size_t width) noexcept {
  if (bytes.size() - position < width) {
    return false;
  }
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < width; ++index) {
    const std::uint64_t shift = 8U * static_cast<std::uint64_t>(index);
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[position + index]))
             << shift;
  }
  position += width;
  out = value;
  return true;
}

}  // namespace

void ByteWriter::u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u32(std::uint32_t value) {
  append_little_endian(bytes_, static_cast<std::uint64_t>(value), 4U);
}

void ByteWriter::u64(std::uint64_t value) { append_little_endian(bytes_, value, 8U); }

void ByteWriter::i64(std::int64_t value) {
  // bit_cast, not a value conversion: the encoding is the two's complement bit
  // pattern, and a negative value must not be sign extended into the prefix.
  append_little_endian(bytes_, std::bit_cast<std::uint64_t>(value), 8U);
}

void ByteWriter::raw(std::span<const std::byte> bytes) {
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void ByteWriter::sized_string(std::string_view text) {
  constexpr std::uint64_t kMaximumDeclarable = 0xFFFFFFFFULL;
  const std::uint64_t size = static_cast<std::uint64_t>(text.size());
  const std::uint32_t declared = size > kMaximumDeclarable
                                     ? static_cast<std::uint32_t>(kMaximumDeclarable)
                                     : static_cast<std::uint32_t>(size);
  u32(declared);
  raw(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

const std::vector<std::byte>& ByteWriter::data() const noexcept { return bytes_; }

std::vector<std::byte> ByteWriter::take() && { return std::move(bytes_); }

std::size_t ByteWriter::size() const noexcept { return bytes_.size(); }

void ByteWriter::reserve(std::size_t bytes) { bytes_.reserve(bytes); }

void ByteWriter::clear() noexcept { bytes_.clear(); }

ByteReader::ByteReader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

bool ByteReader::u8(std::uint8_t& out) noexcept {
  std::uint64_t value = 0;
  if (!read_little_endian(bytes_, position_, value, 1U)) {
    return false;
  }
  out = static_cast<std::uint8_t>(value);
  return true;
}

bool ByteReader::u32(std::uint32_t& out) noexcept {
  std::uint64_t value = 0;
  if (!read_little_endian(bytes_, position_, value, 4U)) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool ByteReader::u64(std::uint64_t& out) noexcept {
  return read_little_endian(bytes_, position_, out, 8U);
}

bool ByteReader::i64(std::int64_t& out) noexcept {
  std::uint64_t value = 0;
  if (!read_little_endian(bytes_, position_, value, 8U)) {
    return false;
  }
  out = std::bit_cast<std::int64_t>(value);
  return true;
}

bool ByteReader::sized_string(std::size_t max_length, std::string& out) {
  std::uint32_t declared = 0;
  if (!u32(declared)) {
    return false;
  }
  const std::size_t length = static_cast<std::size_t>(declared);

  // The bound is checked before the input is touched, so an over-long declared
  // field costs nothing and cannot be used to make a reader allocate.
  if (length > max_length) {
    limit_exceeded_ = true;
    return false;
  }
  if (length > remaining()) {
    return false;  // truncated input, left just after the length prefix
  }
  if (length == 0U) {
    out.clear();
    return true;
  }
  // Out of line construction: the caller's string is only touched once the
  // whole payload is known to be present, so a failure cannot leave a partial
  // value behind. Bytes hold object representation, so viewing them as char is
  // the one aliasing case the language defines.
  out.assign(reinterpret_cast<const char*>(bytes_.data() + position_), length);
  position_ += length;
  return true;
}

bool ByteReader::at_end() const noexcept { return position_ >= bytes_.size(); }

std::size_t ByteReader::remaining() const noexcept { return bytes_.size() - position_; }

std::size_t ByteReader::position() const noexcept { return position_; }

bool ByteReader::limit_exceeded() const noexcept { return limit_exceeded_; }

}  // namespace detail
}  // namespace facilitydrain
