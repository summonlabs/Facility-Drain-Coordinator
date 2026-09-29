// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SRC_FILE_LOCK_HPP
#define FACILITYDRAIN_SRC_FILE_LOCK_HPP

#include "facilitydrain/errors.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace facilitydrain {
namespace detail {

/// An exclusive, operating system backed lock on a file.
///
/// This is what makes a durable store single writer. It is held for the whole
/// lifetime of a read write session and is released by the operating system
/// even when the process dies without running any cleanup, so a crashed writer
/// can never block or authorise a later one.
///
/// The lock is advisory in the sense that a process which never acquires it is
/// free to read the file; it is mandatory in the sense that a second acquire of
/// the same path fails while the first is held.
class FileLock {
 public:
  FileLock() = default;
  ~FileLock();

  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  /// Takes the lock without blocking. Fails with kStoreLocked when another live
  /// process holds it and with kStoreIoError for anything else. On success the
  /// file is truncated and the record is written into it, so an operator can
  /// see who holds the store.
  [[nodiscard]] static Result<FileLock> acquire(const std::filesystem::path& path, std::string_view record);

  [[nodiscard]] bool held() const noexcept;
  void release() noexcept;

 private:
  void* handle_ = nullptr;
};

/// Reads the record left in a lock file without taking the lock. Used by
/// read only inspection to report who currently holds the store.
[[nodiscard]] Result<std::string> read_lock_record(const std::filesystem::path& path);

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_SRC_FILE_LOCK_HPP
