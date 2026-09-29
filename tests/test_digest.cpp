// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

using facilitydrain::ContentDigest;
using facilitydrain::combine_digests;
using facilitydrain::digest_bytes;
using facilitydrain::digest_text;

constexpr std::size_t kHexDigits = ContentDigest::kSize * 2U;

// The SHA-256 known answers below are the published NIST vectors. They are the
// only external facts in this file: everything else is a property of the
// construction.
constexpr std::string_view kSha256OfEmpty{
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"};
constexpr std::string_view kSha256OfAbc{
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
constexpr std::string_view kSha256OfTwoBlockMessage{
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"};
constexpr std::string_view kSha256OfOneBlockOfA{
    "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"};
constexpr std::string_view kSha256OfMillionA{
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"};

[[nodiscard]] ContentDigest digest_of_hex(std::string_view hex) {
  const std::optional<ContentDigest> parsed = ContentDigest::from_hex(hex);
  FDC_REQUIRE(parsed.has_value());
  return *parsed;
}

[[nodiscard]] bool is_lowercase_hex(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char character : text) {
    const bool digit = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    if (!digit && !lower) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string upper_case(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    result.push_back(character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A') : character);
  }
  return result;
}

[[nodiscard]] ContentDigest digest_of_text_bytes(std::string_view text) {
  const std::span<const char> characters(text.data(), text.size());
  return digest_bytes(std::as_bytes(characters));
}

}  // namespace

// ---------------------------------------------------------------------------
// SHA-256 known answers
// ---------------------------------------------------------------------------

FDC_TEST(digest, sha256_known_answers) {
  FDC_CHECK_EQ(digest_text(std::string_view{}).to_hex(), std::string{kSha256OfEmpty});
  FDC_CHECK_EQ(digest_text(std::string_view{"abc"}).to_hex(), std::string{kSha256OfAbc});

  // A fifty six byte message: one full block plus the start of the padded one.
  const std::string two_blocks{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
  FDC_CHECK_EQ(two_blocks.size(), 56U);
  FDC_CHECK_EQ(digest_text(two_blocks).to_hex(), std::string{kSha256OfTwoBlockMessage});

  // Exactly one block of input, which forces the padding into a second block.
  const std::string one_block(64U, 'a');
  FDC_CHECK_EQ(digest_text(one_block).to_hex(), std::string{kSha256OfOneBlockOfA});

  // The million character vector: many blocks, so the length accumulator and
  // the block loop are both exercised.
  const std::string million(1000000U, 'a');
  FDC_CHECK_EQ(digest_text(million).to_hex(), std::string{kSha256OfMillionA});

  // Every rendering is exactly the canonical width.
  FDC_CHECK_EQ(digest_text(std::string_view{"abc"}).to_hex().size(), kHexDigits);
  FDC_CHECK(is_lowercase_hex(digest_text(std::string_view{"abc"}).to_hex()));
}

FDC_TEST(digest, digest_bytes_equals_digest_text) {
  constexpr std::array<std::string_view, 5> kTexts{
      "", "abc", "hello world", "drain the rack", "caf\xC3\xA9 \xE2\x9C\x93"};
  for (const std::string_view text : kTexts) {
    FDC_CHECK(digest_of_text_bytes(text) == digest_text(text));
    FDC_CHECK_EQ(digest_of_text_bytes(text).to_hex(), digest_text(text).to_hex());
  }

  // The same three bytes however they are spelled.
  const std::array<std::byte, 3> raw{std::byte{0x61}, std::byte{0x62}, std::byte{0x63}};
  FDC_CHECK(digest_bytes(raw) == digest_text(std::string_view{"abc"}));
  FDC_CHECK(digest_bytes(std::span<const std::byte>{}) == digest_text(std::string_view{}));
}

// ---------------------------------------------------------------------------
// Text form
// ---------------------------------------------------------------------------

FDC_TEST(digest, hex_round_trip_and_strictness) {
  const ContentDigest digest = digest_text(std::string_view{"round trip"});
  const std::string hex = digest.to_hex();
  FDC_CHECK_EQ(hex.size(), kHexDigits);
  FDC_CHECK(is_lowercase_hex(hex));

  const std::optional<ContentDigest> round_tripped = ContentDigest::from_hex(hex);
  FDC_REQUIRE(round_tripped.has_value());
  FDC_CHECK(*round_tripped == digest);
  FDC_CHECK_EQ(round_tripped->to_hex(), hex);

  // Exactly one spelling parses: uppercase, prefixes, separators, and short or
  // long forms are all refused rather than repaired.
  FDC_CHECK(!ContentDigest::from_hex(std::string_view{}).has_value());
  FDC_CHECK(!ContentDigest::from_hex(hex.substr(0U, kHexDigits - 1U)).has_value());
  FDC_CHECK(!ContentDigest::from_hex(hex + "0").has_value());
  FDC_CHECK(!ContentDigest::from_hex("0x" + hex).has_value());
  FDC_CHECK(!ContentDigest::from_hex(upper_case(hex)).has_value());
  FDC_CHECK(!ContentDigest::from_hex(std::string(63U, '0') + "-").has_value());
  FDC_CHECK(!ContentDigest::from_hex(std::string(64U, 'g')).has_value());
  FDC_CHECK(!ContentDigest::from_hex(std::string(64U, ' ')).has_value());

  // Only the first character upper case is still not canonical.
  std::string mixed = hex;
  mixed[0] = static_cast<char>(mixed[0] - 'a' + 'A');
  FDC_CHECK(!ContentDigest::from_hex(mixed).has_value());

  // A zero digest has exactly one spelling too.
  const std::optional<ContentDigest> zeros = ContentDigest::from_hex(std::string(64U, '0'));
  FDC_REQUIRE(zeros.has_value());
  FDC_CHECK(zeros->is_zero());
  FDC_CHECK_EQ(zeros->to_hex(), std::string(64U, '0'));
}

FDC_TEST(digest, is_zero_only_for_the_default_value) {
  const ContentDigest none{};
  FDC_CHECK(none.is_zero());
  FDC_CHECK(none == ContentDigest{});
  FDC_CHECK_EQ(none.to_hex(), std::string(64U, '0'));

  // No hash output is ever the zero digest.
  FDC_CHECK(!digest_text(std::string_view{}).is_zero());
  FDC_CHECK(!digest_text(std::string_view{"a"}).is_zero());
  FDC_CHECK(!digest_text(std::string_view{"abc"}).is_zero());

  FDC_CHECK(!digest_of_hex(std::string(63U, '0') + "1").is_zero());
  FDC_CHECK(!digest_of_hex("1" + std::string(63U, '0')).is_zero());
  FDC_CHECK(!digest_of_hex(std::string(64U, 'f')).is_zero());
}

FDC_TEST(digest, from_bytes_matches_the_hex_form) {
  std::array<std::byte, ContentDigest::kSize> raw{};
  for (std::size_t index = 0; index < raw.size(); ++index) {
    raw[index] = static_cast<std::byte>(static_cast<std::uint8_t>(index));
  }
  const ContentDigest built = ContentDigest::from_bytes(std::span<const std::byte, ContentDigest::kSize>{raw});
  FDC_CHECK_EQ(built.to_hex(),
               std::string{"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"});
  FDC_CHECK(!built.is_zero());
  FDC_CHECK_EQ(built.bytes().size(), ContentDigest::kSize);
  FDC_CHECK(built.bytes()[0] == std::byte{0x00});
  FDC_CHECK(built.bytes()[ContentDigest::kSize - 1U] == std::byte{0x1F});
  FDC_CHECK(built == digest_of_hex(built.to_hex()));

  std::array<std::byte, ContentDigest::kSize> zeros{};
  FDC_CHECK(ContentDigest::from_bytes(std::span<const std::byte, ContentDigest::kSize>{zeros}).is_zero());
}

// ---------------------------------------------------------------------------
// Domain separated combination
// ---------------------------------------------------------------------------

FDC_TEST(digest, combine_is_order_sensitive) {
  const ContentDigest first = digest_text(std::string_view{"scope"});
  const ContentDigest second = digest_text(std::string_view{"policy"});
  const std::array<ContentDigest, 2> forward{first, second};
  const std::array<ContentDigest, 2> backward{second, first};

  FDC_CHECK(combine_digests("evidence", forward) != combine_digests("evidence", backward));
  // The same sequence always produces the same digest.
  FDC_CHECK(combine_digests("evidence", forward) == combine_digests("evidence", forward));
  FDC_CHECK(combine_digests("evidence", backward) == combine_digests("evidence", backward));
  // Swapping two equal entries is still the same sequence.
  const std::array<ContentDigest, 2> repeated{first, first};
  FDC_CHECK(combine_digests("evidence", repeated) == combine_digests("evidence", repeated));
  FDC_CHECK(!combine_digests("evidence", forward).is_zero());
}

FDC_TEST(digest, combine_is_length_sensitive) {
  const ContentDigest whole = digest_text(std::string_view{"ab"});
  const ContentDigest part_a = digest_text(std::string_view{"a"});
  const ContentDigest part_b = digest_text(std::string_view{"b"});

  const std::array<ContentDigest, 1> one{whole};
  const std::array<ContentDigest, 2> two{part_a, part_b};
  const std::array<ContentDigest, 3> three{part_a, part_b, part_a};
  const std::span<const ContentDigest> empty_sequence{};

  // The count is hashed, so two entries can never collide with the single
  // digest of their concatenation, and a sequence of one is not the entry.
  FDC_CHECK(combine_digests("entries", one) != combine_digests("entries", two));
  FDC_CHECK(combine_digests("entries", one) != whole);
  FDC_CHECK(combine_digests("entries", two) != combine_digests("entries", three));
  FDC_CHECK(combine_digests("entries", empty_sequence) != combine_digests("entries", one));
  FDC_CHECK(combine_digests("entries", empty_sequence) != combine_digests("entries", two));

  // An empty sequence still has a digest: absence of entries is a statement.
  FDC_CHECK(!combine_digests("entries", empty_sequence).is_zero());
}

FDC_TEST(digest, combine_is_domain_sensitive) {
  const std::array<ContentDigest, 2> entries{digest_text(std::string_view{"scope"}),
                                             digest_text(std::string_view{"policy"})};

  FDC_CHECK(combine_digests("enumeration", entries) != combine_digests("completion", entries));
  FDC_CHECK(combine_digests("enumeration", entries) == combine_digests("enumeration", entries));
  // Even an empty tag is a distinct tag.
  FDC_CHECK(combine_digests("", entries) != combine_digests("enumeration", entries));
  FDC_CHECK(!combine_digests("", entries).is_zero());

  // The separator byte cannot be smuggled into a tag to forge another tag.
  std::string forged{"enumeration"};
  forged.push_back('\x1F');
  forged += "completion";
  FDC_CHECK(combine_digests(forged, entries) != combine_digests("completion", entries));
  FDC_CHECK(combine_digests(forged, entries) != combine_digests("enumeration", entries));
}

// ---------------------------------------------------------------------------
// Value semantics
// ---------------------------------------------------------------------------

FDC_TEST(digest, equality_and_ordering) {
  const ContentDigest zero = digest_of_hex(std::string(64U, '0'));
  const ContentDigest one = digest_of_hex(std::string(63U, '0') + "1");
  const ContentDigest big = digest_of_hex("f" + std::string(63U, '0'));

  FDC_CHECK(zero == zero);
  FDC_CHECK(zero != one);
  // ContentDigest deliberately offers only ==, != and <, which is all a strict
  // weak order needs for ordered containers and maps.
  FDC_CHECK(zero < one);
  FDC_CHECK(one < big);
  FDC_CHECK(!(big < one));
  FDC_CHECK(!(zero < zero));
  FDC_CHECK(ContentDigest{} == zero);
}

FDC_TEST(digest, hash_works_in_unordered_containers) {
  const ContentDigest first = digest_text(std::string_view{"first"});
  const ContentDigest second = digest_text(std::string_view{"second"});
  const ContentDigest first_again = digest_text(std::string_view{"first"});

  FDC_CHECK_EQ(std::hash<ContentDigest>{}(first), std::hash<ContentDigest>{}(first_again));
  FDC_CHECK(std::hash<ContentDigest>{}(first) != std::hash<ContentDigest>{}(second));

  std::unordered_set<ContentDigest> digests;
  digests.insert(first);
  digests.insert(second);
  digests.insert(first_again);
  FDC_CHECK_EQ(digests.size(), 2U);
  FDC_CHECK(digests.count(first) == 1U);
  FDC_CHECK(digests.count(ContentDigest{}) == 0U);

  std::unordered_map<ContentDigest, std::string> by_digest;
  by_digest.emplace(first, std::string{"first"});
  by_digest.emplace(second, std::string{"second"});
  by_digest[first_again] = std::string{"updated"};
  FDC_CHECK_EQ(by_digest.size(), 2U);
  FDC_CHECK_EQ(by_digest.at(first), std::string{"updated"});
  FDC_CHECK_EQ(by_digest.at(second), std::string{"second"});
}
