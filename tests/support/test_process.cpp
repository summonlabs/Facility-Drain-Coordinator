// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "test_process.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#error "The Facility Drain Coordinator test process support targets Windows."
#endif

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>

namespace fdc_test {
namespace {

/// The exit code given to a terminated child. It is deliberately not a value a
/// child mode returns for a real result, so an abrupt end can never be mistaken
/// for a clean run.
constexpr UINT kTerminatedExitCode = 0xDEADu;

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return {};
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

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

/// Process level diagnostics go to standard error, so the harness report on
/// standard output stays machine readable. In child mode standard error is
/// redirected into the captured output, where the diagnostic stays visible.
void report_diagnostic(const std::string& message) {
  std::cerr << "test_process: " << message << std::endl;
}

/// Converts narrowly encoded text into the wide form the Win32 API takes.
///
/// The system code page is used rather than UTF-8 because that is what
/// std::filesystem::path::string() produces and, more importantly, what the
/// child's own narrow argv is decoded from: the conversion is its exact
/// inverse, so a mode or an argument reaches the child byte for byte, whether
/// it is a path with spaces, a quoted fragment, or text the library reads as
/// UTF-8. The conversion can only fail when the code page itself is unusable,
/// and the caller reports that instead of starting a child with wrong text.
[[nodiscard]] bool to_wide(const std::string& text, std::wstring& result) {
  result.clear();
  if (text.empty()) {
    return true;
  }
  const int length = static_cast<int>(text.size());
  const int size = MultiByteToWideChar(CP_ACP, 0, text.data(), length, nullptr, 0);
  if (size <= 0) {
    return false;
  }
  result.resize(static_cast<std::size_t>(size));
  const int written = MultiByteToWideChar(CP_ACP, 0, text.data(), length, result.data(), size);
  return written == size;
}

/// The standard Windows command line quoting routine. The argument is wrapped
/// in double quotes, embedded quotes are escaped, and every backslash that
/// precedes a quote or the closing quote is doubled, so the parser on the far
/// side reproduces the argument exactly.
[[nodiscard]] std::wstring quote_argument(std::wstring_view argument) {
  std::wstring result;
  result.push_back(L'"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

/// Builds the full command line, quoting the executable and every argument.
[[nodiscard]] bool build_command_line(const std::string& mode, const std::vector<std::string>& args,
                                      std::wstring& command) {
  command = quote_argument(executable_path().wstring());
  command.append(L" --child");
  std::wstring wide;
  if (!to_wide(mode, wide)) {
    report_diagnostic("the child mode \"" + mode + "\" cannot be represented on this system");
    return false;
  }
  command.push_back(L' ');
  command.append(quote_argument(wide));
  for (const std::string& argument : args) {
    if (!to_wide(argument, wide)) {
      report_diagnostic("a child argument cannot be represented on this system");
      return false;
    }
    command.push_back(L' ');
    command.append(quote_argument(wide));
  }
  return true;
}

}  // namespace

ChildProcess::~ChildProcess() {
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
  if (!output_read_ && !output_path_.empty()) {
    // The output was never read, so leaving it behind would leak a file into
    // the temporary directory for every failed test.
    std::error_code error;
    std::filesystem::remove(output_path_, error);
  }
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_),
      output_path_(std::move(other.output_path_)),
      output_read_(other.output_read_) {
  other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(handle_));
    }
    handle_ = other.handle_;
    output_path_ = std::move(other.output_path_);
    output_read_ = other.output_read_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool ChildProcess::valid() const noexcept { return handle_ != nullptr; }

std::uint64_t ChildProcess::process_id() const noexcept {
  if (handle_ == nullptr) {
    return 0;
  }
  return static_cast<std::uint64_t>(GetProcessId(static_cast<HANDLE>(handle_)));
}

bool ChildProcess::running() const noexcept {
  if (handle_ == nullptr) {
    return false;
  }
  return WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_TIMEOUT;
}

ChildProcess ChildProcess::start(const std::string& mode, const std::vector<std::string>& args,
                                 const std::filesystem::path& output_path) {
  ChildProcess child;
  if (executable_path().empty()) {
    report_diagnostic("the test executable path has not been set");
    return child;
  }
  std::wstring command;
  if (!build_command_line(mode, args, command)) {
    return child;
  }
  child.output_path_ = output_path;

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = nullptr;
  attributes.bInheritHandle = TRUE;

  const std::wstring output_wide = output_path.wstring();
  const HANDLE output = CreateFileW(output_wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    report_diagnostic("could not create the child output file " + display_path(output_path) + " (error " +
                      std::to_string(GetLastError()) + ")");
    return child;
  }

  // The child's standard input is the null device: it never reads from the
  // harness, and no pipe is involved anywhere in this helper.
  bool owns_input = true;
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (input == INVALID_HANDLE_VALUE) {
    owns_input = false;
    input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) {
      report_diagnostic("no standard input handle is available for the child (error " +
                        std::to_string(GetLastError()) + ")");
      CloseHandle(output);
      return child;
    }
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = output;
  startup.hStdError = output;

  PROCESS_INFORMATION information{};
  const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                      &startup, &information);
  const DWORD start_error = started == 0 ? GetLastError() : 0u;
  CloseHandle(output);
  if (owns_input) {
    CloseHandle(input);
  }
  if (started == 0) {
    report_diagnostic("could not start " + display_path(executable_path()) + " in the child mode " + mode +
                      " (error " + std::to_string(start_error) + ")");
    return child;
  }
  CloseHandle(information.hThread);
  child.handle_ = information.hProcess;
  return child;
}

bool ChildProcess::wait(int& exit_code, std::string& output) {
  if (handle_ == nullptr) {
    report_diagnostic("wait was called on a process that is not running");
    return false;
  }
  const HANDLE process = static_cast<HANDLE>(handle_);
  const DWORD result = WaitForSingleObject(process, INFINITE);
  if (result != WAIT_OBJECT_0) {
    report_diagnostic("the child process could not be waited for (wait result " + std::to_string(result) + ")");
    return false;
  }
  DWORD code = 0;
  if (GetExitCodeProcess(process, &code) == 0) {
    report_diagnostic("the child exit code could not be read (error " + std::to_string(GetLastError()) + ")");
    return false;
  }
  exit_code = static_cast<int>(code);
  output = read_text_file(output_path_);
  output_read_ = true;
  CloseHandle(process);
  handle_ = nullptr;
  return true;
}

void ChildProcess::terminate() {
  if (handle_ == nullptr) {
    return;
  }
  const HANDLE process = static_cast<HANDLE>(handle_);
  // A child that already ended is not an error: terminating it is only
  // meaningful while it runs, and the crash tests do race with their child's
  // own exit path.
  if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
    if (TerminateProcess(process, kTerminatedExitCode) == 0) {
      report_diagnostic("the child process could not be terminated (error " + std::to_string(GetLastError()) + ")");
    }
  }
  WaitForSingleObject(process, INFINITE);
  CloseHandle(process);
  handle_ = nullptr;
}

int run_child(const std::string& mode, const std::vector<std::string>& args, std::string& output) {
  const std::filesystem::path directory = make_temp_directory("child");
  const std::filesystem::path output_path = directory / "output.txt";
  int exit_code = -1;
  {
    ChildProcess child = ChildProcess::start(mode, args, output_path);
    if (!child.valid()) {
      output = "the child process could not be started";
      remove_tree(directory);
      return -1;
    }
    if (!child.wait(exit_code, output)) {
      output = "the child process could not be waited for";
      remove_tree(directory);
      return -1;
    }
  }
  remove_tree(directory);
  return exit_code;
}

void sleep_milliseconds(std::uint32_t milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds{milliseconds});
}

bool wait_for_file(const std::filesystem::path& path, const ChildProcess& child, std::uint32_t bound_milliseconds) {
  constexpr std::uint32_t kStepMilliseconds = 5;
  std::uint32_t waited = 0;
  for (;;) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      return true;
    }
    if (!child.running()) {
      return false;
    }
    if (waited >= bound_milliseconds) {
      return false;
    }
    sleep_milliseconds(kStepMilliseconds);
    waited += kStepMilliseconds;
  }
}

}  // namespace fdc_test
