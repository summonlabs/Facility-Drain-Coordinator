// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "file_lock.hpp"

#include "file_ops.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace facilitydrain {
namespace detail {
namespace {

/// A lock record is a short line of text. Bounding it means a corrupted or
/// hostile lock file cannot make a diagnostic read allocate without limit.
constexpr std::uint64_t kMaxLockRecordBytes = 4096;

/// The platform's own text for a failure code. A detail says why as well as
/// which number, and never reports a bare code with no explanation.
[[nodiscard]] std::string describe_platform_error(int code) {
  return std::error_code{code, std::system_category()}.message() + " (error " + std::to_string(code) + ")";
}

#ifdef _WIN32
/// The Windows lock is taken on one reserved byte rather than over the whole
/// file, and the reason is not an optimisation.
///
/// A Windows byte range lock is mandatory for I/O: while an exclusive lock
/// covers a byte, no other handle may read or write it. An exclusive lock over
/// the whole file would therefore make the holder's own record unreadable by
/// exactly the read only inspector whose purpose is to report who holds the
/// store, and the kStoreLocked detail could never name the holder.
///
/// The reserved byte sits far beyond any record this class writes, so every
/// writer is still excluded -- a second lock of the same byte fails with
/// ERROR_LOCK_VIOLATION, which is what makes the refusal a lock and not a
/// convention -- while the record itself stays readable.
constexpr std::uint64_t kLockByteOffset = 1024ULL * 1024ULL;
constexpr DWORD kLockByteCount = 1;
constexpr DWORD kLockByteCountHigh = 0;

/// The single byte range every acquire and release uses.
[[nodiscard]] OVERLAPPED lock_byte_range() noexcept {
  OVERLAPPED range{};
  range.Offset = static_cast<DWORD>(kLockByteOffset & 0xFFFFFFFFULL);
  range.OffsetHigh = static_cast<DWORD>(kLockByteOffset >> 32U);
  return range;
}
#else
/// O_CLOEXEC is POSIX.1-2008; a platform without it simply gets no flag.
#ifdef O_CLOEXEC
constexpr int kCloseOnExec = O_CLOEXEC;
#else
constexpr int kCloseOnExec = 0;
#endif
#endif

/// The platform handle plus the bookkeeping the FileLock class has no room for.
///
/// FileLock carries a single void* so that its layout stays trivial, and this
/// record is what that pointer points at. Owning it here is what makes the move
/// operations and the destructor one line each.
struct LockState {
  /// Normalised path, used as the key of the process wide registry below.
  std::string key{};
  /// The record written into the lock file. The bytes live here so the write
  /// path and the diagnostic path share one copy of the text.
  std::string record{};
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
#else
  int descriptor = -1;
#endif

  LockState() = default;
  LockState(const LockState&) = delete;
  LockState& operator=(const LockState&) = delete;
  ~LockState() { close_handle(); }

  void close_handle() noexcept {
#ifdef _WIN32
    if (handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
    }
#else
    if (descriptor >= 0) {
      ::close(descriptor);
      descriptor = -1;
    }
#endif
  }

  /// Drops the byte range lock without closing the file. Closing would release
  /// it too, but an explicit unlock keeps the release path readable and
  /// identical on both platforms.
  void unlock() noexcept {
#ifdef _WIN32
    if (handle != INVALID_HANDLE_VALUE) {
      OVERLAPPED range = lock_byte_range();
      ::UnlockFileEx(handle, 0, kLockByteCount, kLockByteCountHigh, &range);
    }
#else
    if (descriptor >= 0) {
      struct flock lock{};
      lock.l_type = F_UNLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = 0;
      lock.l_len = 0;
      ::fcntl(descriptor, F_SETLK, &lock);
    }
#endif
  }
};

// ---------------------------------------------------------------------------
// Process wide registry of held locks
// ---------------------------------------------------------------------------
//
// The operating system lock alone is not enough. Windows locks belong to a file
// handle, and POSIX fcntl locks belong to the process: on POSIX a second
// acquire from the same process would simply succeed, and two sessions in one
// process would both believe they are the only writer. The registry makes the
// same process attempting a second acquire of the same path fail with
// kStoreLocked, which is what the contract promises.
//
// It is deliberately process local and holds no authority of any kind: it is a
// mutual exclusion device for this process, not a durable fact, and it
// disappears with the process. Nothing that is written or decided depends on
// it. The mutex makes the check-then-insert atomic against another thread
// locking the same path.

std::mutex& registry_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, LockState*>& registry() {
  static std::map<std::string, LockState*> held{};
  return held;
}

/// A path key for the registry. Windows path spellings are case insensitive, so
/// ASCII is folded there to stop two spellings of one file from looking like
/// two files; the operating system lock is still the protection for every other
/// spelling.
[[nodiscard]] std::string lock_key(const std::filesystem::path& path) {
  std::string key = to_utf8(path.lexically_normal());
#ifdef _WIN32
  for (char& character : key) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
#endif
  return key;
}

[[nodiscard]] std::string locked_detail(const std::filesystem::path& path) {
  std::string detail = "the writer lock is already held [path: ";
  detail.append(to_utf8(path));
  detail.append("]");
  const Result<std::string> holder = read_lock_record(path);
  if (holder && !holder.value().empty()) {
    detail.append("; holder record: ");
    detail.append(holder.value());
  } else {
    // The lock file exists but the record could not be read, which is possible
    // when the holder does not share read access. Saying so is more useful than
    // inventing a record or failing the diagnostic.
    detail.append("; the holder record is empty or unreadable");
  }
  return detail;
}

}  // namespace

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

Result<FileLock> FileLock::acquire(const std::filesystem::path& path, std::string_view record) {
  try {
    auto state = std::make_unique<LockState>();
    state->key = lock_key(path);
    state->record.assign(record);

    std::lock_guard<std::mutex> guard{registry_mutex()};
    if (registry().find(state->key) != registry().end()) {
      return make_error<FileLock>(ErrorCode::kStoreLocked, locked_detail(path));
    }

#ifdef _WIN32
    // Read and write sharing are granted: an operator, a read only inspector
    // and a second writer must all be able to *open* the file, so that each of
    // them can read who holds it. It is the byte range lock below, not the open,
    // that decides who may be the writer.
    state->handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state->handle == INVALID_HANDLE_VALUE) {
      const DWORD last = ::GetLastError();
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: CreateFileW: " +
                                      describe_platform_error(static_cast<int>(last)) + " [path: " +
                                      to_utf8(path) + "]");
    }
    OVERLAPPED range = lock_byte_range();
    if (::LockFileEx(state->handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, kLockByteCount,
                     kLockByteCountHigh, &range) == 0) {
      const DWORD last = ::GetLastError();
      if (last == ERROR_LOCK_VIOLATION || last == ERROR_SHARING_VIOLATION) {
        return make_error<FileLock>(ErrorCode::kStoreLocked, locked_detail(path));
      }
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: LockFileEx: " +
                                      describe_platform_error(static_cast<int>(last)) + " [path: " +
                                      to_utf8(path) + "]");
    }
    // The record is written only after the lock is held, so a reader can never
    // see a record left behind by a process that does not hold the lock.
    LARGE_INTEGER origin{};
    if (::SetFilePointerEx(state->handle, origin, nullptr, FILE_BEGIN) == 0 || ::SetEndOfFile(state->handle) == 0) {
      const DWORD last = ::GetLastError();
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: truncating the lock file: " +
                                      describe_platform_error(static_cast<int>(last)) + " [path: " +
                                      to_utf8(path) + "]");
    }
    std::size_t written_total = 0;
    while (written_total < state->record.size()) {
      const DWORD wanted =
          static_cast<DWORD>((std::min)(state->record.size() - written_total,
                                        static_cast<std::size_t>(MAXDWORD)));
      DWORD written = 0;
      if (::WriteFile(state->handle, state->record.data() + written_total, wanted, &written, nullptr) == 0) {
        const DWORD last = ::GetLastError();
        return make_error<FileLock>(ErrorCode::kStoreIoError,
                                    "FileLock::acquire: writing the lock record: " +
                                        describe_platform_error(static_cast<int>(last)) + " [path: " +
                                        to_utf8(path) + "]");
      }
      if (written != wanted) {
        return make_error<FileLock>(ErrorCode::kStoreIoError,
                                    "FileLock::acquire: writing the lock record: a short write of " +
                                        std::to_string(written) + " of " + std::to_string(wanted) +
                                        " bytes [path: " + to_utf8(path) + "]");
      }
      written_total += static_cast<std::size_t>(written);
    }
    if (::FlushFileBuffers(state->handle) == 0) {
      // A record that is not durable is a record an operator cannot rely on,
      // and the whole point of the file is that it outlives this process.
      const DWORD last = ::GetLastError();
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: flushing the lock record: " +
                                      describe_platform_error(static_cast<int>(last)) + " [path: " +
                                      to_utf8(path) + "]");
    }
#else
    state->descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | kCloseOnExec, 0600);
    if (state->descriptor < 0) {
      const int last = errno;
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: open: " + describe_platform_error(last) + " [path: " +
                                      to_utf8(path) + "]");
    }
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    // A length of zero means "to the end of the file, however large it grows",
    // so the whole file is covered without knowing its size.
    lock.l_len = 0;
    if (::fcntl(state->descriptor, F_SETLK, &lock) != 0) {
      const int last = errno;
      if (last == EACCES || last == EAGAIN) {
        return make_error<FileLock>(ErrorCode::kStoreLocked, locked_detail(path));
      }
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: fcntl(F_SETLK): " + describe_platform_error(last) +
                                      " [path: " + to_utf8(path) + "]");
    }
    if (::ftruncate(state->descriptor, 0) != 0 || ::lseek(state->descriptor, 0, SEEK_SET) < 0) {
      const int last = errno;
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: truncating the lock file: " + describe_platform_error(last) +
                                      " [path: " + to_utf8(path) + "]");
    }
    std::size_t written_total = 0;
    while (written_total < state->record.size()) {
      const ssize_t written =
          ::write(state->descriptor, state->record.data() + written_total, state->record.size() - written_total);
      if (written < 0) {
        const int last = errno;
        if (last == EINTR) {
          continue;
        }
        return make_error<FileLock>(ErrorCode::kStoreIoError,
                                    "FileLock::acquire: writing the lock record: " +
                                        describe_platform_error(last) + " [path: " + to_utf8(path) + "]");
      }
      if (written == 0) {
        return make_error<FileLock>(ErrorCode::kStoreIoError,
                                    "FileLock::acquire: writing the lock record made no progress [path: " +
                                        to_utf8(path) + "]");
      }
      written_total += static_cast<std::size_t>(written);
    }
    if (::fsync(state->descriptor) != 0) {
      const int last = errno;
      return make_error<FileLock>(ErrorCode::kStoreIoError,
                                  "FileLock::acquire: flushing the lock record: " + describe_platform_error(last) +
                                      " [path: " + to_utf8(path) + "]");
    }
#endif

    registry().emplace(state->key, state.get());
    FileLock result;
    result.handle_ = state.release();
    // The result is moved into place: a FileLock is the sole owner of the lock,
    // so it is movable but never copyable.
    return std::move(result);
  } catch (const std::exception& error) {
    return make_error<FileLock>(ErrorCode::kStoreIoError,
                                "FileLock::acquire: " + std::string{error.what()} + " [path: " + to_utf8(path) +
                                    "]");
  }
}

bool FileLock::held() const noexcept { return handle_ != nullptr; }

void FileLock::release() noexcept {
  LockState* state = static_cast<LockState*>(handle_);
  handle_ = nullptr;
  if (state == nullptr) {
    return;
  }
  state->unlock();
  // The operating system releases the lock when the handle closes, which is the
  // property this whole class exists for: a process that dies without running
  // this function cannot leave a store locked.
  state->close_handle();
  // The registry entry is removed only after the lock is really gone, so a
  // second acquire in this process can never slip in while the file is still
  // locked. Until then it is refused conservatively.
  {
    std::lock_guard<std::mutex> guard{registry_mutex()};
    const auto found = registry().find(state->key);
    if (found != registry().end() && found->second == state) {
      registry().erase(found);
    }
  }
  delete state;
}

Result<std::string> read_lock_record(const std::filesystem::path& path) {
  if (!path_exists(path)) {
    return make_error<std::string>(ErrorCode::kStoreNotFound,
                                   "read_lock_record: the lock file does not exist [path: " + to_utf8(path) + "]");
  }
  const Result<std::vector<std::byte>> bytes = read_file_bounded(path, kMaxLockRecordBytes);
  if (!bytes) {
    // The file exists but cannot be read, which happens when a holder does not
    // share read access or when the file was removed between the two calls. An
    // empty record is the honest answer for a diagnostic: the caller learns
    // nothing about the holder and no error is invented for it.
    return std::string{};
  }
  const std::vector<std::byte>& buffer = bytes.value();
  if (buffer.empty()) {
    return std::string{};
  }
  return std::string{reinterpret_cast<const char*>(buffer.data()), buffer.size()};
}

}  // namespace detail
}  // namespace facilitydrain
