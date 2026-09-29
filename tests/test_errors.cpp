// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include "facilitydrain/facility_drain_coordinator.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using facilitydrain::Error;
using facilitydrain::ErrorCode;
using facilitydrain::Result;
using facilitydrain::Status;
using facilitydrain::error_code_from_token;
using facilitydrain::is_authority_error;
using facilitydrain::is_lifecycle_error;
using facilitydrain::is_limit_error;
using facilitydrain::is_store_error;
using facilitydrain::is_validation_error;
using facilitydrain::make_error;
using facilitydrain::to_token;

struct CodeRange {
  std::uint32_t low;
  std::uint32_t high;
};

// The numeric ranges documented in errors.hpp. Every value inside every range
// is walked in full, so a code added anywhere inside a range is exercised
// without this file having to name it.
constexpr std::array<CodeRange, 7> kDocumentedRanges{{{0U, 0U},
                                                      {1U, 99U},
                                                      {100U, 199U},
                                                      {200U, 299U},
                                                      {300U, 399U},
                                                      {400U, 499U},
                                                      {900U, 999U}}};

constexpr std::string_view kUnknownToken{"unknown"};

// A canonical error token is lowercase, uses '-' between words and never starts
// or ends with a separator.
[[nodiscard]] bool is_canonical_token(std::string_view token) {
  if (token.empty()) {
    return false;
  }
  for (const char character : token) {
    const bool lowercase_letter = character >= 'a' && character <= 'z';
    const bool digit = character >= '0' && character <= '9';
    if (!lowercase_letter && !digit && character != '-') {
      return false;
    }
  }
  return token.front() != '-' && token.back() != '-';
}

[[nodiscard]] std::uint32_t classification_count(ErrorCode code) {
  std::uint32_t total = 0;
  if (is_validation_error(code)) {
    ++total;
  }
  if (is_limit_error(code)) {
    ++total;
  }
  if (is_authority_error(code)) {
    ++total;
  }
  if (is_lifecycle_error(code)) {
    ++total;
  }
  if (is_store_error(code)) {
    ++total;
  }
  return total;
}

/// A value type that cannot be default constructed, so a failure shaped Result
/// provably does not need one.
struct Payload {
  explicit Payload(int raw) noexcept : value(raw) {}
  int value = 0;
};

[[nodiscard]] Result<int> parse_positive(int value) {
  if (value <= 0) {
    return make_error<int>(ErrorCode::kInvalidArgument, "value must be positive");
  }
  return value;
}

// A Result is always constructed with a value or with an error: there is no
// path by which a missing answer becomes a value, a zero or a healthy verdict.
static_assert(!std::is_default_constructible_v<Result<int>>,
              "a Result must be constructed from a value or from an Error");

}  // namespace

// ---------------------------------------------------------------------------
// Codes and tokens
// ---------------------------------------------------------------------------

FDC_TEST(errors, token_round_trip_over_every_documented_code) {
  std::uint32_t defined = 0;
  std::uint32_t undefined = 0;
  for (const CodeRange& range : kDocumentedRanges) {
    std::uint32_t expected_next = range.low;
    for (std::uint32_t value = range.low; value <= range.high; ++value) {
      const ErrorCode code = static_cast<ErrorCode>(value);
      const std::string_view token = to_token(code);
      FDC_CHECK(!token.empty());
      if (token == kUnknownToken) {
        ++undefined;
        continue;
      }
      ++defined;
      // Codes inside a documented range are contiguous from its start, so a
      // code inserted in the middle of a range is noticed here rather than
      // being silently skipped by this walk.
      FDC_CHECK_EQ(value, expected_next);
      ++expected_next;
      FDC_CHECK(is_canonical_token(token));

      const std::optional<ErrorCode> round_tripped = error_code_from_token(token);
      FDC_CHECK(round_tripped.has_value());
      const std::uint32_t numeric =
          round_tripped.has_value() ? static_cast<std::uint32_t>(*round_tripped) : 0xFFFFFFFFU;
      FDC_CHECK_EQ(numeric, value);
    }
  }
  // Every enumerator the header documents, counted so that a deletion is a
  // failure and an addition is not.
  FDC_CHECK(defined >= 64U);
  FDC_CHECK(undefined > 0U);
}

FDC_TEST(errors, classification_helpers_match_the_numeric_ranges) {
  for (std::uint32_t value = 0; value <= 1000U; ++value) {
    const ErrorCode code = static_cast<ErrorCode>(value);
    FDC_CHECK_EQ(is_validation_error(code), value >= 1U && value <= 99U);
    FDC_CHECK_EQ(is_limit_error(code), value >= 100U && value <= 199U);
    FDC_CHECK_EQ(is_authority_error(code), value >= 200U && value <= 299U);
    FDC_CHECK_EQ(is_lifecycle_error(code), value >= 300U && value <= 399U);
    FDC_CHECK_EQ(is_store_error(code), value >= 400U && value <= 499U);
    // The ranges are disjoint: a code is in at most one class.
    FDC_CHECK(classification_count(code) <= 1U);
  }

  // Every real failure code except the two that belong to no range (kOk and
  // kInternal) is classified exactly once.
  for (const CodeRange& range : kDocumentedRanges) {
    for (std::uint32_t value = range.low; value <= range.high; ++value) {
      if (value == 0U || value == 900U) {
        continue;
      }
      const ErrorCode code = static_cast<ErrorCode>(value);
      if (to_token(code) == kUnknownToken) {
        continue;
      }
      FDC_CHECK_EQ(classification_count(code), 1U);
    }
  }

  // The boundary values are exactly where the ranges say they are.
  FDC_CHECK(is_validation_error(ErrorCode::kInvalidArgument));
  FDC_CHECK(is_validation_error(static_cast<ErrorCode>(99U)));
  FDC_CHECK(!is_validation_error(static_cast<ErrorCode>(100U)));
  FDC_CHECK(is_limit_error(static_cast<ErrorCode>(199U)));
  FDC_CHECK(!is_limit_error(static_cast<ErrorCode>(200U)));
  FDC_CHECK(is_authority_error(ErrorCode::kPlanNotFound));
  FDC_CHECK(is_lifecycle_error(ErrorCode::kPlanTerminal));
  FDC_CHECK(is_store_error(ErrorCode::kStoreLocked));
  FDC_CHECK(!is_store_error(ErrorCode::kInternal));
  FDC_CHECK(!is_validation_error(ErrorCode::kOk));
  FDC_CHECK(!is_store_error(ErrorCode::kOk));
}

FDC_TEST(errors, unknown_values_have_no_token) {
  FDC_CHECK_EQ(to_token(static_cast<ErrorCode>(15U)), kUnknownToken);
  FDC_CHECK_EQ(to_token(static_cast<ErrorCode>(500U)), kUnknownToken);
  FDC_CHECK_EQ(to_token(static_cast<ErrorCode>(899U)), kUnknownToken);
  FDC_CHECK_EQ(to_token(static_cast<ErrorCode>(1000U)), kUnknownToken);
  FDC_CHECK_EQ(to_token(static_cast<ErrorCode>(0xFFFFFFFFU)), kUnknownToken);

  // "unknown" is not a code, and neither is any near miss of a real token.
  constexpr std::array<std::string_view, 10> kRejected{
      "unknown", "", "OK", " ok", "ok ", "kOk", "store_locked", "Store-Locked", "store-locked\n",
      "invalid-argument "};
  for (const std::string_view text : kRejected) {
    FDC_CHECK(!error_code_from_token(text).has_value());
  }
}

FDC_TEST(errors, documented_tokens_are_the_expected_spellings) {
  FDC_CHECK_EQ(to_token(ErrorCode::kOk), std::string_view{"ok"});
  FDC_CHECK_EQ(to_token(ErrorCode::kInvalidArgument), std::string_view{"invalid-argument"});
  FDC_CHECK_EQ(to_token(ErrorCode::kMissingRequiredField), std::string_view{"missing-required-field"});
  FDC_CHECK_EQ(to_token(ErrorCode::kInvalidGenerationOrder), std::string_view{"invalid-generation-order"});
  FDC_CHECK_EQ(to_token(ErrorCode::kLimitExceeded), std::string_view{"limit-exceeded"});
  FDC_CHECK_EQ(to_token(ErrorCode::kCounterExhausted), std::string_view{"counter-exhausted"});
  FDC_CHECK_EQ(to_token(ErrorCode::kConsumerDigestMismatch), std::string_view{"consumer-digest-mismatch"});
  FDC_CHECK_EQ(to_token(ErrorCode::kAcknowledgementIsNotEffect),
               std::string_view{"acknowledgement-is-not-effect"});
  FDC_CHECK_EQ(to_token(ErrorCode::kStoreRecoveryFailed), std::string_view{"store-recovery-failed"});
  FDC_CHECK_EQ(to_token(ErrorCode::kInternal), std::string_view{"internal"});

  const std::optional<ErrorCode> locked = error_code_from_token("store-locked");
  FDC_CHECK(locked.has_value());
  FDC_CHECK_EQ(locked.has_value() ? static_cast<std::uint32_t>(*locked) : 0xFFFFFFFFU,
               static_cast<std::uint32_t>(ErrorCode::kStoreLocked));
  const std::optional<ErrorCode> ok = error_code_from_token("ok");
  FDC_CHECK(ok.has_value());
  FDC_CHECK(ok.has_value() && *ok == ErrorCode::kOk);
}

// ---------------------------------------------------------------------------
// Error, Status and Result
// ---------------------------------------------------------------------------

FDC_TEST(errors, error_to_text_shapes) {
  const Error none{};
  FDC_CHECK(none.ok());
  FDC_CHECK_EQ(none.code(), ErrorCode::kOk);
  FDC_CHECK(none.detail().empty());
  FDC_CHECK_EQ(none.to_text(), std::string{"ok"});

  // kOk renders as "ok" whatever diagnostic text happens to be attached.
  const Error still_ok{ErrorCode::kOk, std::string_view{"ignored"}};
  FDC_CHECK(still_ok.ok());
  FDC_CHECK_EQ(still_ok.to_text(), std::string{"ok"});

  // A code with no detail is the bare token.
  const Error bare{ErrorCode::kStoreLocked, std::string_view{}};
  FDC_CHECK(!bare.ok());
  FDC_CHECK_EQ(bare.code(), ErrorCode::kStoreLocked);
  FDC_CHECK_EQ(bare.to_text(), std::string{"store-locked"});

  // A code with detail is "token: detail".
  const Error detailed{ErrorCode::kStoreLocked, std::string_view{"another writer holds the lock"}};
  FDC_CHECK_EQ(detailed.to_text(), std::string{"store-locked: another writer holds the lock"});
  FDC_CHECK_EQ(detailed.detail(), std::string{"another writer holds the lock"});

  const Error made = Error::make(ErrorCode::kInvalidScope, std::string_view{"scope 0 does not exist"});
  FDC_CHECK_EQ(made.code(), ErrorCode::kInvalidScope);
  FDC_CHECK_EQ(made.to_text(), std::string{"invalid-scope: scope 0 does not exist"});
  FDC_CHECK_EQ(made.detail(), std::string{"scope 0 does not exist"});
}

FDC_TEST(errors, status_semantics) {
  const Status success{};
  FDC_CHECK(success.ok());
  FDC_CHECK_EQ(success.code(), ErrorCode::kOk);
  FDC_CHECK_EQ(success.to_text(), std::string{"ok"});
  FDC_CHECK(Status::success().ok());

  const Status failure = Status::failure(ErrorCode::kTooManyEntries, std::string_view{"too many"});
  FDC_CHECK(!failure.ok());
  FDC_CHECK_EQ(failure.code(), ErrorCode::kTooManyEntries);
  FDC_CHECK_EQ(failure.error().detail(), std::string{"too many"});
  FDC_CHECK_EQ(failure.to_text(), std::string{"too-many-entries: too many"});

  const Status from_error{Error{ErrorCode::kStoreIoError, std::string_view{"read failed"}}};
  FDC_CHECK(!from_error.ok());
  FDC_CHECK_EQ(from_error.code(), ErrorCode::kStoreIoError);
  FDC_CHECK_EQ(from_error.error().detail(), std::string{"read failed"});
}

FDC_TEST(errors, result_semantics) {
  const Result<int> value_result{7};
  FDC_CHECK(value_result.has_value());
  FDC_CHECK(static_cast<bool>(value_result));
  FDC_CHECK_EQ(value_result.value(), 7);
  FDC_CHECK_EQ(*value_result, 7);

  const Result<std::string> text_result{std::string{"drained"}};
  FDC_CHECK(text_result.has_value());
  FDC_CHECK_EQ(text_result.value(), std::string{"drained"});
  FDC_CHECK_EQ(text_result->size(), 7U);

  const Result<Payload> payload_result{Payload{5}};
  FDC_REQUIRE(payload_result.has_value());
  FDC_CHECK_EQ(payload_result.value().value, 5);
  FDC_CHECK_EQ(payload_result->value, 5);
  FDC_CHECK_EQ((*payload_result).value, 5);

  // A Result that holds no value is failure shaped even when the code it was
  // built from is the zero code: nothing can be read from it as a value.
  const Result<Payload> shapeless{Error{}};
  FDC_CHECK(!shapeless.has_value());
  FDC_CHECK(!static_cast<bool>(shapeless));
  FDC_CHECK(shapeless.error().ok());
}

FDC_TEST(errors, make_error_and_error_propagation) {
  const Result<int> built = make_error<int>(ErrorCode::kRevisionConflict, std::string_view{"stale revision"});
  FDC_CHECK(!built.has_value());
  FDC_CHECK_EQ(built.error().code(), ErrorCode::kRevisionConflict);
  FDC_CHECK_EQ(built.error().detail(), std::string{"stale revision"});
  FDC_CHECK_EQ(built.error().to_text(), std::string{"revision-conflict: stale revision"});

  // The error travels out of the operation unchanged, including its detail.
  FDC_CHECK_CODE(parse_positive(0), ErrorCode::kInvalidArgument);

  const Result<int> rejected = parse_positive(-3);
  FDC_CHECK(!rejected.has_value());
  FDC_CHECK_EQ(rejected.error().detail(), std::string{"value must be positive"});
  // value() would dereference an empty optional here, so the guard is the only
  // thing a caller may touch on a failure shape.
  FDC_CHECK(!rejected.has_value());
  FDC_CHECK(!static_cast<bool>(rejected));

  const Result<int> accepted = parse_positive(11);
  FDC_REQUIRE_OK(accepted);
  FDC_CHECK_EQ(accepted.value(), 11);
  FDC_CHECK(accepted.error().ok());

  const Result<int> copied = accepted;
  FDC_CHECK(copied.has_value());
  FDC_CHECK_EQ(copied.value(), 11);
}
