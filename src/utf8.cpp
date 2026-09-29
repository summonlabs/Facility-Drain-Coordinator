// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "utf8.hpp"

#include "facilitydrain/identity.hpp"

#include <cstddef>
#include <string>

namespace facilitydrain {
namespace detail {
namespace {

// U+0000..U+001F and U+007F are the only code points validate_text() refuses on
// policy grounds. Every one of them is a single byte in UTF-8, so a byte scan
// finds them exactly once well-formedness has been established.
[[nodiscard]] constexpr bool is_control_byte(std::uint8_t value) noexcept {
  return value <= 0x1FU || value == 0x7FU;
}

[[nodiscard]] std::string field_prefix(std::string_view field_name) {
  std::string text{"field "};
  text.append(field_name);
  return text;
}

// "field label is 900 bytes, the bound is 256"
[[nodiscard]] std::string bound_detail(std::string_view field_name, std::size_t size,
                                       std::uint32_t max_bytes) {
  std::string detail = field_prefix(field_name);
  detail.append(" is ");
  detail.append(format_strong(static_cast<std::uint64_t>(size)));
  detail.append(" bytes, the bound is ");
  detail.append(format_strong(static_cast<std::uint64_t>(max_bytes)));
  return detail;
}

// "field label contains the control character U+001F"
[[nodiscard]] std::string control_detail(std::string_view field_name, std::uint8_t value) {
  constexpr char kHexDigits[] = "0123456789ABCDEF";
  std::string detail = field_prefix(field_name);
  detail.append(" contains the control character U+00");
  detail.push_back(kHexDigits[(value >> 4U) & 0x0FU]);
  detail.push_back(kHexDigits[value & 0x0FU]);
  return detail;
}

}  // namespace

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    // static_cast, not a signed char comparison: the byte value is wanted
    // modulo 256 regardless of whether char is signed on this platform.
    const std::uint32_t lead = static_cast<std::uint8_t>(text[index]);
    if (lead <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xC2U && lead <= 0xDFU) {
      continuation_count = 1;
      code_point = lead & 0x1FU;
      minimum = 0x80U;  // 0xC0 and 0xC1 would encode below this: overlong
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
      continuation_count = 2;
      code_point = lead & 0x0FU;
      minimum = 0x800U;
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
      continuation_count = 3;
      code_point = lead & 0x07U;
      minimum = 0x10000U;
    } else {
      // 0x80..0xC1 (stray continuation or overlong lead) and 0xF5..0xFF
      // (beyond U+10FFFF) are never valid lead bytes.
      return false;
    }

    // A sequence that runs past the end of the input is truncated, not valid.
    if (text.size() - index <= continuation_count) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const std::uint32_t continuation = static_cast<std::uint8_t>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }

    if (code_point < minimum) {
      return false;  // overlong encoding of a code point that has a shorter form
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
      return false;  // UTF-16 surrogate halves are not scalar values
    }
    if (code_point > 0x10FFFFU) {
      return false;  // beyond the Unicode range
    }
    index += continuation_count + 1U;
  }
  return true;
}

Status validate_text(std::string_view value, std::string_view field_name,
                     std::uint32_t max_bytes) {
  // Order is part of the contract: bound, then well-formedness, then policy.
  if (value.size() > static_cast<std::size_t>(max_bytes)) {
    return Status::failure(ErrorCode::kFieldTooLong, bound_detail(field_name, value.size(), max_bytes));
  }
  if (!is_valid_utf8(value)) {
    std::string detail = field_prefix(field_name);
    detail.append(" is not well-formed UTF-8");
    return Status::failure(ErrorCode::kInvalidText, detail);
  }
  for (const char character : value) {
    const std::uint8_t byte = static_cast<std::uint8_t>(character);
    if (is_control_byte(byte)) {
      return Status::failure(ErrorCode::kInvalidText, control_detail(field_name, byte));
    }
  }
  return Status::success();
}

}  // namespace detail
}  // namespace facilitydrain
