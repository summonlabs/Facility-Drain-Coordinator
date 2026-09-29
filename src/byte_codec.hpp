// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_DETAIL_BYTE_CODEC_HPP
#define FACILITYDRAIN_DETAIL_BYTE_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace facilitydrain {
namespace detail {

// Canonical little-endian codec for durable payloads and digest inputs.
//
// Every integer is fixed width and little-endian and every variable length
// field carries an explicit 32 bit length prefix, so a reader never has to
// infer a field's size from its contents. There is no padding, no alignment
// requirement and no host endianness dependence: a payload written on one
// machine decodes identically on every other, and its bytes are the bytes that
// were hashed.
//
// Integers are assembled and split one byte at a time. That is well defined for
// unaligned buffers, cannot trip strict aliasing, and avoids the class of bug
// where a struct is memcpy'd onto the wire and its layout silently becomes part
// of the format.

class ByteWriter {
 public:
  /// Appends one byte.
  void u8(std::uint8_t value);
  /// Appends four bytes, least significant first.
  void u32(std::uint32_t value);
  /// Appends eight bytes, least significant first.
  void u64(std::uint64_t value);
  /// Appends eight bytes of the two's complement representation, least
  /// significant first.
  void i64(std::int64_t value);
  /// Appends the bytes verbatim.
  void raw(std::span<const std::byte> bytes);
  /// Appends a 32 bit little-endian byte length followed by the text bytes.
  ///
  /// The prefix is 32 bits wide and the text length is a size_t. A text larger
  /// than the prefix can express (more than 4 GiB, unreachable through any
  /// Limits bound) is written with a saturating 0xFFFFFFFF prefix: silently
  /// wrapping the length could produce a plausible smaller one, whereas the
  /// saturated value is refused by every conforming reader before it reads a
  /// single payload byte.
  void sized_string(std::string_view text);

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept;
  /// Consumes the buffer. Taking is only possible from an rvalue writer, so no
  /// caller can keep appending to a buffer whose storage has moved away.
  [[nodiscard]] std::vector<std::byte> take() &&;
  [[nodiscard]] std::size_t size() const noexcept;
  void reserve(std::size_t bytes);
  void clear() noexcept;

 private:
  std::vector<std::byte> bytes_;
};

// The reader counterpart.
//
// It never throws and never allocates more than the caller supplied bound: a
// sized_string whose declared length exceeds max_length sets limit_exceeded()
// and fails without reading, let alone materialising, the payload. Every
// failing read leaves the reader exactly where the failure happened and leaves
// the caller's output parameter untouched, so a half decoded record can never
// be mistaken for a decoded one.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> bytes) noexcept;

  /// Each of these consumes its field on success and consumes nothing on
  /// failure. The integer is assembled byte by byte, least significant first.
  [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool i64(std::int64_t& out) noexcept;

  /// Reads a 32 bit length prefix and then that many text bytes.
  ///
  /// Fails, setting nothing, when the prefix itself cannot be read. Fails with
  /// limit_exceeded() set, having consumed only the prefix, when the declared
  /// length is greater than max_length. Fails with the reader left just after
  /// the prefix when the declared length runs past the end of the input, which
  /// is truncation rather than an over-long field.
  [[nodiscard]] bool sized_string(std::size_t max_length, std::string& out);

  [[nodiscard]] bool at_end() const noexcept;
  [[nodiscard]] std::size_t remaining() const noexcept;
  [[nodiscard]] std::size_t position() const noexcept;
  [[nodiscard]] bool limit_exceeded() const noexcept;

 private:
  std::span<const std::byte> bytes_;
  std::size_t position_ = 0;
  bool limit_exceeded_ = false;
};

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_DETAIL_BYTE_CODEC_HPP
