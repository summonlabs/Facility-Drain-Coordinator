// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace facilitydrain {
namespace {

struct TokenEntry {
  IdentityParseError error;
  std::string_view token;
};

// One table drives both the diagnostic spelling and the canonical list, so a
// new enumerator cannot be given two different names in two places.
constexpr auto kIdentityTokens = std::array{
    TokenEntry{IdentityParseError::kOk, "ok"},
    TokenEntry{IdentityParseError::kEmpty, "empty"},
    TokenEntry{IdentityParseError::kLeadingWhitespace, "leading-whitespace"},
    TokenEntry{IdentityParseError::kTrailingWhitespace, "trailing-whitespace"},
    TokenEntry{IdentityParseError::kInvalidCharacter, "invalid-character"},
    TokenEntry{IdentityParseError::kOverflow, "overflow"},
    TokenEntry{IdentityParseError::kNegativeSign, "negative-sign"},
    TokenEntry{IdentityParseError::kHexWithoutDigits, "hex-without-digits"},
    TokenEntry{IdentityParseError::kDecimalLeadingZero, "decimal-leading-zero"},
};

// A value outside the enumeration has no canonical spelling. "unknown" is not
// in the table, so it never round-trips into a real error.
constexpr std::string_view kUnknownToken = "unknown";

// The six characters the canonical form treats as whitespace. Anything else
// that is not a digit is an invalid character, not padding to be trimmed:
// accepting what a caller did not mean is how two identities become one.
[[nodiscard]] constexpr bool is_whitespace(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
         character == '\v' || character == '\f';
}

[[nodiscard]] constexpr std::uint64_t max_value() noexcept {
  return (std::numeric_limits<std::uint64_t>::max)();
}

}  // namespace

std::string_view to_string(IdentityParseError error) noexcept {
  for (const TokenEntry& entry : kIdentityTokens) {
    if (entry.error == error) {
      return entry.token;
    }
  }
  return kUnknownToken;
}

std::string format_strong(std::uint64_t raw) {
  if (raw == 0U) {
    return std::string{"0"};
  }
  // Twenty digits is the exact width of UINT64_MAX, so the buffer can never be
  // overrun and no length check is needed inside the loop.
  std::array<char, 20> digits{};
  std::size_t begin = digits.size();
  while (raw != 0U) {
    --begin;
    digits[begin] = static_cast<char>('0' + static_cast<int>(raw % 10U));
    raw /= 10U;
  }
  return std::string{digits.data() + begin, digits.size() - begin};
}

IdentityParseError parse_decimal(std::string_view text, std::uint64_t& out) noexcept {
  // Fixed order, first violation wins, so one text always yields one error:
  // empty, leading whitespace, trailing whitespace, sign, leading zero,
  // non-digit character, overflow.
  if (text.empty()) {
    return IdentityParseError::kEmpty;
  }
  if (is_whitespace(text.front())) {
    return IdentityParseError::kLeadingWhitespace;
  }
  if (is_whitespace(text.back())) {
    return IdentityParseError::kTrailingWhitespace;
  }
  if (text.front() == '-') {
    return IdentityParseError::kNegativeSign;
  }
  if (text.front() == '+') {
    // A plus sign is not a second way to spell a positive number; there is
    // exactly one canonical spelling and this is not it.
    return IdentityParseError::kInvalidCharacter;
  }
  if (text.size() > 1U && text.front() == '0') {
    return IdentityParseError::kDecimalLeadingZero;
  }

  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return IdentityParseError::kInvalidCharacter;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
    // Refuse before multiplying: value * 10 would already have wrapped and the
    // caller would receive a plausible identity that is not the one they typed.
    if (value > (max_value() - digit) / 10U) {
      return IdentityParseError::kOverflow;
    }
    value = value * 10U + digit;
  }

  out = value;  // written only on success; a failure never yields a partial value
  return IdentityParseError::kOk;
}

IdentityParseError parse_strong(std::string_view text, std::uint64_t& out) noexcept {
  // The same fixed order as parse_decimal, with the hexadecimal branch taken
  // only for a well formed "0x" prefix:
  // empty, leading whitespace, trailing whitespace, sign, then either the
  // hexadecimal checks or the decimal ones.
  if (text.empty()) {
    return IdentityParseError::kEmpty;
  }
  if (is_whitespace(text.front())) {
    return IdentityParseError::kLeadingWhitespace;
  }
  if (is_whitespace(text.back())) {
    return IdentityParseError::kTrailingWhitespace;
  }
  if (text.front() == '-') {
    return IdentityParseError::kNegativeSign;
  }
  if (text.front() == '+') {
    return IdentityParseError::kInvalidCharacter;
  }

  const bool has_prefix =
      text.size() >= 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  if (!has_prefix) {
    // Decimal, byte for byte identical to parse_decimal so the two entry
    // points can never disagree about a decimal spelling.
    return parse_decimal(text, out);
  }

  // An upper case prefix is not a second spelling of the prefix; it is an
  // invalid character, exactly like an upper case digit.
  if (text[1] == 'X') {
    return IdentityParseError::kInvalidCharacter;
  }
  if (text.size() == 2U) {
    return IdentityParseError::kHexWithoutDigits;
  }

  const std::string_view digits = text.substr(2U);
  // Leading zeros after the prefix are rejected for the same reason they are
  // in decimal: 0x0A and 0x000A would otherwise be two identities of one value.
  if (digits.size() > 1U && digits.front() == '0') {
    return IdentityParseError::kDecimalLeadingZero;
  }
  // A canonical prefix form has no leading zero beyond a single "0", so a
  // seventeen digit value is at least 16^16 == 2^64 and cannot fit. The digit
  // count is checked as well as the accumulated value so the stated bound is
  // enforced directly rather than only as a consequence.
  if (digits.size() > 16U) {
    return IdentityParseError::kOverflow;
  }

  std::uint64_t value = 0;
  for (const char character : digits) {
    std::uint32_t nibble = 0;
    if (character >= '0' && character <= '9') {
      nibble = static_cast<std::uint32_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      nibble = static_cast<std::uint32_t>(character - 'a') + 10U;
    } else {
      return IdentityParseError::kInvalidCharacter;
    }
    value = (value << 4U) | nibble;
  }

  out = value;
  return IdentityParseError::kOk;
}

}  // namespace facilitydrain
