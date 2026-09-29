// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_SRC_STORE_HPP
#define FACILITYDRAIN_SRC_STORE_HPP

#include "coordinator_internal.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/persistence.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace facilitydrain {
namespace detail {

// ---------------------------------------------------------------------------
// Durable store layout, version 1
// ---------------------------------------------------------------------------
//
//   <root>/writer.lock                        exclusive lock, held for the session
//   <root>/CURRENT                            the pointer record, 128 bytes
//   <root>/gen-<20 digit sequence>.fdcdrain   a committed generation
//   <root>/tmp-<token>                        a transient publish file
//
// The pointer is the only authority. A generation file that CURRENT does not
// name is never adopted, never merged, and never interpreted.
//
// Container file = 128 byte header followed by the encoded payload:
//   0..7    magic "FDCDRN01"
//   8..11   u32 container format version, must be kContainerFormatVersion
//   12..15  u32 payload version, must be kStateFormatVersion
//   16..23  u64 commit sequence
//   24..31  u64 payload length
//   32..35  u32 crc32 of the payload bytes
//   36..39  u32 crc32 of this whole 128 byte header with this field zeroed
//   40..71  sha256 of the payload bytes
//   72..79  u64 flags, must be zero
//   80..127 reserved, must be all zero
//
// CURRENT record, 128 bytes:
//   0..7    magic "FDCDCUR1"
//   8..11   u32 store layout version, must be kStoreLayoutVersion
//   12..15  u32 reserved, must be zero
//   16..23  u64 commit sequence
//   24..55  sha256 over the whole container file (header and payload)
//   56..59  u32 crc32 of this whole 128 byte record with this field zeroed
//   60..127 reserved, must be all zero
//
// Publish sequence, in order, with a fault point after each step:
//   1. encode and validate the next state                      (kBeforeStageWrite)
//   2. create tmp-<token> exclusively with header and payload  (kAfterStageWriteBeforeSync)
//   3. flush the staged file to the device                     (kAfterSyncBeforePublish)
//   4. read the staged file back and verify header, length, crc32 and sha256;
//      a mismatch is kCommitFailed and the staged file is removed
//   5. atomically replace gen-<sequence> with the staged file  (kAfterPublishBeforePointer)
//   6. write the pointer record to tmp, flush it, atomically replace CURRENT
//                                                              (kAfterPointerBeforeFlush, kAfterPointerFlush)
//   7. prune generations outside the retention bound and remove transient files
// The in memory commit sequence is only considered durable after step 6.

struct StoreOpenOptions {
  std::filesystem::path root{};
  Limits limits{};
  PublishFaultHooks faults{};
  std::string writer_label{};
  bool create_if_missing = true;
  bool read_only = false;
};

/// One read write session over a store directory.
///
/// The session holds the writer lock for its whole lifetime. It never keeps the
/// state: the coordinator owns the state and hands it to publish, so there is
/// exactly one copy of the truth in the process.
class StoreSession {
 public:
  StoreSession() = default;
  ~StoreSession();

  StoreSession(StoreSession&& other) noexcept;
  StoreSession& operator=(StoreSession&& other) noexcept;
  StoreSession(const StoreSession&) = delete;
  StoreSession& operator=(const StoreSession&) = delete;

  /// Opens or creates the store, recovers exactly one generation into the state
  /// out parameter, and fills the recovery report.
  ///
  /// Rejections: a live writer holds the lock (kStoreLocked); there is no
  /// store here and create_if_missing is false (kStoreNotFound); CURRENT is
  /// malformed
  /// (kStoreCorrupt, kStoreChecksumMismatch, kStoreVersionUnsupported); the
  /// generation CURRENT names is missing (kMissingGenerationFile); the payload
  /// cannot be decoded (whatever decode_state returns); a generation file with
  /// a sequence above the published one exists (kStoreRecoveryFailed, because
  /// such a file means a publish did not complete and the outcome is not
  /// knowable from the bytes); the requested limits are below the stored ones
  /// (kLimitExceeded).
  [[nodiscard]] static Result<StoreSession> open(const StoreOpenOptions& options, RecoveryReport& report,
                                                 CoordinatorState& state);

  /// Publishes a whole new generation. Durable on return, or the previous
  /// generation is unchanged.
  [[nodiscard]] Status publish(const CoordinatorState& state);

  /// Flushes the published generation file and the store directory.
  [[nodiscard]] Status flush();

  [[nodiscard]] CommitSequence sequence() const noexcept { return CommitSequence{sequence_}; }
  [[nodiscard]] bool read_only() const noexcept { return read_only_; }
  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

 private:
  [[nodiscard]] Status write_generation(std::uint64_t sequence, std::span<const std::byte> container,
                                        const PublishFaultHooks& hooks);
  [[nodiscard]] Status write_pointer(std::uint64_t sequence, ContentDigest container_digest,
                                     const PublishFaultHooks& hooks);
  [[nodiscard]] Status prune(std::uint64_t published_sequence);

  std::filesystem::path root_{};
  Limits limits_{};
  PublishFaultHooks faults_{};
  std::uint64_t sequence_ = 0;
  bool read_only_ = false;
  bool open_ = false;
  void* lock_ = nullptr;
};

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_SRC_STORE_HPP
