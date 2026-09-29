// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "test_harness.hpp"

namespace {

/// The exit code returned for a child mode no handler is registered for. It is
/// the same code the harness uses, so a mistyped mode is never mistaken for a
/// child result.
constexpr int kUnknownChildModeExitCode = 64;

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  if (argc > 1) {
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
  }

  // The child process helper starts a second copy of this exact executable, so
  // the path has to be absolute and resolved before any test runs. A failure to
  // resolve it is not fatal here: starting a child reports it as a diagnostic.
  if (argc > 0 && argv[0] != nullptr && argv[0][0] != '\0') {
    const std::filesystem::path self{argv[0]};
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(self, error);
    fdc_test::set_executable_path(error ? self : absolute.lexically_normal());
  }

  if (!arguments.empty() && arguments.front() == "--child") {
    if (arguments.size() < 2) {
      std::cerr << "usage: --child <mode> [args...]" << std::endl;
      return kUnknownChildModeExitCode;
    }
    const std::vector<std::string> rest(arguments.begin() + 2, arguments.end());
    return fdc_test::dispatch_child(arguments[1], rest);
  }
  return fdc_test::run_all(arguments);
}
