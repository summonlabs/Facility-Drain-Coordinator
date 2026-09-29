// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_PERSISTENCE_HPP
#define FACILITYDRAIN_PERSISTENCE_HPP

#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/export.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Durable store
// ---------------------------------------------------------------------------
//
// A durable store is a directory containing one writer lock, one CURRENT
// pointer record, and a bounded number of committed generations. A mutation
// stages a whole new generation, flushes it, reads it back and verifies it,
// publishes it by atomically replacing CURRENT, and only then reports success.
// A reader therefore sees either the previous complete generation or the new
// complete generation, and never a mixture of the two.
//
// Recovery reads CURRENT, verifies the container it names, and adopts exactly
// that one generation. Anything unpublished is discarded, never merged.

/// A precise point at which a publish is made to fail or to kill the process.
///
/// The failure points return an error: they exercise the error paths in process.
/// The crash points call std::abort, which terminates the process immediately
/// without running destructors or flushing anything, which is what makes the
/// crash consistency claims real rather than simulated.
enum class FaultPoint : std::uint8_t {
  kNone = 0,
  kBeforeStageWrite = 1,
  kAfterStageWriteBeforeSync = 2,
  kAfterSyncBeforePublish = 3,
  kAfterPublishBeforePointer = 4,
  kAfterPointerBeforeFlush = 5,
  kAfterPointerFlush = 6,
};

[[nodiscard]] FACILITYDRAIN_API std::string_view to_token(FaultPoint point) noexcept;

struct PublishFaultHooks {
  /// Make the publish fail at this point, in process, returning an error.
  FaultPoint fail_at = FaultPoint::kNone;
  /// Terminate the process at this point, without cleanup.
  FaultPoint crash_at = FaultPoint::kNone;
  /// Truncate the staged container by this many bytes before publishing.
  std::uint32_t truncate_staged_bytes = 0;
  /// Flip one bit inside the staged container at this offset.
  bool corrupt_staged = false;
  std::uint64_t corrupt_staged_offset = 0;

  [[nodiscard]] bool active() const noexcept {
    return fail_at != FaultPoint::kNone || crash_at != FaultPoint::kNone || truncate_staged_bytes != 0 ||
           corrupt_staged;
  }
};

/// What a store open did. Reported once, and retained by the coordinator so the
/// caller can see exactly which generation was adopted and what was discarded.
struct RecoveryReport {
  bool created_new_store = false;
  bool opened_existing_store = false;
  bool recovered = false;
  /// The commit sequence of the generation that was adopted.
  CommitSequence recovered_sequence{};
  /// The control epoch the store was last written under, and the epoch this
  /// incarnation is now writing under. They differ by exactly one on a restart.
  ControlEpoch previous_epoch{};
  ControlEpoch current_epoch{};
  /// Transient publish files that were removed rather than interpreted.
  std::uint32_t transient_files_removed = 0;
  /// Generation files that exist but are not the published generation.
  std::uint32_t unpublished_generations = 0;
  /// Generation files removed because they fell outside the retention bound.
  std::uint32_t pruned_generations = 0;
  /// Grants that were live in the previous incarnation and are fenced now.
  std::uint32_t grants_fenced = 0;
  /// Plans whose derived state changed because a grant was fenced.
  std::uint32_t plans_reopened = 0;
  std::string detail{};
};

/// Read only inspection of a store directory, without taking the writer lock
/// and without writing anything.
struct StoreInspection {
  bool exists = false;
  bool has_pointer = false;
  CommitSequence sequence{};
  ControlEpoch epoch{};
  ContentDigest state_digest{};
  std::uint64_t generation_files = 0;
  std::uint64_t transient_files = 0;
  std::uint64_t bytes = 0;
  bool writer_lock_present = false;
  std::string writer_lock_record{};
};

[[nodiscard]] FACILITYDRAIN_API Result<StoreInspection> inspect_store(const std::filesystem::path& root,
                                                                      const Limits& limits);

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_PERSISTENCE_HPP
