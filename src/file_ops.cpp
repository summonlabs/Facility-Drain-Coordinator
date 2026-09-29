// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "file_ops.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
// rename(2) is declared by <stdio.h>; the POSIX branch calls it directly rather
// than through std::filesystem, which reports failures as exceptions.
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace facilitydrain {
namespace detail {
namespace {

/// Platform calls take 32 bit lengths, so every transfer is chunked. The bound
/// also keeps a single failed read from looking like an end of file.
constexpr std::size_t kChunkBytes = 1024U * 1024U;

[[nodiscard]] std::string describe_system_error(const std::error_code& error) {
  if (!error) {
    return "no system error was reported";
  }
  return error.message() + " (error " + std::to_string(error.value()) + ")";
}

/// Every failure detail names the operation and the path. A bare errno tells an
/// operator nothing about which file inside a store went wrong, and the path is
/// rendered as UTF-8 so the same text is produced on every platform.
[[nodiscard]] std::string failure_detail(std::string_view operation, const std::filesystem::path& path,
                                         std::string_view reason) {
  std::string detail;
  detail.reserve(operation.size() + reason.size() + 48U);
  detail.append(operation);
  detail.append(": ");
  detail.append(reason);
  detail.append(" [path: ");
  detail.append(to_utf8(path));
  detail.append("]");
  return detail;
}

[[nodiscard]] Status failure(ErrorCode code, std::string_view operation, const std::filesystem::path& path,
                             std::string_view reason) {
  return Status::failure(Error{code, failure_detail(operation, path, reason)});
}

template <class T>
[[nodiscard]] Result<T> failure_result(ErrorCode code, std::string_view operation,
                                       const std::filesystem::path& path, std::string_view reason) {
  return make_error<T>(code, failure_detail(operation, path, reason));
}

#ifdef _WIN32

[[nodiscard]] std::error_code windows_error(unsigned long code) {
  return std::error_code{static_cast<int>(code), std::system_category()};
}

/// The two Windows failures that mean "there is no such file or directory" get
/// the documented store code; everything else is an I/O failure.
[[nodiscard]] ErrorCode windows_missing_code(unsigned long code) noexcept {
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_NAME) {
    return ErrorCode::kStoreNotFound;
  }
  return ErrorCode::kStoreIoError;
}

/// Closes a Win32 handle on every path, including the early returns below.
class HandleGuard {
 public:
  explicit HandleGuard(HANDLE handle) noexcept : handle_(handle) {}
  ~HandleGuard() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
  }

  HandleGuard(const HandleGuard&) = delete;
  HandleGuard& operator=(const HandleGuard&) = delete;
  HandleGuard(HandleGuard&&) = delete;
  HandleGuard& operator=(HandleGuard&&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

#else

/// O_CLOEXEC is POSIX.1-2008; a platform without it simply gets no flag rather
/// than a compile error.
#ifdef O_CLOEXEC
constexpr int kCloseOnExec = O_CLOEXEC;
#else
constexpr int kCloseOnExec = 0;
#endif

[[nodiscard]] std::error_code errno_error(int error_number) {
  return std::error_code{error_number, std::system_category()};
}

[[nodiscard]] std::string describe_errno(int error_number) {
  return describe_system_error(errno_error(error_number));
}

/// Closes a file descriptor on every path, including the early returns below.
class FdGuard {
 public:
  explicit FdGuard(int descriptor) noexcept : descriptor_(descriptor) {}
  ~FdGuard() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
  FdGuard(FdGuard&&) = delete;
  FdGuard& operator=(FdGuard&&) = delete;

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

 private:
  int descriptor_ = -1;
};

[[nodiscard]] ErrorCode errno_missing_code(int error_number) noexcept {
  if (error_number == ENOENT) {
    return ErrorCode::kStoreNotFound;
  }
  return ErrorCode::kStoreIoError;
}

#endif  // _WIN32

}  // namespace

Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes) {
#ifdef _WIN32
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD last = ::GetLastError();
    return failure_result<std::vector<std::byte>>(
        windows_missing_code(last), "read_file_bounded: GetFileAttributesW", path,
        describe_system_error(windows_error(last)));
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    // A directory is not a file and never has been: reading one would either
    // fail later or, worse, return something that looks like content.
    return failure_result<std::vector<std::byte>>(ErrorCode::kStoreIoError, "read_file_bounded", path,
                                                  "the path is a directory, not a file");
  }
  // Readers share the file with other readers and with the writer that replaces
  // it; sharing delete here is what lets an atomic replacement proceed while a
  // read only inspection is still holding the previous file open.
  HandleGuard guard{::CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (!guard.valid()) {
    const DWORD last = ::GetLastError();
    return failure_result<std::vector<std::byte>>(windows_missing_code(last),
                                                  "read_file_bounded: CreateFileW", path,
                                                  describe_system_error(windows_error(last)));
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(guard.get(), &size) == 0) {
    const DWORD last = ::GetLastError();
    return failure_result<std::vector<std::byte>>(ErrorCode::kStoreIoError, "read_file_bounded: GetFileSizeEx",
                                                  path, describe_system_error(windows_error(last)));
  }
  const std::uint64_t file_bytes = static_cast<std::uint64_t>(size.QuadPart);
  // The bound is checked before the buffer is created. A file that claims to be
  // enormous must not be able to make this process allocate for it.
  if (file_bytes > max_bytes) {
    return failure_result<std::vector<std::byte>>(
        ErrorCode::kPayloadTooLarge, "read_file_bounded", path,
        "the file is " + std::to_string(file_bytes) + " bytes, above the bound of " +
            std::to_string(max_bytes) + " bytes");
  }
  if (file_bytes > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    return failure_result<std::vector<std::byte>>(ErrorCode::kPayloadTooLarge, "read_file_bounded", path,
                                                  "the file is larger than this process can address");
  }
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_bytes));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const DWORD wanted = static_cast<DWORD>((std::min)(buffer.size() - offset, kChunkBytes));
    DWORD read_bytes = 0;
    if (::ReadFile(guard.get(), buffer.data() + offset, wanted, &read_bytes, nullptr) == 0) {
      const DWORD last = ::GetLastError();
      // A file that shrank under the reader reports end of file; that is a short
      // read, not an I/O failure.
      if (last == ERROR_HANDLE_EOF) {
        break;
      }
      return failure_result<std::vector<std::byte>>(ErrorCode::kStoreIoError,
                                                    "read_file_bounded: ReadFile", path,
                                                    describe_system_error(windows_error(last)));
    }
    if (read_bytes == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read_bytes);
  }
  if (offset != buffer.size()) {
    return failure_result<std::vector<std::byte>>(
        ErrorCode::kStoreTruncated, "read_file_bounded", path,
        "the file reported " + std::to_string(file_bytes) + " bytes but only " + std::to_string(offset) +
            " bytes could be read");
  }
  return buffer;
#else
  FdGuard guard{::open(path.c_str(), O_RDONLY | kCloseOnExec)};
  if (!guard.valid()) {
    const int last = errno;
    return failure_result<std::vector<std::byte>>(errno_missing_code(last), "read_file_bounded: open", path,
                                                  describe_errno(last));
  }
  struct stat info {};
  if (::fstat(guard.get(), &info) != 0) {
    const int last = errno;
    return failure_result<std::vector<std::byte>>(ErrorCode::kStoreIoError, "read_file_bounded: fstat", path,
                                                  describe_errno(last));
  }
  if (S_ISDIR(info.st_mode)) {
    return failure_result<std::vector<std::byte>>(ErrorCode::kStoreIoError, "read_file_bounded", path,
                                                  "the path is a directory, not a file");
  }
  if (info.st_size < 0) {
    return failure_result<std::vector<std::byte>>(ErrorCode::kStoreIoError, "read_file_bounded: fstat", path,
                                                  "the file reports a negative size");
  }
  const std::uint64_t file_bytes = static_cast<std::uint64_t>(info.st_size);
  if (file_bytes > max_bytes) {
    return failure_result<std::vector<std::byte>>(
        ErrorCode::kPayloadTooLarge, "read_file_bounded", path,
        "the file is " + std::to_string(file_bytes) + " bytes, above the bound of " +
            std::to_string(max_bytes) + " bytes");
  }
  if (file_bytes > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    return failure_result<std::vector<std::byte>>(ErrorCode::kPayloadTooLarge, "read_file_bounded", path,
                                                  "the file is larger than this process can address");
  }
  std::vector<std::byte> buffer(static_cast<std::size_t>(file_bytes));
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const std::size_t wanted = (std::min)(buffer.size() - offset, kChunkBytes);
    const ssize_t read_bytes = ::read(guard.get(), buffer.data() + offset, wanted);
    if (read_bytes < 0) {
      const int last = errno;
      if (last == EINTR) {
        continue;
      }
      return failure_result<std::vector<std::byte>>(ErrorCode::kStoreIoError, "read_file_bounded: read", path,
                                                    describe_errno(last));
    }
    if (read_bytes == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read_bytes);
  }
  if (offset != buffer.size()) {
    return failure_result<std::vector<std::byte>>(
        ErrorCode::kStoreTruncated, "read_file_bounded", path,
        "the file reported " + std::to_string(file_bytes) + " bytes but only " + std::to_string(offset) +
            " bytes could be read");
  }
  return buffer;
#endif
}

Status write_new_file(const std::filesystem::path& path, std::span<const std::byte> bytes) {
#ifdef _WIN32
  // CREATE_NEW with no write sharing: the file is ours alone while it is being
  // written, and an existing name -- including one an attacker placed in
  // advance -- is never opened, followed or overwritten.
  HandleGuard guard{::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!guard.valid()) {
    const DWORD last = ::GetLastError();
    ErrorCode code = ErrorCode::kStoreIoError;
    if (last == ERROR_FILE_EXISTS || last == ERROR_ALREADY_EXISTS) {
      code = ErrorCode::kStoreExists;
    } else if (last == ERROR_PATH_NOT_FOUND) {
      code = ErrorCode::kStoreNotFound;
    }
    return failure(code, "write_new_file: CreateFileW with CREATE_NEW", path,
                   describe_system_error(windows_error(last)));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD wanted = static_cast<DWORD>((std::min)(bytes.size() - offset, kChunkBytes));
    DWORD written = 0;
    if (::WriteFile(guard.get(), bytes.data() + offset, wanted, &written, nullptr) == 0) {
      const DWORD last = ::GetLastError();
      return failure(ErrorCode::kStoreIoError, "write_new_file: WriteFile", path,
                     describe_system_error(windows_error(last)));
    }
    if (written != wanted) {
      return failure(ErrorCode::kStoreIoError, "write_new_file: WriteFile", path,
                     "a short write of " + std::to_string(written) + " of " + std::to_string(wanted) +
                         " bytes; a partially written file is an error, never a silent truncation");
    }
    offset += static_cast<std::size_t>(written);
  }
  return Status::success();
#else
  FdGuard guard{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | kCloseOnExec, 0600)};
  if (!guard.valid()) {
    const int last = errno;
    ErrorCode code = ErrorCode::kStoreIoError;
    if (last == EEXIST) {
      code = ErrorCode::kStoreExists;
    } else if (last == ENOENT) {
      code = ErrorCode::kStoreNotFound;
    }
    return failure(code, "write_new_file: open with O_CREAT|O_EXCL", path, describe_errno(last));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t wanted = (std::min)(bytes.size() - offset, kChunkBytes);
    const ssize_t written = ::write(guard.get(), bytes.data() + offset, wanted);
    if (written < 0) {
      const int last = errno;
      if (last == EINTR) {
        continue;
      }
      return failure(ErrorCode::kStoreIoError, "write_new_file: write", path, describe_errno(last));
    }
    if (written == 0 || static_cast<std::size_t>(written) != wanted) {
      return failure(ErrorCode::kStoreIoError, "write_new_file: write", path,
                     "a short write of " + std::to_string(written) + " of " + std::to_string(wanted) +
                         " bytes; a partially written file is an error, never a silent truncation");
    }
    offset += static_cast<std::size_t>(written);
  }
  return Status::success();
#endif
}

Status write_existing_file_at(const std::filesystem::path& path, std::uint64_t offset,
                              std::span<const std::byte> bytes) {
#ifdef _WIN32
  if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
    return failure(ErrorCode::kStoreIoError, "write_existing_file_at", path,
                   "the offset " + std::to_string(offset) + " is beyond the range a file position can express");
  }
  HandleGuard guard{::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!guard.valid()) {
    const DWORD last = ::GetLastError();
    return failure(windows_missing_code(last), "write_existing_file_at: CreateFileW", path,
                   describe_system_error(windows_error(last)));
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (::SetFilePointerEx(guard.get(), position, nullptr, FILE_BEGIN) == 0) {
    const DWORD last = ::GetLastError();
    return failure(ErrorCode::kStoreIoError, "write_existing_file_at: SetFilePointerEx", path,
                   describe_system_error(windows_error(last)));
  }
  std::size_t written_total = 0;
  while (written_total < bytes.size()) {
    const DWORD wanted = static_cast<DWORD>((std::min)(bytes.size() - written_total, kChunkBytes));
    DWORD written = 0;
    if (::WriteFile(guard.get(), bytes.data() + written_total, wanted, &written, nullptr) == 0) {
      const DWORD last = ::GetLastError();
      return failure(ErrorCode::kStoreIoError, "write_existing_file_at: WriteFile", path,
                     describe_system_error(windows_error(last)));
    }
    if (written != wanted) {
      return failure(ErrorCode::kStoreIoError, "write_existing_file_at: WriteFile", path,
                     "a short write of " + std::to_string(written) + " of " + std::to_string(wanted) + " bytes");
    }
    written_total += static_cast<std::size_t>(written);
  }
  return Status::success();
#else
  if (offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())) {
    return failure(ErrorCode::kStoreIoError, "write_existing_file_at", path,
                   "the offset " + std::to_string(offset) + " is beyond the range a file position can express");
  }
  FdGuard guard{::open(path.c_str(), O_WRONLY | kCloseOnExec)};
  if (!guard.valid()) {
    const int last = errno;
    return failure(errno_missing_code(last), "write_existing_file_at: open", path, describe_errno(last));
  }
  std::size_t written_total = 0;
  while (written_total < bytes.size()) {
    const std::size_t wanted = (std::min)(bytes.size() - written_total, kChunkBytes);
    const ssize_t written =
        ::pwrite(guard.get(), bytes.data() + written_total, wanted, static_cast<off_t>(offset) +
                                                                        static_cast<off_t>(written_total));
    if (written < 0) {
      const int last = errno;
      if (last == EINTR) {
        continue;
      }
      return failure(ErrorCode::kStoreIoError, "write_existing_file_at: pwrite", path, describe_errno(last));
    }
    if (written == 0 || static_cast<std::size_t>(written) != wanted) {
      return failure(ErrorCode::kStoreIoError, "write_existing_file_at: pwrite", path,
                     "a short write of " + std::to_string(written) + " of " + std::to_string(wanted) + " bytes");
    }
    written_total += static_cast<std::size_t>(written);
  }
  return Status::success();
#endif
}

Status sync_file(const std::filesystem::path& path) {
#ifdef _WIN32
  // FlushFileBuffers requires a handle with write access, so the file is opened
  // for writing even though nothing is written; the flush itself is the point.
  HandleGuard guard{::CreateFileW(path.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (!guard.valid()) {
    const DWORD last = ::GetLastError();
    return failure(windows_missing_code(last), "sync_file: CreateFileW", path,
                   describe_system_error(windows_error(last)));
  }
  if (::FlushFileBuffers(guard.get()) == 0) {
    const DWORD last = ::GetLastError();
    return failure(ErrorCode::kStoreIoError, "sync_file: FlushFileBuffers", path,
                   describe_system_error(windows_error(last)));
  }
  return Status::success();
#else
  // fsync works on a read only descriptor, and opening read only means a flush
  // never needs write permission on a file this process did not create.
  FdGuard guard{::open(path.c_str(), O_RDONLY | kCloseOnExec)};
  if (!guard.valid()) {
    const int last = errno;
    return failure(errno_missing_code(last), "sync_file: open", path, describe_errno(last));
  }
  if (::fsync(guard.get()) != 0) {
    const int last = errno;
    return failure(ErrorCode::kStoreIoError, "sync_file: fsync", path, describe_errno(last));
  }
  return Status::success();
#endif
}

Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
  // MOVEFILE_WRITE_THROUGH is what makes the replacement durable before this
  // returns: without it the rename could still be sitting in a cache when the
  // process dies, and the pointer would name a generation that is not there.
  if (::MoveFileExW(source.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const DWORD last = ::GetLastError();
    return failure(windows_missing_code(last), "replace_file: MoveFileExW", source,
                   describe_system_error(windows_error(last)) + ", destination [" + to_utf8(destination) + "]");
  }
  return Status::success();
#else
  if (::rename(source.c_str(), destination.c_str()) != 0) {
    const int last = errno;
    return failure(errno_missing_code(last), "replace_file: rename", source,
                   describe_errno(last) + ", destination [" + to_utf8(destination) + "]");
  }
  // rename(2) is atomic but its directory entry is not durable until the
  // containing directory is flushed.
  const std::filesystem::path parent =
      destination.has_parent_path() ? destination.parent_path() : std::filesystem::path{"."};
  return sync_directory(parent);
#endif
}

Status sync_directory(const std::filesystem::path& path) {
#ifdef _WIN32
  // Windows has no directory fsync: a directory is not openable as a file for
  // flushing, and NTFS metadata changes are ordered by the file system's log.
  // The durability of a replacement therefore comes from MOVEFILE_WRITE_THROUGH
  // in replace_file, and reporting success here is the honest answer rather
  // than a stub with nothing behind it. The difference is documented in
  // persistence.hpp.
  (void)path;
  return Status::success();
#else
  FdGuard guard{::open(path.c_str(), O_RDONLY | kCloseOnExec)};
  if (!guard.valid()) {
    const int last = errno;
    return failure(errno_missing_code(last), "sync_directory: open", path, describe_errno(last));
  }
  if (::fsync(guard.get()) != 0) {
    const int last = errno;
    // Some file systems refuse to flush a directory handle. That is a real
    // durability limitation rather than a success, so it is reported.
    return failure(ErrorCode::kStoreIoError, "sync_directory: fsync", path, describe_errno(last));
  }
  return Status::success();
#endif
}

Result<std::uint64_t> file_size(const std::filesystem::path& path) {
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) == 0) {
    const DWORD last = ::GetLastError();
    return failure_result<std::uint64_t>(windows_missing_code(last), "file_size: GetFileAttributesExW", path,
                                         describe_system_error(windows_error(last)));
  }
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    return failure_result<std::uint64_t>(ErrorCode::kStoreIoError, "file_size", path,
                                         "the path is a directory, and a directory has no file size");
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | static_cast<std::uint64_t>(info.nFileSizeLow);
  return size;
#else
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    const int last = errno;
    return failure_result<std::uint64_t>(errno_missing_code(last), "file_size: stat", path, describe_errno(last));
  }
  if (S_ISDIR(info.st_mode)) {
    return failure_result<std::uint64_t>(ErrorCode::kStoreIoError, "file_size", path,
                                         "the path is a directory, and a directory has no file size");
  }
  if (info.st_size < 0) {
    return failure_result<std::uint64_t>(ErrorCode::kStoreIoError, "file_size: stat", path,
                                         "the file reports a negative size");
  }
  return static_cast<std::uint64_t>(info.st_size);
#endif
}

bool path_exists(const std::filesystem::path& path) noexcept {
  try {
    std::error_code error;
    const bool present = std::filesystem::exists(path, error);
    // A path that cannot be inspected is reported as absent: this predicate
    // answers "is there something here", and an unreadable entry is not a
    // usable one.
    return !error && present;
  } catch (...) {
    return false;
  }
}

Status remove_file(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD last = ::GetLastError();
    if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) {
      return Status::success();
    }
    return failure(ErrorCode::kStoreIoError, "remove_file: GetFileAttributesW", path,
                   describe_system_error(windows_error(last)));
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
    // Cleanup removes files it created. Refusing directories means a transient
    // name can never be used to delete a directory tree.
    return failure(ErrorCode::kStoreIoError, "remove_file", path,
                   "the path is a directory; remove_file never removes directories");
  }
  if (::DeleteFileW(path.c_str()) == 0) {
    const DWORD last = ::GetLastError();
    if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) {
      return Status::success();
    }
    return failure(ErrorCode::kStoreIoError, "remove_file: DeleteFileW", path,
                   describe_system_error(windows_error(last)));
  }
  return Status::success();
#else
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    const int last = errno;
    if (last == ENOENT) {
      return Status::success();
    }
    return failure(ErrorCode::kStoreIoError, "remove_file: lstat", path, describe_errno(last));
  }
  if (S_ISDIR(info.st_mode)) {
    return failure(ErrorCode::kStoreIoError, "remove_file", path,
                   "the path is a directory; remove_file never removes directories");
  }
  if (::unlink(path.c_str()) != 0) {
    const int last = errno;
    if (last == ENOENT) {
      return Status::success();
    }
    return failure(ErrorCode::kStoreIoError, "remove_file: unlink", path, describe_errno(last));
  }
  return Status::success();
#endif
}

Status ensure_directory(const std::filesystem::path& path) {
  try {
    if (path.empty()) {
      return failure(ErrorCode::kStorePathInvalid, "ensure_directory", path, "the path is empty");
    }
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      if (std::filesystem::is_directory(path, error) && !error) {
        return Status::success();
      }
      return failure(ErrorCode::kStoreIoError, "ensure_directory", path,
                     "the path exists and is not a directory");
    }
    // create_directories would report a file in the middle of the path as a
    // generic failure; checking the immediate parent names the actual problem.
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty() && std::filesystem::exists(parent, error) && !error &&
        !std::filesystem::is_directory(parent, error)) {
      return failure(ErrorCode::kStoreIoError, "ensure_directory", path,
                     "the parent path exists and is not a directory");
    }
    error.clear();
    if (!std::filesystem::create_directories(path, error) && error) {
      return failure(ErrorCode::kStoreIoError, "ensure_directory: create_directories", path,
                     describe_system_error(error));
    }
    return Status::success();
  } catch (const std::filesystem::filesystem_error& error) {
    return failure(ErrorCode::kStoreIoError, "ensure_directory", path, error.what());
  } catch (const std::exception& error) {
    return failure(ErrorCode::kStoreIoError, "ensure_directory", path, error.what());
  }
}

Status remove_empty_directory(const std::filesystem::path& path) noexcept {
#ifdef _WIN32
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD last = ::GetLastError();
    if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) {
      return Status::success();
    }
    return failure(ErrorCode::kStoreIoError, "remove_empty_directory: GetFileAttributesW", path,
                   describe_system_error(windows_error(last)));
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
    return failure(ErrorCode::kStoreIoError, "remove_empty_directory", path,
                   "the path is not a directory");
  }
  if (::RemoveDirectoryW(path.c_str()) == 0) {
    const DWORD last = ::GetLastError();
    if (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) {
      return Status::success();
    }
    if (last == ERROR_DIR_NOT_EMPTY) {
      return failure(ErrorCode::kStoreIoError, "remove_empty_directory", path,
                     "the directory is not empty; this removal is never recursive");
    }
    return failure(ErrorCode::kStoreIoError, "remove_empty_directory: RemoveDirectoryW", path,
                   describe_system_error(windows_error(last)));
  }
  return Status::success();
#else
  if (::rmdir(path.c_str()) != 0) {
    const int last = errno;
    if (last == ENOENT) {
      return Status::success();
    }
    if (last == ENOTEMPTY || last == EEXIST) {
      return failure(ErrorCode::kStoreIoError, "remove_empty_directory", path,
                     "the directory is not empty; this removal is never recursive");
    }
    if (last == ENOTDIR) {
      return failure(ErrorCode::kStoreIoError, "remove_empty_directory", path,
                     "the path is not a directory");
    }
    return failure(ErrorCode::kStoreIoError, "remove_empty_directory: rmdir", path, describe_errno(last));
  }
  return Status::success();
#endif
}

std::string random_token() {
  // Two sources, neither trusted alone: random_device makes the name
  // unpredictable to another process, and the counter makes two tokens from
  // this process distinct even if the entropy source repeats itself. Neither is
  // the primary protection -- a transient file is created with an exclusive
  // create, so a guessed name still cannot take one over -- which is why the
  // counter below is the one piece of process wide state in this library: it
  // names transient files and can never influence a decision.
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t index = counter.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t bits = 0;
  try {
    std::random_device device;
    const std::uint64_t high = static_cast<std::uint64_t>(device());
    const std::uint64_t low = static_cast<std::uint64_t>(device());
    bits = (high << 32U) ^ low;
  } catch (const std::exception&) {
    // A platform without an entropy source must not make naming a transient
    // file fail. The counter below still keeps tokens distinct inside this
    // process, and the exclusive create still keeps them safe.
    bits = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  }
  bits ^= index * 0x9E3779B97F4A7C15ULL;
  // Avalanche, so consecutive counters do not produce tokens that differ only
  // in their low digits and are therefore easy to guess from one another.
  bits ^= bits >> 33U;
  bits *= 0xFF51AFD7ED558CCDULL;
  bits ^= bits >> 33U;
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string token(16, '0');
  for (std::size_t position = 0; position < 16U; ++position) {
    const std::size_t shift = (15U - position) * 4U;
    token[position] = kHexDigits[(bits >> shift) & 0x0FU];
  }
  return token;
}

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string to_utf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

}  // namespace detail
}  // namespace facilitydrain
