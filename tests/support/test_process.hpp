// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_TESTS_SUPPORT_TEST_PROCESS_HPP
#define FACILITYDRAIN_TESTS_SUPPORT_TEST_PROCESS_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fdc_test {

/// One real operating system process running this same test executable in a
/// named child mode.
///
/// The child's combined standard output and error are redirected into a file
/// rather than a pipe, so the harness never depends on pipe semantics that a
/// restricted environment might forbid. A start that fails is reported on
/// standard error, and the output file is removed by the destructor whenever
/// nobody read it, so a failure cannot leak the file either.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();

  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  /// Starts `--child <mode> args...` from this executable, with `output_path`
  /// receiving the child's combined output. The returned object is invalid when
  /// the process could not be started.
  [[nodiscard]] static ChildProcess start(const std::string& mode, const std::vector<std::string>& args,
                                          const std::filesystem::path& output_path);

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::uint64_t process_id() const noexcept;
  [[nodiscard]] bool running() const noexcept;

  /// Waits for the child to finish and reads its output. Returns false when the
  /// child could not be waited for, in which case `exit_code` and `output` are
  /// left untouched.
  [[nodiscard]] bool wait(int& exit_code, std::string& output);

  /// Terminates the child abruptly, without giving it a chance to run any
  /// cleanup. This is how a crash is simulated.
  void terminate();

 private:
  void* handle_ = nullptr;
  std::filesystem::path output_path_{};
  bool output_read_ = false;
};

/// Runs one child mode to completion and returns its exit code, with the
/// child's combined output in `output`. Returns -1 when the child could not be
/// started or waited for, and says so in `output`.
[[nodiscard]] int run_child(const std::string& mode, const std::vector<std::string>& args, std::string& output);

/// Waits until `path` exists, polling with a short sleep. Returns false when the
/// child exits first or the rendezvous bound is reached: both are reported as
/// test failures rather than being mistaken for success.
[[nodiscard]] bool wait_for_file(const std::filesystem::path& path, const ChildProcess& child,
                                 std::uint32_t bound_milliseconds = 60000);

void sleep_milliseconds(std::uint32_t milliseconds);

}  // namespace fdc_test

#endif  // FACILITYDRAIN_TESTS_SUPPORT_TEST_PROCESS_HPP
