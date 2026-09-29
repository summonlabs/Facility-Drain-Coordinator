// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <utility>

namespace fdc_test {
namespace {

/// The name used when a failure is reported outside any test, which is what
/// happens inside a child mode: there is no test to attribute it to, but the
/// diagnostic keeps the same shape so a log reader never has to guess.
constexpr std::string_view kNoTest{"(no-test)"};

struct Entry {
  std::string suite;
  std::string name;
  TestFunction function;
};

std::vector<Entry>& registry() {
  static std::vector<Entry> entries;
  return entries;
}

std::map<std::string, std::function<int(const std::vector<std::string>&)>, std::less<>>& child_modes() {
  static std::map<std::string, std::function<int(const std::vector<std::string>&)>, std::less<>> modes;
  return modes;
}

bool g_current_failed = false;
std::string g_current_test_name;
std::filesystem::path g_executable_path;
std::atomic<std::uint64_t> g_temp_counter{0};

/// A path rendered for a diagnostic. The narrow conversion can fail for a path
/// the active code page cannot represent, and a diagnostic must never be the
/// thing that crashes the harness.
std::string display_path(const std::filesystem::path& path) {
  try {
    return path.string();
  } catch (const std::exception&) {
    return "<path>";
  }
}

/// A token that is unique inside this process, and practically unique between
/// concurrent processes as well, without relying on randomness.
std::string unique_token() {
  const std::uint64_t counter = g_temp_counter.fetch_add(1) + 1;
  const auto ticks = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  static constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (int shift = 60; shift >= 0; shift -= 4) {
    result.push_back(digits[(ticks >> shift) & 0x0Fu]);
  }
  result.push_back('-');
  result.append(std::to_string(counter));
  return result;
}

/// A label becomes part of a directory name, so anything that could change
/// which directory is meant is folded away rather than trusted.
std::string safe_label(std::string_view label) {
  std::string result;
  for (const char character : label) {
    const bool plain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '-' || character == '_';
    result.push_back(plain ? character : '_');
  }
  return result;
}

}  // namespace

Registrar::Registrar(const char* suite, const char* name, TestFunction function) {
  registry().push_back(Entry{suite, name, function});
}

void register_child_mode(std::string_view mode, std::function<int(const std::vector<std::string>&)> handler) {
  child_modes().insert_or_assign(std::string{mode}, std::move(handler));
}

int dispatch_child(const std::string& mode, const std::vector<std::string>& args) {
  const auto position = child_modes().find(mode);
  if (position == child_modes().end()) {
    std::cerr << "fdc_tests: no child mode named " << mode << "\n";
    return 64;
  }
  try {
    return position->second(args);
  } catch (const std::exception& error) {
    std::cerr << "fdc_tests: child mode " << mode << " raised: " << error.what() << "\n";
    return 70;
  } catch (...) {
    std::cerr << "fdc_tests: child mode " << mode << " raised a non-standard exception\n";
    return 70;
  }
}

void report_failure(const char* file, int line, const std::string& message) {
  g_current_failed = true;
  std::cout << "FAIL " << (g_current_test_name.empty() ? std::string{kNoTest} : g_current_test_name) << " " << file
            << ":" << line << " " << message << std::endl;
}

bool current_test_has_failed() noexcept { return g_current_failed; }

const std::filesystem::path& executable_path() { return g_executable_path; }

void set_executable_path(const std::filesystem::path& path) { g_executable_path = path; }

std::filesystem::path make_temp_directory(std::string_view label) {
  std::error_code error;
  const std::filesystem::path base = std::filesystem::temp_directory_path(error);
  const std::filesystem::path root = error ? std::filesystem::path{"."} : base;
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::filesystem::path directory =
        root / ("fdc-test-" + safe_label(label) + "-" + unique_token());
    error.clear();
    if (std::filesystem::create_directories(directory, error)) {
      return directory;
    }
    if (error) {
      report_failure(__FILE__, __LINE__,
                     "could not create the temporary directory " + display_path(directory) + ": " +
                         error.message());
      return directory;
    }
    // The name already existed, so a fresh one is tried rather than a directory
    // whose contents are not known to be empty being reused.
  }
  report_failure(__FILE__, __LINE__,
                 "could not create a unique temporary directory under " + display_path(root));
  return root;
}

void remove_tree(const std::filesystem::path& path) noexcept {
  try {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
      // A temporary directory that survives its test is a defect in the test,
      // not a detail: it usually means a file inside it was still open.
      // Reporting it here turns a silent leak into a failed test.
      report_failure(__FILE__, __LINE__, "could not remove the temporary directory " + display_path(path) + ": " +
                                             error.message());
      return;
    }
    error.clear();
    if (std::filesystem::exists(path, error) && !error) {
      report_failure(__FILE__, __LINE__,
                     "the temporary directory " + display_path(path) + " still exists after removal");
    }
  } catch (const std::exception& error) {
    report_failure(__FILE__, __LINE__,
                   "could not remove the temporary directory " + display_path(path) + ": " + error.what());
  }
}

std::string render(std::string_view value) { return std::string{value}; }
std::string render(const std::string& value) { return value; }
std::string render(bool value) { return value ? "true" : "false"; }
std::string render(const char* value) { return value == nullptr ? "<null>" : std::string{value}; }
std::string render(const std::filesystem::path& value) { return display_path(value); }
std::string render(facilitydrain::ErrorCode value) { return std::string{facilitydrain::to_token(value)}; }

int run_all(const std::vector<std::string>& arguments) {
  std::string filter;
  bool list_only = false;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--filter" && index + 1 < arguments.size()) {
      filter = arguments[index + 1];
      ++index;
    } else if (arguments[index] == "--list") {
      list_only = true;
    }
  }

  std::vector<Entry> entries = registry();
  // A deterministic order that does not depend on link order.
  std::sort(entries.begin(), entries.end(), [](const Entry& lhs, const Entry& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  std::size_t executed = 0;
  std::size_t failed = 0;
  for (const Entry& entry : entries) {
    const std::string full = entry.suite + "." + entry.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    if (list_only) {
      std::cout << full << "\n";
      continue;
    }
    ++executed;
    g_current_failed = false;
    g_current_test_name = full;
    std::cout << "[ RUN  ] " << full << std::endl;
    try {
      entry.function();
    } catch (const TestAbort&) {
      // Already reported by the macro that abandoned the test.
    } catch (const std::exception& error) {
      report_failure(__FILE__, __LINE__, std::string{"unexpected exception: "} + error.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "unexpected non-standard exception");
    }
    g_current_test_name.clear();
    if (g_current_failed) {
      ++failed;
      std::cout << "[ FAIL ] " << full << std::endl;
    } else {
      std::cout << "[  OK  ] " << full << std::endl;
    }
  }

  if (list_only) {
    return 0;
  }
  std::cout << "tests=" << executed << " passed=" << (executed - failed) << " failed=" << failed << std::endl;
  return failed == 0 ? 0 : 1;
}

}  // namespace fdc_test
