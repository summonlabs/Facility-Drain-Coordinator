// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_TESTS_SUPPORT_TEST_HARNESS_HPP
#define FACILITYDRAIN_TESTS_SUPPORT_TEST_HARNESS_HPP

#include "facilitydrain/errors.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace fdc_test {

/// Thrown by FDC_REQUIRE to abandon the current test without abandoning the
/// run. The library itself never throws for control flow; this exception is
/// used only inside the harness.
class TestAbort {};

using TestFunction = void (*)();

/// Registers one test. Tests are grouped by suite so that a failure names the
/// area it belongs to.
struct Registrar {
  Registrar(const char* suite, const char* name, TestFunction function);
};

/// Registers a handler for `--child <mode> ...`, used by the tests that need a
/// real second operating system process.
void register_child_mode(std::string_view mode, std::function<int(const std::vector<std::string>&)> handler);

/// Runs the child handler registered for `mode`. Returns 64 when no handler is
/// registered.
int dispatch_child(const std::string& mode, const std::vector<std::string>& args);

/// Runs every registered test. `--filter <substring>` restricts the run to the
/// tests whose "suite.name" contains the substring; `--list` prints the names
/// instead of running them. Returns 0 only when every executed test passed.
int run_all(const std::vector<std::string>& arguments);

// -- reporting ---------------------------------------------------------------

/// Reports one failed check as "FAIL suite.name file:line message", so that a
/// failure stays attributable to a test even when it is raised from a helper.
void report_failure(const char* file, int line, const std::string& message);

/// True when the test that is currently running has reported a failure.
[[nodiscard]] bool current_test_has_failed() noexcept;

/// Evaluates a check condition through a function call rather than inline.
/// Real tests compare compile time constants, and a direct `if (a == b)` on
/// constants is diagnosed as a constant conditional expression by MSVC
/// (C4127); routing both this and `same` through a call keeps the check honest
/// without suppressing the warning.
template <class T>
[[nodiscard]] bool holds(T&& condition) {
  return static_cast<bool>(condition);
}

template <class Left, class Right>
[[nodiscard]] bool same(const Left& lhs, const Right& rhs) {
  return lhs == rhs;
}

/// The absolute path of the running test executable. Set once by test_main so
/// that a test can start a second copy of itself.
[[nodiscard]] const std::filesystem::path& executable_path();
void set_executable_path(const std::filesystem::path& path);

/// A unique, empty temporary directory for one test. Removed by `remove_tree`,
/// which the test is expected to call.
[[nodiscard]] std::filesystem::path make_temp_directory(std::string_view label);
void remove_tree(const std::filesystem::path& path) noexcept;

/// Renders a value for a diagnostic. Values with a to_text() render as that
/// text, error codes render as their stable token, and plain values render as
/// their numeric form.
std::string render(std::string_view value);
std::string render(const std::string& value);
std::string render(bool value);
std::string render(const char* value);
std::string render(const std::filesystem::path& value);
std::string render(facilitydrain::ErrorCode value);

template <class T>
std::string render(const T& value) {
  if constexpr (requires { value.to_text(); }) {
    return std::string{value.to_text()};
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_floating_point_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

}  // namespace fdc_test

#define FDC_TEST(suite_name, test_name)                                                \
  static void fdc_test_##suite_name##_##test_name();                                   \
  static const ::fdc_test::Registrar fdc_registrar_##suite_name##_##test_name{         \
      #suite_name, #test_name, &fdc_test_##suite_name##_##test_name};                  \
  static void fdc_test_##suite_name##_##test_name()

#define FDC_CHECK(condition)                                                           \
  do {                                                                                 \
    if (!::fdc_test::holds(condition)) {                                               \
      ::fdc_test::report_failure(__FILE__, __LINE__, "check failed: " #condition);     \
    }                                                                                  \
  } while (false)

#define FDC_CHECK_EQ(actual, expected)                                                 \
  do {                                                                                 \
    const auto fdc_actual = (actual);                                                  \
    const auto fdc_expected = (expected);                                              \
    if (!::fdc_test::same(fdc_actual, fdc_expected)) {                                 \
      ::fdc_test::report_failure(                                                      \
          __FILE__, __LINE__,                                                          \
          std::string{"expected "} + ::fdc_test::render(fdc_expected) +                \
              " but found " + ::fdc_test::render(fdc_actual) +                         \
              " (" #actual " == " #expected ")");                                      \
    }                                                                                  \
  } while (false)

#define FDC_CHECK_CODE(result, expected_code)                                          \
  do {                                                                                 \
    const auto& fdc_result = (result);                                                 \
    if (fdc_result.has_value()) {                                                      \
      ::fdc_test::report_failure(__FILE__, __LINE__,                                   \
                                 "expected " #expected_code                            \
                                 " but the operation succeeded");                      \
    } else if (fdc_result.error().code() != (expected_code)) {                         \
      ::fdc_test::report_failure(                                                      \
          __FILE__, __LINE__,                                                          \
          std::string{"expected " #expected_code " but found "} +                      \
              std::string{::facilitydrain::to_token(fdc_result.error().code())} +      \
              ": " + fdc_result.error().detail());                                     \
    }                                                                                  \
  } while (false)

#define FDC_CHECK_STATUS(status, expected_code)                                        \
  do {                                                                                 \
    const auto& fdc_status = (status);                                                 \
    if (fdc_status.ok()) {                                                             \
      ::fdc_test::report_failure(__FILE__, __LINE__,                                   \
                                 "expected " #expected_code                            \
                                 " but the operation succeeded");                      \
    } else if (fdc_status.code() != (expected_code)) {                                 \
      ::fdc_test::report_failure(                                                      \
          __FILE__, __LINE__,                                                          \
          std::string{"expected " #expected_code " but found "} +                      \
              std::string{::facilitydrain::to_token(fdc_status.code())} +              \
              ": " + fdc_status.error().detail());                                     \
    }                                                                                  \
  } while (false)

#define FDC_REQUIRE(condition)                                                         \
  do {                                                                                 \
    if (!::fdc_test::holds(condition)) {                                               \
      ::fdc_test::report_failure(__FILE__, __LINE__,                                   \
                                 "requirement failed: " #condition);                   \
      throw ::fdc_test::TestAbort{};                                                   \
    }                                                                                  \
  } while (false)

#define FDC_REQUIRE_OK(result)                                                         \
  do {                                                                                 \
    const auto& fdc_result = (result);                                                 \
    if (!fdc_result.has_value()) {                                                     \
      ::fdc_test::report_failure(                                                      \
          __FILE__, __LINE__,                                                          \
          std::string{"expected success from " #result " but found "} +                \
              std::string{::facilitydrain::to_token(fdc_result.error().code())} +      \
              ": " + fdc_result.error().detail());                                     \
      throw ::fdc_test::TestAbort{};                                                   \
    }                                                                                  \
  } while (false)

#define FDC_REQUIRE_CODE(result, expected_code)                                        \
  do {                                                                                 \
    const auto& fdc_result = (result);                                                 \
    if (fdc_result.has_value()) {                                                      \
      ::fdc_test::report_failure(__FILE__, __LINE__,                                   \
                                 "expected failure " #expected_code " from " #result   \
                                 " but the operation succeeded");                      \
      throw ::fdc_test::TestAbort{};                                                   \
    }                                                                                  \
    if (fdc_result.error().code() != (expected_code)) {                                \
      ::fdc_test::report_failure(                                                      \
          __FILE__, __LINE__,                                                          \
          std::string{"expected failure " #expected_code " from " #result              \
                      " but found "} +                                                 \
              std::string{::facilitydrain::to_token(fdc_result.error().code())} +      \
              ": " + fdc_result.error().detail());                                     \
      throw ::fdc_test::TestAbort{};                                                   \
    }                                                                                  \
  } while (false)

#endif  // FACILITYDRAIN_TESTS_SUPPORT_TEST_HARNESS_HPP
