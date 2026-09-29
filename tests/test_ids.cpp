// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace {

using facilitydrain::AssetId;
using facilitydrain::IdentityParseError;
using facilitydrain::ObservationSequence;
using facilitydrain::PlanId;
using facilitydrain::RackId;
using facilitydrain::Revision;
using facilitydrain::ZoneId;
using facilitydrain::format_strong;
using facilitydrain::parse_decimal;
using facilitydrain::parse_strong;
using facilitydrain::to_string;
using facilitydrain::try_parse;

constexpr std::uint64_t kUint64Maximum = (std::numeric_limits<std::uint64_t>::max)();
constexpr IdentityParseError kOk = IdentityParseError::kOk;

// ---------------------------------------------------------------------------
// Compile time proofs
// ---------------------------------------------------------------------------
//
// Strong types with the same representation are deliberately not
// interchangeable. Every conversion below is rejected by the compiler, so a
// call site that mixes an asset with a rack, or a revision with an observation
// sequence, does not build instead of silently comparing raw values.

static_assert(!std::is_convertible_v<AssetId, RackId>,
              "an asset identity must not be usable as a rack identity");
static_assert(!std::is_convertible_v<RackId, AssetId>,
              "a rack identity must not be usable as an asset identity");
static_assert(!std::is_same_v<AssetId, RackId>, "each tag names its own complete type");
static_assert(!std::is_convertible_v<AssetId, ZoneId>,
              "an asset identity must not be usable as a zone identity");
static_assert(!std::is_convertible_v<PlanId, AssetId>, "a plan identity is not an asset identity");
static_assert(!std::is_convertible_v<Revision, ObservationSequence>,
              "a revision is not an observation sequence");
static_assert(!std::is_convertible_v<std::uint64_t, AssetId>,
              "the raw value constructor is explicit, so a bare integer is never an identity");
static_assert(!std::is_assignable_v<AssetId&, RackId>,
              "an identity cannot be assigned into another identity type");
static_assert(std::is_same_v<decltype(Revision{}.next()), Revision>, "next() preserves the strong type");

// The exact width of the canonical decimal form: twenty digits is UINT64_MAX,
// so no rendering can ever be wider.
constexpr std::size_t kMaxDecimalDigits = 20;

[[nodiscard]] std::string to_canonical_hex(std::uint64_t value) {
  if (value == 0ULL) {
    return std::string{"0"};
  }
  std::string digits;
  std::uint64_t remaining = value;
  while (remaining != 0ULL) {
    const std::uint32_t nibble = static_cast<std::uint32_t>(remaining & 0x0FULL);
    digits.push_back(static_cast<char>(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U))));
    remaining >>= 4U;
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

[[nodiscard]] std::string upper_case(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    if (character >= 'a' && character <= 'z') {
      result.push_back(static_cast<char>(character - 'a' + 'A'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] bool is_all_decimal_digits(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

constexpr std::array<IdentityParseError, 9> kAllIdentityParseErrors{
    IdentityParseError::kOk,
    IdentityParseError::kEmpty,
    IdentityParseError::kLeadingWhitespace,
    IdentityParseError::kTrailingWhitespace,
    IdentityParseError::kInvalidCharacter,
    IdentityParseError::kOverflow,
    IdentityParseError::kNegativeSign,
    IdentityParseError::kHexWithoutDigits,
    IdentityParseError::kDecimalLeadingZero};

}  // namespace

// ---------------------------------------------------------------------------
// Canonical text form
// ---------------------------------------------------------------------------

FDC_TEST(ids, format_strong_is_never_padded_or_signed) {
  FDC_CHECK_EQ(format_strong(0ULL), std::string{"0"});
  FDC_CHECK_EQ(format_strong(1ULL), std::string{"1"});
  FDC_CHECK_EQ(format_strong(9ULL), std::string{"9"});
  FDC_CHECK_EQ(format_strong(10ULL), std::string{"10"});
  FDC_CHECK_EQ(format_strong(1000000ULL), std::string{"1000000"});
  FDC_CHECK_EQ(format_strong(kUint64Maximum), std::string{"18446744073709551615"});
  FDC_CHECK_EQ(format_strong(kUint64Maximum).size(), kMaxDecimalDigits);
  FDC_CHECK_EQ(format_strong(kUint64Maximum - 1ULL), std::string{"18446744073709551614"});
  // Exactly one spelling: no padding, no sign, no separator.
  FDC_CHECK(format_strong(7ULL).find('+') == std::string::npos);
  FDC_CHECK(format_strong(7ULL).find('-') == std::string::npos);
  FDC_CHECK(format_strong(7ULL) != std::string{"07"});
}

FDC_TEST(ids, decimal_parse_round_trips) {
  constexpr std::array<std::uint64_t, 12> kValues{0ULL,
                                                  1ULL,
                                                  9ULL,
                                                  10ULL,
                                                  99ULL,
                                                  100ULL,
                                                  255ULL,
                                                  1000ULL,
                                                  65536ULL,
                                                  4294967295ULL,
                                                  18446744073709551614ULL,
                                                  kUint64Maximum};
  for (const std::uint64_t value : kValues) {
    const std::string text = format_strong(value);
    FDC_CHECK(is_all_decimal_digits(text));
    if (value == 0ULL) {
      FDC_CHECK_EQ(text, std::string{"0"});
    } else {
      FDC_CHECK(text.front() != '0');
    }

    std::uint64_t parsed = kUint64Maximum;
    FDC_CHECK_EQ(parse_decimal(text, parsed), kOk);
    FDC_CHECK_EQ(parsed, value);

    // parse_strong accepts the canonical decimal spelling unchanged, so the two
    // entry points can never disagree about a decimal form.
    std::uint64_t strong_parsed = 0ULL;
    FDC_CHECK_EQ(parse_strong(text, strong_parsed), kOk);
    FDC_CHECK_EQ(strong_parsed, value);
  }
}

FDC_TEST(ids, hexadecimal_parse_round_trips) {
  constexpr std::array<std::uint64_t, 12> kValues{0ULL,
                                                  1ULL,
                                                  9ULL,
                                                  10ULL,
                                                  15ULL,
                                                  16ULL,
                                                  255ULL,
                                                  4096ULL,
                                                  4294967295ULL,
                                                  4294967296ULL,
                                                  18446744073709551614ULL,
                                                  kUint64Maximum};
  for (const std::uint64_t value : kValues) {
    const std::string digits = to_canonical_hex(value);
    const std::string text = "0x" + digits;
    std::uint64_t parsed = 0ULL;
    FDC_CHECK_EQ(parse_strong(text, parsed), kOk);
    FDC_CHECK_EQ(parsed, value);

    // Both spellings of one value are the same value.
    std::uint64_t decimal_parsed = 0ULL;
    FDC_CHECK_EQ(parse_strong(format_strong(value), decimal_parsed), kOk);
    FDC_CHECK_EQ(decimal_parsed, parsed);

    // A hexadecimal form has exactly one canonical spelling: a leading zero
    // digit beyond a single "0" is not a second one.
    if (digits.size() > 1U) {
      std::uint64_t unused = 0ULL;
      FDC_CHECK_EQ(parse_strong("0x0" + digits, unused), IdentityParseError::kDecimalLeadingZero);
      FDC_CHECK_EQ(parse_strong("0X" + digits, unused), IdentityParseError::kInvalidCharacter);
      const std::string uppercase_digits = upper_case(digits);
      if (uppercase_digits != digits) {
        FDC_CHECK_EQ(parse_strong("0x" + uppercase_digits, unused), IdentityParseError::kInvalidCharacter);
      }
    }
  }
}

FDC_TEST(ids, empty_text_is_rejected) {
  std::uint64_t out = 0ULL;
  FDC_CHECK_EQ(parse_decimal(std::string_view{}, out), IdentityParseError::kEmpty);
  FDC_CHECK_EQ(parse_strong(std::string_view{}, out), IdentityParseError::kEmpty);
  FDC_CHECK_EQ(out, 0ULL);
}

FDC_TEST(ids, leading_whitespace_is_rejected) {
  constexpr std::array<std::string_view, 6> kTexts{" 1", "\t1", "\n1", "\r1", "\v1", "\f1"};
  for (const std::string_view text : kTexts) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_decimal(text, out), IdentityParseError::kLeadingWhitespace);
    FDC_CHECK_EQ(parse_strong(text, out), IdentityParseError::kLeadingWhitespace);
    FDC_CHECK_EQ(out, 0ULL);
  }
}

FDC_TEST(ids, trailing_whitespace_is_rejected) {
  constexpr std::array<std::string_view, 6> kTexts{"1 ", "1\t", "1\n", "1\r", "1\v", "1\f"};
  for (const std::string_view text : kTexts) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_decimal(text, out), IdentityParseError::kTrailingWhitespace);
    FDC_CHECK_EQ(parse_strong(text, out), IdentityParseError::kTrailingWhitespace);
    FDC_CHECK_EQ(out, 0ULL);
  }
  // Whitespace on both sides reports the leading kind: the documented order is
  // fixed, so one text always yields exactly one error.
  std::uint64_t out = 0ULL;
  FDC_CHECK_EQ(parse_decimal(" 1 ", out), IdentityParseError::kLeadingWhitespace);
}

FDC_TEST(ids, invalid_characters_are_rejected) {
  constexpr std::array<std::string_view, 8> kTexts{"12a", "1a2", "1.5", "1,000", "1_000", "1e3", "abc", "one"};
  for (const std::string_view text : kTexts) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_decimal(text, out), IdentityParseError::kInvalidCharacter);
    FDC_CHECK_EQ(out, 0ULL);
  }
  // A plus sign is not a second spelling of a positive number.
  std::uint64_t out = 0ULL;
  FDC_CHECK_EQ(parse_decimal("+1", out), IdentityParseError::kInvalidCharacter);
  FDC_CHECK_EQ(parse_strong("+1", out), IdentityParseError::kInvalidCharacter);

  // The hexadecimal branch reports the same kind for a bad digit, and an upper
  // case prefix is an invalid character rather than an accepted alias.
  FDC_CHECK_EQ(parse_strong("0x1g", out), IdentityParseError::kInvalidCharacter);
  FDC_CHECK_EQ(parse_strong("0x1G", out), IdentityParseError::kInvalidCharacter);
  FDC_CHECK_EQ(parse_strong("0xabz", out), IdentityParseError::kInvalidCharacter);
  FDC_CHECK_EQ(parse_strong("0xAB", out), IdentityParseError::kInvalidCharacter);
  FDC_CHECK_EQ(parse_strong("0X10", out), IdentityParseError::kInvalidCharacter);
  FDC_CHECK_EQ(out, 0ULL);
}

FDC_TEST(ids, overflow_is_rejected) {
  constexpr std::array<std::string_view, 5> kDecimal{"18446744073709551616",
                                                     "18446744073709551617",
                                                     "99999999999999999999",
                                                     "100000000000000000000",
                                                     "340282366920938463463374607431768211456"};
  for (const std::string_view text : kDecimal) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_decimal(text, out), IdentityParseError::kOverflow);
    FDC_CHECK_EQ(out, 0ULL);
  }
  constexpr std::array<std::string_view, 2> kHex{"0x10000000000000000", "0xffffffffffffffffff"};
  for (const std::string_view text : kHex) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_strong(text, out), IdentityParseError::kOverflow);
    FDC_CHECK_EQ(out, 0ULL);
  }
  // The largest value that does fit is accepted, so the bound is exact.
  std::uint64_t out = 0ULL;
  FDC_CHECK_EQ(parse_decimal("18446744073709551615", out), kOk);
  FDC_CHECK_EQ(out, kUint64Maximum);
  FDC_CHECK_EQ(parse_strong("0xffffffffffffffff", out), kOk);
  FDC_CHECK_EQ(out, kUint64Maximum);
}

FDC_TEST(ids, negative_sign_is_rejected) {
  constexpr std::array<std::string_view, 4> kTexts{"-1", "-0", "-18446744073709551615", "-"};
  for (const std::string_view text : kTexts) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_decimal(text, out), IdentityParseError::kNegativeSign);
    FDC_CHECK_EQ(parse_strong(text, out), IdentityParseError::kNegativeSign);
    FDC_CHECK_EQ(out, 0ULL);
  }
}

FDC_TEST(ids, hexadecimal_without_digits_is_rejected) {
  std::uint64_t out = 0ULL;
  FDC_CHECK_EQ(parse_strong("0x", out), IdentityParseError::kHexWithoutDigits);
  FDC_CHECK_EQ(out, 0ULL);
  // "0x" followed by a space is a trailing whitespace problem, which is
  // reported first, and "0" alone is plain decimal zero.
  FDC_CHECK_EQ(parse_strong("0x ", out), IdentityParseError::kTrailingWhitespace);
  FDC_CHECK_EQ(parse_decimal("0", out), kOk);
  FDC_CHECK_EQ(parse_strong("0x0", out), kOk);
  FDC_CHECK_EQ(out, 0ULL);
}

FDC_TEST(ids, decimal_leading_zero_is_rejected) {
  constexpr std::array<std::string_view, 4> kDecimal{"00", "01", "007", "0123456789"};
  for (const std::string_view text : kDecimal) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_decimal(text, out), IdentityParseError::kDecimalLeadingZero);
    FDC_CHECK_EQ(parse_strong(text, out), IdentityParseError::kDecimalLeadingZero);
    FDC_CHECK_EQ(out, 0ULL);
  }
  constexpr std::array<std::string_view, 3> kHex{"0x01", "0x00", "0x000a"};
  for (const std::string_view text : kHex) {
    std::uint64_t out = 0ULL;
    FDC_CHECK_EQ(parse_strong(text, out), IdentityParseError::kDecimalLeadingZero);
    FDC_CHECK_EQ(out, 0ULL);
  }
}

FDC_TEST(ids, parse_failure_leaves_the_output_untouched) {
  constexpr std::array<std::string_view, 9> kFailingTexts{
      "", " 1", "1 ", "-1", "+1", "01", "12a", "0x", "18446744073709551616"};
  for (const std::string_view text : kFailingTexts) {
    std::uint64_t out = 42ULL;
    FDC_CHECK(parse_decimal(text, out) != kOk);
    FDC_CHECK_EQ(out, 42ULL);
    out = 42ULL;
    FDC_CHECK(parse_strong(text, out) != kOk);
    FDC_CHECK_EQ(out, 42ULL);
  }
}

FDC_TEST(ids, error_tokens_are_canonical_and_unique) {
  FDC_CHECK_EQ(to_string(IdentityParseError::kOk), std::string_view{"ok"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kEmpty), std::string_view{"empty"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kLeadingWhitespace), std::string_view{"leading-whitespace"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kTrailingWhitespace), std::string_view{"trailing-whitespace"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kInvalidCharacter), std::string_view{"invalid-character"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kOverflow), std::string_view{"overflow"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kNegativeSign), std::string_view{"negative-sign"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kHexWithoutDigits), std::string_view{"hex-without-digits"});
  FDC_CHECK_EQ(to_string(IdentityParseError::kDecimalLeadingZero), std::string_view{"decimal-leading-zero"});

  for (const IdentityParseError left : kAllIdentityParseErrors) {
    FDC_CHECK(!to_string(left).empty());
    for (const IdentityParseError right : kAllIdentityParseErrors) {
      if (left != right) {
        FDC_CHECK(to_string(left) != to_string(right));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Typed helpers
// ---------------------------------------------------------------------------

FDC_TEST(ids, typed_values_render_and_parse) {
  FDC_CHECK_EQ(to_string(PlanId{7ULL}), std::string{"7"});
  FDC_CHECK_EQ(to_string(AssetId{kUint64Maximum}), std::string{"18446744073709551615"});

  IdentityParseError error = IdentityParseError::kInvalidCharacter;
  // try_parse is parameterised by the tag, so the alias the caller stores is
  // exactly the alias the parser was asked for.
  const std::optional<PlanId> parsed = try_parse<facilitydrain::tags::PlanIdTag>("42", error);
  FDC_CHECK_EQ(error, IdentityParseError::kOk);
  FDC_REQUIRE(parsed.has_value());
  FDC_CHECK_EQ(parsed->value(), 42ULL);

  const std::optional<PlanId> parsed_hex = try_parse<facilitydrain::tags::PlanIdTag>("0x2a", error);
  FDC_CHECK_EQ(error, IdentityParseError::kOk);
  FDC_REQUIRE(parsed_hex.has_value());
  FDC_CHECK_EQ(parsed_hex->value(), 42ULL);

  const std::optional<PlanId> rejected = try_parse<facilitydrain::tags::PlanIdTag>("0x2A", error);
  FDC_CHECK(!rejected.has_value());
  FDC_CHECK_EQ(error, IdentityParseError::kInvalidCharacter);
}

// ---------------------------------------------------------------------------
// Ordering, hashing and advancing
// ---------------------------------------------------------------------------

FDC_TEST(ids, ordering_of_strong_values) {
  const AssetId one{1ULL};
  const AssetId two{2ULL};
  const AssetId also_two{2ULL};

  FDC_CHECK(one < two);
  FDC_CHECK(two > one);
  FDC_CHECK(one <= two);
  FDC_CHECK(one <= one);
  FDC_CHECK(two >= one);
  FDC_CHECK(one != two);
  FDC_CHECK(two == also_two);
  FDC_CHECK(!(one < one));
  FDC_CHECK(!(one > one));

  const AssetId zero;
  FDC_CHECK(zero.is_default());
  FDC_CHECK(zero == AssetId{0ULL});
  FDC_CHECK(!one.is_default());
  FDC_CHECK_EQ(zero.value(), 0ULL);

  // max() is the largest representable value and is never the default.
  FDC_CHECK_EQ(AssetId::max().value(), kUint64Maximum);
  FDC_CHECK(!AssetId::max().is_default());
  FDC_CHECK(one < AssetId::max());
}

FDC_TEST(ids, next_advances_within_the_type) {
  const Revision first{1ULL};
  const Revision second = first.next();
  FDC_CHECK_EQ(second.value(), 2ULL);
  // next() returns a new value and leaves the original alone.
  FDC_CHECK_EQ(first.value(), 1ULL);

  const Revision from_zero;
  FDC_CHECK(from_zero.is_default());
  const Revision advanced = from_zero.next();
  FDC_CHECK_EQ(advanced.value(), 1ULL);
  FDC_CHECK(!advanced.is_default());

  const ObservationSequence sequence{1000ULL};
  FDC_CHECK_EQ(sequence.next().value(), 1001ULL);
  FDC_CHECK(sequence < sequence.next());
}

FDC_TEST(ids, hashing_of_strong_values) {
  FDC_CHECK_EQ(std::hash<AssetId>{}(AssetId{7ULL}), std::hash<std::uint64_t>{}(7ULL));
  FDC_CHECK_EQ(std::hash<PlanId>{}(PlanId{7ULL}), std::hash<std::uint64_t>{}(7ULL));
  FDC_CHECK_EQ(std::hash<AssetId>{}(AssetId{0ULL}), std::hash<std::uint64_t>{}(0ULL));

  std::unordered_set<PlanId> plans;
  plans.insert(PlanId{1ULL});
  plans.insert(PlanId{1ULL});
  plans.insert(PlanId{2ULL});
  FDC_CHECK_EQ(plans.size(), 2U);
  FDC_CHECK(plans.count(PlanId{1ULL}) == 1U);
  FDC_CHECK(plans.count(PlanId{3ULL}) == 0U);

  std::unordered_map<Revision, std::string> by_revision;
  by_revision.emplace(Revision{1ULL}, std::string{"first"});
  by_revision.emplace(Revision{2ULL}, std::string{"second"});
  FDC_CHECK_EQ(by_revision.size(), 2U);
  FDC_CHECK_EQ(by_revision.at(Revision{2ULL}), std::string{"second"});
}
