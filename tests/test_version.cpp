// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using facilitydrain::format_strong;
using facilitydrain::kContainerFormatVersion;
using facilitydrain::kContainerHeaderSize;
using facilitydrain::kContainerMagic;
using facilitydrain::kGenerationFilePrefix;
using facilitydrain::kGenerationFileSuffix;
using facilitydrain::kPointerFileName;
using facilitydrain::kPointerMagic;
using facilitydrain::kPointerRecordSize;
using facilitydrain::kReportFormatVersion;
using facilitydrain::kRetainedGenerations;
using facilitydrain::kStateFormatVersion;
using facilitydrain::kStoreLayoutVersion;
using facilitydrain::kTransientFilePrefix;
using facilitydrain::kVersionMajor;
using facilitydrain::kVersionMinor;
using facilitydrain::kVersionPatch;
using facilitydrain::kWriterLockFileName;
using facilitydrain::version_string;

// The width of the zero padded decimal field in a generation file name.
constexpr std::size_t kGenerationDigits = 20U;

// The released version is a compile time fact as well as a rendered string, so
// the constants and the text can never drift apart.
static_assert(kVersionMajor == 1U && kVersionMinor == 0U && kVersionPatch == 0U,
              "the version constants are the 1.0.0 release");
static_assert(kStateFormatVersion == 1U, "the canonical state payload version is 1");
static_assert(kContainerFormatVersion == 1U, "the durable container version is 1");
static_assert(kStoreLayoutVersion == 1U, "the store directory layout version is 1");
static_assert(kReportFormatVersion == 1U, "the canonical report format version is 1");
static_assert(kContainerHeaderSize == 128U, "the container header is 128 bytes");
static_assert(kPointerRecordSize == 128U, "the CURRENT pointer record is 128 bytes");
static_assert(kRetainedGenerations == 4U, "four committed generations are retained");
static_assert(kContainerMagic.size() == 8U, "the container magic is eight bytes");
static_assert(kPointerMagic.size() == 8U, "the pointer magic is eight bytes");

[[nodiscard]] bool parse_component(std::string_view text, std::uint32_t& out) {
  if (text.empty() || text.size() > 9U) {
    return false;
  }
  if (text.size() > 1U && text.front() == '0') {
    return false;  // canonical: no leading zeros
  }
  std::uint32_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    value = value * 10U + static_cast<std::uint32_t>(character - '0');
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_version(std::string_view text, std::uint32_t& major, std::uint32_t& minor,
                                 std::uint32_t& patch) {
  const std::size_t first = text.find('.');
  if (first == std::string_view::npos) {
    return false;
  }
  const std::size_t second = text.find('.', first + 1U);
  if (second == std::string_view::npos) {
    return false;
  }
  if (text.find('.', second + 1U) != std::string_view::npos) {
    return false;
  }
  return parse_component(text.substr(0U, first), major) &&
         parse_component(text.substr(first + 1U, second - first - 1U), minor) &&
         parse_component(text.substr(second + 1U), patch);
}

[[nodiscard]] std::size_t count_occurrences(std::string_view text, char character) {
  std::size_t total = 0;
  for (const char value : text) {
    if (value == character) {
      ++total;
    }
  }
  return total;
}

/// The documented name of the generation file for one generation number:
/// "gen-" followed by twenty zero padded decimal digits and ".fdcdrain".
[[nodiscard]] std::string generation_file_name(std::uint64_t generation) {
  const std::string digits = format_strong(generation);
  FDC_REQUIRE(digits.size() <= kGenerationDigits);
  std::string name{kGenerationFilePrefix};
  name.append(kGenerationDigits - digits.size(), '0');
  name += digits;
  name += kGenerationFileSuffix;
  return name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Library version
// ---------------------------------------------------------------------------

FDC_TEST(version, version_string_is_the_release_text) {
  const std::string_view text{version_string()};
  FDC_CHECK_EQ(text, std::string_view{"1.0.0"});
  FDC_CHECK(!text.empty());
  FDC_CHECK(text.front() != 'v');
  FDC_CHECK_EQ(count_occurrences(text, '.'), 2U);
  FDC_CHECK_EQ(count_occurrences(text, '-'), 0U);
}

FDC_TEST(version, version_string_agrees_with_the_constants) {
  const std::string_view text{version_string()};
  std::uint32_t major = 0U;
  std::uint32_t minor = 0U;
  std::uint32_t patch = 0U;
  FDC_REQUIRE(parse_version(text, major, minor, patch));
  FDC_CHECK_EQ(major, kVersionMajor);
  FDC_CHECK_EQ(minor, kVersionMinor);
  FDC_CHECK_EQ(patch, kVersionPatch);
  FDC_CHECK_EQ(kVersionMajor, 1U);
  FDC_CHECK_EQ(kVersionMinor, 0U);
  FDC_CHECK_EQ(kVersionPatch, 0U);
}

// ---------------------------------------------------------------------------
// Format versions
// ---------------------------------------------------------------------------

FDC_TEST(version, format_versions_are_the_documented_values) {
  FDC_CHECK_EQ(kStateFormatVersion, 1U);
  FDC_CHECK_EQ(kContainerFormatVersion, 1U);
  FDC_CHECK_EQ(kStoreLayoutVersion, 1U);
  FDC_CHECK_EQ(kReportFormatVersion, 1U);
  // A reader accepts exactly the documented version, so the four are all the
  // first revision of their format.
  FDC_CHECK_EQ(kStateFormatVersion, kContainerFormatVersion);
  FDC_CHECK_EQ(kContainerFormatVersion, kStoreLayoutVersion);
  FDC_CHECK_EQ(kStoreLayoutVersion, kReportFormatVersion);
}

FDC_TEST(version, magic_strings_are_the_documented_magic) {
  FDC_CHECK_EQ(kContainerMagic, std::string_view{"FDCDRN01"});
  FDC_CHECK_EQ(kPointerMagic, std::string_view{"FDCDCUR1"});
  FDC_CHECK_EQ(kContainerMagic.size(), 8U);
  FDC_CHECK_EQ(kPointerMagic.size(), 8U);
  FDC_CHECK(kContainerMagic != kPointerMagic);

  // Both magics are exactly eight printable ASCII bytes, so a truncated or
  // byte swapped header cannot be mistaken for a valid one.
  for (const char character : kContainerMagic) {
    FDC_CHECK(character >= 0x21 && character <= 0x7E);
  }
  for (const char character : kPointerMagic) {
    FDC_CHECK(character >= 0x21 && character <= 0x7E);
  }
}

FDC_TEST(version, store_file_names_are_the_documented_names) {
  FDC_CHECK_EQ(kPointerFileName, std::string_view{"CURRENT"});
  FDC_CHECK_EQ(kWriterLockFileName, std::string_view{"writer.lock"});
  FDC_CHECK_EQ(kTransientFilePrefix, std::string_view{"tmp-"});
  FDC_CHECK_EQ(kGenerationFilePrefix, std::string_view{"gen-"});
  FDC_CHECK_EQ(kGenerationFileSuffix, std::string_view{".fdcdrain"});

  FDC_CHECK(kPointerFileName != kWriterLockFileName);
  FDC_CHECK(kPointerFileName.find('/') == std::string_view::npos);
  FDC_CHECK(kPointerFileName.find('\\') == std::string_view::npos);
  FDC_CHECK(kWriterLockFileName.find('/') == std::string_view::npos);
  FDC_CHECK(kWriterLockFileName.find('\\') == std::string_view::npos);
}

FDC_TEST(version, generation_file_names_and_the_retention_bound) {
  FDC_CHECK_EQ(kRetainedGenerations, 4U);
  FDC_CHECK_EQ(kContainerHeaderSize, 128U);
  FDC_CHECK_EQ(kPointerRecordSize, 128U);
  FDC_CHECK_EQ(kGenerationDigits, 20U);

  // "gen-" + twenty digits + ".fdcdrain" is thirty two bytes for every
  // generation number, which is what makes the names sortable as text.
  FDC_CHECK_EQ(generation_file_name(1ULL), std::string{"gen-00000000000000000001.fdcdrain"});
  FDC_CHECK_EQ(generation_file_name(42ULL), std::string{"gen-00000000000000000042.fdcdrain"});
  FDC_CHECK_EQ(generation_file_name(18446744073709551615ULL),
               std::string{"gen-18446744073709551615.fdcdrain"});

  constexpr std::size_t kExpectedLength = 4U + 20U + 9U;  // "gen-" + twenty digits + ".fdcdrain"
  FDC_CHECK_EQ(generation_file_name(1ULL).size(), kExpectedLength);
  FDC_CHECK_EQ(generation_file_name(18446744073709551615ULL).size(), kExpectedLength);
  FDC_CHECK_EQ(generation_file_name(7ULL).size(),
               kGenerationFilePrefix.size() + kGenerationDigits + kGenerationFileSuffix.size());

  const std::string name = generation_file_name(7ULL);
  FDC_CHECK(name.starts_with(kGenerationFilePrefix));
  FDC_CHECK(name.ends_with(kGenerationFileSuffix));
  FDC_CHECK(name.find('/') == std::string::npos);
  FDC_CHECK(name.find('\\') == std::string::npos);
  FDC_CHECK(name.find(' ') == std::string::npos);

  // The padded middle is exactly twenty decimal digits.
  const std::string_view middle{name.data() + kGenerationFilePrefix.size(), kGenerationDigits};
  FDC_CHECK_EQ(middle.size(), kGenerationDigits);
  for (const char character : middle) {
    FDC_CHECK(character >= '0' && character <= '9');
  }
  FDC_CHECK(middle.ends_with("000000000007"));

  // Older generations are pruned, so the number of retained files is bounded.
  FDC_CHECK(kRetainedGenerations > 0U);
  FDC_CHECK(kRetainedGenerations < 64U);
}
