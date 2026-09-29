// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SRC_FILE_OPS_HPP
#define FACILITYDRAIN_SRC_FILE_OPS_HPP

#include "facilitydrain/errors.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace facilitydrain {
namespace detail {

/// Reads a whole file after checking its size against a bound. The size check
/// happens before the buffer is reserved, so a file that claims to be enormous
/// cannot make this process allocate for it.
[[nodiscard]] Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                               std::uint64_t max_bytes);

/// Creates a new file and writes every byte into it. The create is exclusive:
/// a pre-existing file -- including one an attacker placed in advance -- is
/// never overwritten and never followed. A short write is an error, never a
/// silent truncation.
[[nodiscard]] Status write_new_file(const std::filesystem::path& path, std::span<const std::byte> bytes);

/// Opens an existing file for writing without truncating it and writes at the
/// given offset, extending the file when required.
[[nodiscard]] Status write_existing_file_at(const std::filesystem::path& path, std::uint64_t offset,
                                            std::span<const std::byte> bytes);

/// Flushes a file's contents to the storage device.
[[nodiscard]] Status sync_file(const std::filesystem::path& path);

/// Atomically replaces destination with source. On Windows this is MoveFileExW
/// with MOVEFILE_REPLACE_EXISTING and MOVEFILE_WRITE_THROUGH; on POSIX it is
/// rename(2) followed by a directory fsync. It does not return before the
/// replacement is durable.
[[nodiscard]] Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination);

/// Flushes a directory entry on POSIX (fsync on the directory). Windows has no
/// equivalent and the durability of a replacement comes from
/// MOVEFILE_WRITE_THROUGH, so there this reports success without doing
/// anything. The difference is documented in persistence.hpp.
[[nodiscard]] Status sync_directory(const std::filesystem::path& path);

[[nodiscard]] Result<std::uint64_t> file_size(const std::filesystem::path& path);
[[nodiscard]] bool path_exists(const std::filesystem::path& path) noexcept;
[[nodiscard]] Status remove_file(const std::filesystem::path& path) noexcept;
[[nodiscard]] Status ensure_directory(const std::filesystem::path& path);
/// Removes a directory only when it is empty. Never recursive: a store cleanup
/// must not be able to delete something it did not create.
[[nodiscard]] Status remove_empty_directory(const std::filesystem::path& path) noexcept;

/// A short unpredictable token used to name transient files. Combined with an
/// exclusive create, so unpredictability is defence in depth rather than the
/// primary protection.
[[nodiscard]] std::string random_token();

/// The current process identity, for diagnostics and for the writer record.
[[nodiscard]] std::uint64_t current_process_id() noexcept;

[[nodiscard]] std::string to_utf8(const std::filesystem::path& path);

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_SRC_FILE_OPS_HPP
