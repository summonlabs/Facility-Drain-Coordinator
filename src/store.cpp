// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "store.hpp"

#include "crc32.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/version.hpp"
#include "file_lock.hpp"
#include "file_ops.hpp"
#include "state_codec.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace facilitydrain {

std::string_view to_token(FaultPoint point) noexcept {
  switch (point) {
    case FaultPoint::kNone:
      return "none";
    case FaultPoint::kBeforeStageWrite:
      return "before-stage-write";
    case FaultPoint::kAfterStageWriteBeforeSync:
      return "after-stage-write-before-sync";
    case FaultPoint::kAfterSyncBeforePublish:
      return "after-sync-before-publish";
    case FaultPoint::kAfterPublishBeforePointer:
      return "after-publish-before-pointer";
    case FaultPoint::kAfterPointerBeforeFlush:
      return "after-pointer-before-flush";
    case FaultPoint::kAfterPointerFlush:
      return "after-pointer-flush";
  }
  return "none";
}

namespace detail {
namespace {

// ---------------------------------------------------------------------------
// Container and pointer field offsets
// ---------------------------------------------------------------------------
//
// These are the offsets documented in store.hpp. They are named constants
// rather than comments because a header whose layout is described only in prose
// is one transcription slip away from being unreadable by the next build.

constexpr std::size_t kContainerMagicOffset = 0;
constexpr std::size_t kContainerFormatOffset = 8;
constexpr std::size_t kContainerPayloadVersionOffset = 12;
constexpr std::size_t kContainerSequenceOffset = 16;
constexpr std::size_t kContainerPayloadLengthOffset = 24;
constexpr std::size_t kContainerPayloadCrcOffset = 32;
constexpr std::size_t kContainerHeaderCrcOffset = 36;
constexpr std::size_t kContainerPayloadDigestOffset = 40;
constexpr std::size_t kContainerFlagsOffset = 72;
constexpr std::size_t kContainerReservedOffset = 80;
constexpr std::size_t kContainerReservedBytes = 48;

constexpr std::size_t kPointerMagicOffset = 0;
constexpr std::size_t kPointerLayoutOffset = 8;
constexpr std::size_t kPointerReservedOffset = 12;
constexpr std::size_t kPointerSequenceOffset = 16;
constexpr std::size_t kPointerDigestOffset = 24;
constexpr std::size_t kPointerCrcOffset = 56;
constexpr std::size_t kPointerTailOffset = 60;
constexpr std::size_t kPointerTailBytes = 68;

/// Both fixed records are 128 bytes, which is why one checksum helper serves
/// both of them.
constexpr std::size_t kFixedRecordBytes = 128;

/// Twenty digits is exactly the width of a u64, so a zero padded sequence is
/// never ambiguous and a name that does not parse is never a generation file.
constexpr std::size_t kSequenceDigits = 20;

// ---------------------------------------------------------------------------
// Fixed width little endian field access
// ---------------------------------------------------------------------------

void put_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
  }
}

void put_u64(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
  }
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

void put_digest(std::span<std::byte> bytes, std::size_t offset, const ContentDigest& digest) noexcept {
  const std::span<const std::byte, ContentDigest::kSize> source = digest.bytes();
  for (std::size_t index = 0; index < ContentDigest::kSize; ++index) {
    bytes[offset + index] = source[index];
  }
}

[[nodiscard]] ContentDigest get_digest(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  std::array<std::byte, ContentDigest::kSize> value{};
  for (std::size_t index = 0; index < ContentDigest::kSize; ++index) {
    value[index] = bytes[offset + index];
  }
  return ContentDigest::from_bytes(value);
}

[[nodiscard]] bool is_zero_range(std::span<const std::byte> bytes, std::size_t offset,
                                 std::size_t count) noexcept {
  for (std::size_t index = 0; index < count; ++index) {
    if (bytes[offset + index] != std::byte{0}) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool magic_matches(std::span<const std::byte> bytes, std::size_t offset,
                                 std::string_view magic) noexcept {
  for (std::size_t index = 0; index < magic.size(); ++index) {
    const auto expected = static_cast<std::uint8_t>(magic[index]);
    if (std::to_integer<std::uint8_t>(bytes[offset + index]) != expected) {
      return false;
    }
  }
  return true;
}

/// True when the stored checksum equals the checksum of the whole record with
/// the checksum field itself zeroed, which is what a checksum that covers its
/// own record has to mean.
[[nodiscard]] bool record_checksum_matches(std::span<const std::byte> record, std::size_t checksum_offset) noexcept {
  const std::uint32_t stored = get_u32(record, checksum_offset);
  std::array<std::byte, kFixedRecordBytes> copy{};
  std::copy(record.begin(), record.end(), copy.begin());
  put_u32(copy, checksum_offset, 0U);
  return crc32(copy) == stored;
}

// ---------------------------------------------------------------------------
// Failure text
// ---------------------------------------------------------------------------

/// Every store failure names the operation and the path, so an operator knows
/// which file inside which directory went wrong without a debugger.
[[nodiscard]] std::string store_detail(std::string_view operation, const std::filesystem::path& path,
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

[[nodiscard]] Status store_failure(ErrorCode code, std::string_view operation,
                                   const std::filesystem::path& path, std::string_view reason) {
  return Status::failure(Error{code, store_detail(operation, path, reason)});
}

template <class T>
[[nodiscard]] Result<T> store_failure_result(ErrorCode code, std::string_view operation,
                                             const std::filesystem::path& path, std::string_view reason) {
  return make_error<T>(code, store_detail(operation, path, reason));
}

/// The one code a publish returns when it could not complete. The step and the
/// underlying cause stay in the detail: a caller decides what to do from the
/// code, and an operator diagnoses from the text.
[[nodiscard]] Status commit_failed(std::string_view step, std::string_view reason,
                                   const std::filesystem::path& path) {
  return store_failure(ErrorCode::kCommitFailed, "publish", path, std::string{step} + ": " + std::string{reason});
}

/// Honours the fault latch at one publish point.
///
/// A crash point terminates the process for real. This is not a simulation: no
/// destructor runs, nothing is flushed, and the store is left exactly as the
/// steps performed so far made it, which is what makes the crash consistency
/// tests meaningful. A point configured both to fail and to crash crashes,
/// because a dead process cannot report an error.
[[nodiscard]] bool fault_should_fail(const PublishFaultHooks& hooks, FaultPoint point,
                                     const std::filesystem::path& path, Status& failure) {
  if (hooks.crash_at == point) {
    std::abort();
  }
  if (hooks.fail_at == point) {
    failure = commit_failed("the publish fault latch is set", std::string{"at "} + std::string{to_token(point)},
                            path);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

[[nodiscard]] std::string format_sequence(std::uint64_t sequence) {
  std::string digits(kSequenceDigits, '0');
  for (std::size_t index = kSequenceDigits; index > 0; --index) {
    digits[index - 1] = static_cast<char>('0' + static_cast<int>(sequence % 10U));
    sequence /= 10U;
  }
  return digits;
}

[[nodiscard]] std::string generation_file_name(std::uint64_t sequence) {
  std::string name{kGenerationFilePrefix};
  name.append(format_sequence(sequence));
  name.append(kGenerationFileSuffix);
  return name;
}

/// Parses a generation file name strictly: the prefix, exactly twenty decimal
/// digits and the suffix, with nothing before or after. A name that does not
/// match is not a generation file at all and is never interpreted as one.
[[nodiscard]] bool parse_generation_file_name(std::string_view name, std::uint64_t& sequence) noexcept {
  const std::size_t prefix_bytes = kGenerationFilePrefix.size();
  const std::size_t suffix_bytes = kGenerationFileSuffix.size();
  if (name.size() != prefix_bytes + kSequenceDigits + suffix_bytes) {
    return false;
  }
  if (name.substr(0, prefix_bytes) != kGenerationFilePrefix) {
    return false;
  }
  if (name.substr(prefix_bytes + kSequenceDigits) != kGenerationFileSuffix) {
    return false;
  }
  const std::string_view digits = name.substr(prefix_bytes, kSequenceDigits);
  std::uint64_t value = 0;
  const std::from_chars_result parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  // Twenty digits can describe a value larger than a u64; from_chars reports
  // that as an error rather than wrapping, and the name is then not a
  // generation file.
  if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
    return false;
  }
  sequence = value;
  return true;
}

[[nodiscard]] std::string join_names(const std::vector<std::string>& names) {
  std::string joined;
  const std::size_t limit = 8U;
  for (std::size_t index = 0; index < names.size() && index < limit; ++index) {
    if (index != 0U) {
      joined.append(", ");
    }
    joined.append(names[index]);
  }
  if (names.size() > limit) {
    joined.append(", ...");
  }
  return joined;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

struct ContainerView {
  std::uint64_t sequence = 0;
  std::span<const std::byte> payload{};
};

struct PointerView {
  std::uint64_t sequence = 0;
  ContentDigest container_digest{};
};

void write_container_header(std::span<std::byte> header, std::uint64_t sequence,
                            std::span<const std::byte> payload, const ContentDigest& payload_digest) noexcept {
  std::fill(header.begin(), header.end(), std::byte{0});
  for (std::size_t index = 0; index < kContainerMagic.size(); ++index) {
    header[kContainerMagicOffset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>(kContainerMagic[index]));
  }
  put_u32(header, kContainerFormatOffset, kContainerFormatVersion);
  put_u32(header, kContainerPayloadVersionOffset, kStateFormatVersion);
  put_u64(header, kContainerSequenceOffset, sequence);
  put_u64(header, kContainerPayloadLengthOffset, static_cast<std::uint64_t>(payload.size()));
  put_u32(header, kContainerPayloadCrcOffset, crc32(payload));
  put_digest(header, kContainerPayloadDigestOffset, payload_digest);
  // The flags and reserved fields stay zero. The header checksum is written
  // last because it covers the whole header with its own field zeroed, so it
  // commits to every field above, including the sequence and the payload
  // length, and a flipped bit anywhere in the header is caught before any field
  // is believed.
  put_u32(header, kContainerHeaderCrcOffset, crc32(header));
}

void write_pointer_record(std::span<std::byte> record, std::uint64_t sequence,
                          const ContentDigest& container_digest) noexcept {
  std::fill(record.begin(), record.end(), std::byte{0});
  for (std::size_t index = 0; index < kPointerMagic.size(); ++index) {
    record[kPointerMagicOffset + index] = static_cast<std::byte>(static_cast<std::uint8_t>(kPointerMagic[index]));
  }
  put_u32(record, kPointerLayoutOffset, kStoreLayoutVersion);
  put_u32(record, kPointerReservedOffset, 0U);
  put_u64(record, kPointerSequenceOffset, sequence);
  put_digest(record, kPointerDigestOffset, container_digest);
  put_u32(record, kPointerCrcOffset, crc32(record));
}

/// Structural checks run in a fixed order from cheapest and most specific to
/// most expensive, and the checksum comes before any field is interpreted: a
/// corrupted header is reported as a checksum mismatch rather than as whatever
/// the corrupted field happens to look like.
[[nodiscard]] Status verify_container(std::span<const std::byte> file, std::uint64_t expected_sequence,
                                      const std::filesystem::path& path, ContainerView& view) {
  const std::size_t header_bytes = static_cast<std::size_t>(kContainerHeaderSize);
  if (file.size() < header_bytes) {
    return store_failure(ErrorCode::kStoreTruncated, "verify_container", path,
                         "the container is " + std::to_string(file.size()) + " bytes, below the " +
                             std::to_string(header_bytes) + " byte header");
  }
  const std::span<const std::byte> header = file.first(header_bytes);
  if (!magic_matches(header, kContainerMagicOffset, kContainerMagic)) {
    return store_failure(ErrorCode::kStoreCorrupt, "verify_container", path,
                         "the container magic is not the expected eight bytes");
  }
  if (get_u32(header, kContainerFormatOffset) != kContainerFormatVersion) {
    return store_failure(ErrorCode::kStoreVersionUnsupported, "verify_container", path,
                         "the container format version " +
                             std::to_string(get_u32(header, kContainerFormatOffset)) +
                             " is not the supported version " + std::to_string(kContainerFormatVersion));
  }
  if (get_u32(header, kContainerPayloadVersionOffset) != kStateFormatVersion) {
    return store_failure(ErrorCode::kStoreVersionUnsupported, "verify_container", path,
                         "the payload version " + std::to_string(get_u32(header, kContainerPayloadVersionOffset)) +
                             " is not the supported version " + std::to_string(kStateFormatVersion));
  }
  if (!record_checksum_matches(header, kContainerHeaderCrcOffset)) {
    return store_failure(ErrorCode::kStoreChecksumMismatch, "verify_container", path,
                         "the header checksum does not match the header bytes");
  }
  if (get_u64(header, kContainerFlagsOffset) != 0U) {
    return store_failure(ErrorCode::kReservedNotZero, "verify_container", path,
                         "the container flags field is not zero");
  }
  if (!is_zero_range(header, kContainerReservedOffset, kContainerReservedBytes)) {
    return store_failure(ErrorCode::kReservedNotZero, "verify_container", path,
                         "the container reserved bytes are not all zero");
  }
  const std::uint64_t sequence = get_u64(header, kContainerSequenceOffset);
  if (sequence != expected_sequence) {
    return store_failure(ErrorCode::kStoreCorrupt, "verify_container", path,
                         "the container carries sequence " + std::to_string(sequence) +
                             " but was published as sequence " + std::to_string(expected_sequence));
  }
  const std::uint64_t payload_length = get_u64(header, kContainerPayloadLengthOffset);
  const std::uint64_t available = static_cast<std::uint64_t>(file.size() - header_bytes);
  if (payload_length > available) {
    return store_failure(ErrorCode::kStoreTruncated, "verify_container", path,
                         "the header declares a payload of " + std::to_string(payload_length) +
                             " bytes but only " + std::to_string(available) + " bytes follow it");
  }
  if (payload_length < available) {
    return store_failure(ErrorCode::kStoreTrailingBytes, "verify_container", path,
                         "the header declares a payload of " + std::to_string(payload_length) +
                             " bytes and " + std::to_string(available) + " bytes follow it");
  }
  const std::span<const std::byte> payload = file.subspan(header_bytes);
  if (crc32(payload) != get_u32(header, kContainerPayloadCrcOffset)) {
    return store_failure(ErrorCode::kStoreChecksumMismatch, "verify_container", path,
                         "the payload checksum does not match the payload bytes");
  }
  if (digest_bytes(payload) != get_digest(header, kContainerPayloadDigestOffset)) {
    return store_failure(ErrorCode::kStoreChecksumMismatch, "verify_container", path,
                         "the payload digest does not match the payload bytes");
  }
  view.sequence = sequence;
  view.payload = payload;
  return Status::success();
}

[[nodiscard]] Status verify_pointer_record(std::span<const std::byte> record, const std::filesystem::path& path,
                                           PointerView& view) {
  if (record.size() < static_cast<std::size_t>(kPointerRecordSize)) {
    return store_failure(ErrorCode::kStoreTruncated, "verify_pointer_record", path,
                         "the pointer record is " + std::to_string(record.size()) + " bytes, and the record is " +
                             std::to_string(kPointerRecordSize) + " bytes");
  }
  if (record.size() > static_cast<std::size_t>(kPointerRecordSize)) {
    return store_failure(ErrorCode::kStoreTrailingBytes, "verify_pointer_record", path,
                         "the pointer record is " + std::to_string(record.size()) + " bytes, and the record is " +
                             std::to_string(kPointerRecordSize) + " bytes");
  }
  if (!magic_matches(record, kPointerMagicOffset, kPointerMagic)) {
    return store_failure(ErrorCode::kStoreCorrupt, "verify_pointer_record", path,
                         "the pointer magic is not the expected eight bytes");
  }
  if (get_u32(record, kPointerLayoutOffset) != kStoreLayoutVersion) {
    return store_failure(ErrorCode::kStoreVersionUnsupported, "verify_pointer_record", path,
                         "the store layout version " + std::to_string(get_u32(record, kPointerLayoutOffset)) +
                             " is not the supported version " + std::to_string(kStoreLayoutVersion));
  }
  if (get_u32(record, kPointerReservedOffset) != 0U) {
    return store_failure(ErrorCode::kReservedNotZero, "verify_pointer_record", path,
                         "the pointer reserved field is not zero");
  }
  if (!record_checksum_matches(record, kPointerCrcOffset)) {
    return store_failure(ErrorCode::kStoreChecksumMismatch, "verify_pointer_record", path,
                         "the pointer checksum does not match the pointer bytes");
  }
  if (!is_zero_range(record, kPointerTailOffset, kPointerTailBytes)) {
    return store_failure(ErrorCode::kReservedNotZero, "verify_pointer_record", path,
                         "the pointer reserved tail is not all zero");
  }
  view.sequence = get_u64(record, kPointerSequenceOffset);
  view.container_digest = get_digest(record, kPointerDigestOffset);
  return Status::success();
}

// ---------------------------------------------------------------------------
// Directory scanning
// ---------------------------------------------------------------------------

struct GenerationEntry {
  std::uint64_t sequence = 0;
  std::filesystem::path path{};
  std::uint64_t bytes = 0;
};

struct DirectoryScan {
  /// Sorted by sequence, so no caller depends on the unspecified order a
  /// directory iterator returns.
  std::vector<GenerationEntry> generations{};
  std::vector<std::filesystem::path> transient_files{};
  std::uint64_t transient_bytes = 0;
  bool writer_lock_present = false;
};

[[nodiscard]] Result<DirectoryScan> scan_directory(const std::filesystem::path& root) {
  DirectoryScan scan;
  std::error_code error;
  std::filesystem::directory_iterator iterator{root, error};
  if (error) {
    return store_failure_result<DirectoryScan>(ErrorCode::kStoreIoError, "scan_directory", root,
                                               "the directory could not be listed: " + error.message());
  }
  const std::filesystem::directory_iterator end{};
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      return store_failure_result<DirectoryScan>(ErrorCode::kStoreIoError, "scan_directory", root,
                                                 "the directory listing failed: " + error.message());
    }
    const std::filesystem::path path = iterator->path();
    const std::string name = to_utf8(path.filename());
    std::uint64_t sequence = 0;
    if (parse_generation_file_name(name, sequence)) {
      // Qualified on purpose: an unqualified call would also find
      // std::filesystem::file_size, which argument dependent lookup considers
      // for a std::filesystem::path argument.
      const Result<std::uint64_t> bytes = detail::file_size(path);
      if (!bytes) {
        return Result<DirectoryScan>{bytes.error()};
      }
      scan.generations.push_back(GenerationEntry{sequence, path, bytes.value()});
      continue;
    }
    if (name.starts_with(kTransientFilePrefix)) {
      scan.transient_files.push_back(path);
      const Result<std::uint64_t> bytes = detail::file_size(path);
      if (bytes) {
        // A transient entry that is not a readable file -- a directory, for
        // instance -- still counts as a transient entry; it is reported by name
        // and removed as such.
        scan.transient_bytes += bytes.value();
      }
      continue;
    }
    if (name == kWriterLockFileName) {
      scan.writer_lock_present = true;
      continue;
    }
    // Anything else belongs to whoever else uses the directory. It is never
    // interpreted, never counted as a generation and never treated as evidence
    // about this store: the pointer record is the only authority, so the entry
    // is simply none of this store's business.
  }
  std::sort(scan.generations.begin(), scan.generations.end(),
            [](const GenerationEntry& lhs, const GenerationEntry& rhs) { return lhs.sequence < rhs.sequence; });
  return scan;
}

// ---------------------------------------------------------------------------
// Identity, records and limits
// ---------------------------------------------------------------------------

/// A fresh, never zero incarnation. The material is the process identity, the
/// clock reading and a random token, so two incarnations differ even inside one
/// process and a restart can never reuse the identity it had before.
[[nodiscard]] IncarnationId make_incarnation() {
  const std::int64_t milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
  const std::string material = "facilitydrain-incarnation-v1|" + std::to_string(current_process_id()) + "|" +
                               std::to_string(milliseconds) + "|" + random_token();
  const ContentDigest digest = digest_text(material);
  const std::span<const std::byte, ContentDigest::kSize> bytes = digest.bytes();
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index])) << (index * 8U);
  }
  // Zero means "no identity" everywhere in this library, so a digest that
  // starts with eight zero bytes is folded to one rather than left to look
  // unset.
  return IncarnationId{value == 0 ? 1U : value};
}

/// The record left in the lock file: who holds the store, in one line.
[[nodiscard]] std::string writer_record(std::string_view label) {
  std::string record = "pid=" + std::to_string(current_process_id());
  if (label.empty()) {
    return record;
  }
  record.append(" label=");
  for (const char character : label) {
    const auto byte = static_cast<unsigned char>(character);
    // The record is read back by an operator and by read_lock_record, so it
    // stays exactly one line: a label carrying control characters is folded
    // rather than allowed to forge a second line.
    record.push_back(byte < 0x20U || byte == 0x7FU ? '?' : character);
  }
  return record;
}

/// The requested limits must be at least the stored ones for every bound: a
/// store reopened with a smaller bound would have to reinterpret payloads that
/// were written to be legal under the larger one.
[[nodiscard]] Status check_requested_limits(const Limits& requested, const Limits& stored,
                                            const std::filesystem::path& root) {
  struct Comparison {
    std::string_view field;
    std::uint64_t requested;
    std::uint64_t stored;
  };
  const std::array<Comparison, 12> comparisons{{
      {"max_plans", requested.max_plans, stored.max_plans},
      {"max_targets_per_plan", requested.max_targets_per_plan, stored.max_targets_per_plan},
      {"max_consumers_per_domain", requested.max_consumers_per_domain, stored.max_consumers_per_domain},
      {"max_evidence_per_domain", requested.max_evidence_per_domain, stored.max_evidence_per_domain},
      {"max_requests_per_plan", requested.max_requests_per_plan, stored.max_requests_per_plan},
      {"max_residuals_per_plan", requested.max_residuals_per_plan, stored.max_residuals_per_plan},
      {"max_history_per_plan", requested.max_history_per_plan, stored.max_history_per_plan},
      {"max_text_bytes", requested.max_text_bytes, stored.max_text_bytes},
      {"max_annotation_bytes", requested.max_annotation_bytes, stored.max_annotation_bytes},
      {"max_state_bytes", requested.max_state_bytes, stored.max_state_bytes},
      {"max_document_bytes", requested.max_document_bytes, stored.max_document_bytes},
      {"max_attempts_per_key", requested.max_attempts_per_key, stored.max_attempts_per_key},
  }};
  for (const Comparison& comparison : comparisons) {
    if (comparison.requested < comparison.stored) {
      return store_failure(ErrorCode::kLimitExceeded, "StoreSession::open: the requested limits", root,
                           "field " + std::string{comparison.field} + " is " +
                               std::to_string(comparison.requested) + ", below the stored " +
                               std::to_string(comparison.stored));
    }
  }
  return Status::success();
}

/// The oldest generation the retention bound keeps when `published_sequence`
/// is the published one: the published generation and the ones immediately
/// below it, so recent history stays inspectable and the store still cannot
/// grow without bound.
[[nodiscard]] std::uint64_t oldest_retained_generation(std::uint64_t published_sequence) noexcept {
  constexpr std::uint64_t kRetained = kRetainedGenerations;
  return published_sequence >= kRetained - 1U ? published_sequence - (kRetained - 1U) : 0U;
}

/// Reads a whole file whose size has already been established, so the read is
/// bounded by exactly that size and a file that grew underneath the reader is
/// rejected instead of being followed.
[[nodiscard]] Result<std::vector<std::byte>> read_sized_file(const std::filesystem::path& path,
                                                             std::uint64_t size) {
  return read_file_bounded(path, size);
}

// ---------------------------------------------------------------------------
// Publish steps
// ---------------------------------------------------------------------------

/// Writes and publishes the staged generation, in the documented order:
/// create, flush, read back and verify, then atomically replace the generation
/// file. The caller removes the staged file when this fails.
[[nodiscard]] Status stage_generation(const std::filesystem::path& staged, std::uint64_t sequence,
                                      std::span<const std::byte> container, const PublishFaultHooks& hooks) {
  Status fault;
  if (fault_should_fail(hooks, FaultPoint::kBeforeStageWrite, staged, fault)) {
    return fault;
  }
  const Status written = write_new_file(staged, container);
  if (!written.ok()) {
    return commit_failed("staging the generation failed", written.error().detail(), staged);
  }

  // The mutation hooks change the staged file after it has been written and
  // before it is read back. They exist to prove that the read back verification
  // is what stands between a damaged generation and a published one.
  std::size_t staged_bytes = container.size();
  if (hooks.truncate_staged_bytes != 0U) {
    staged_bytes = container.size() > hooks.truncate_staged_bytes
                       ? container.size() - static_cast<std::size_t>(hooks.truncate_staged_bytes)
                       : 0U;
    // file_ops writes files whole and never edits them in place, so the
    // shortened file is written under the same name with the same exclusive
    // create: the bytes a truncate would have left are exactly these, and the
    // verification below cannot tell the difference -- which is the point.
    const Status removed = remove_file(staged);
    if (!removed.ok()) {
      return removed;
    }
    const Status rewritten = write_new_file(staged, container.first(staged_bytes));
    if (!rewritten.ok()) {
      return commit_failed("truncating the staged generation failed", rewritten.error().detail(), staged);
    }
  }
  if (hooks.corrupt_staged) {
    if (hooks.corrupt_staged_offset >= static_cast<std::uint64_t>(staged_bytes)) {
      return commit_failed("the corruption hook names an offset beyond the staged generation",
                           "offset " + std::to_string(hooks.corrupt_staged_offset) + " of " +
                               std::to_string(staged_bytes) + " bytes",
                           staged);
    }
    const std::byte flipped =
        container[static_cast<std::size_t>(hooks.corrupt_staged_offset)] ^ std::byte{0x01};
    const Status corrupted =
        write_existing_file_at(staged, hooks.corrupt_staged_offset, std::span<const std::byte>{&flipped, 1U});
    if (!corrupted.ok()) {
      return commit_failed("corrupting the staged generation failed", corrupted.error().detail(), staged);
    }
  }

  if (fault_should_fail(hooks, FaultPoint::kAfterStageWriteBeforeSync, staged, fault)) {
    return fault;
  }

  const Status synced = sync_file(staged);
  if (!synced.ok()) {
    return commit_failed("flushing the staged generation failed", synced.error().detail(), staged);
  }

  if (fault_should_fail(hooks, FaultPoint::kAfterSyncBeforePublish, staged, fault)) {
    return fault;
  }

  // Read the staged file back and verify the header, the length against the
  // file, the payload checksum and the payload digest. A generation that does
  // not survive this is never published, whatever the storage device did.
  const Result<std::uint64_t> staged_size = detail::file_size(staged);
  if (!staged_size) {
    return commit_failed("reading the staged generation failed", staged_size.error().detail(), staged);
  }
  const Result<std::vector<std::byte>> read_back = read_sized_file(staged, staged_size.value());
  if (!read_back) {
    return commit_failed("reading the staged generation failed", read_back.error().detail(), staged);
  }
  ContainerView view;
  const Status verified = verify_container(read_back.value(), sequence, staged, view);
  if (!verified.ok()) {
    return commit_failed("the staged generation did not verify after it was written",
                         verified.error().detail(), staged);
  }

  // The replacement is atomic and, on Windows, write through, so the generation
  // file is durable the moment this returns.
  const std::filesystem::path target = staged.parent_path() / generation_file_name(sequence);
  const Status replaced = replace_file(staged, target);
  if (!replaced.ok()) {
    return commit_failed("publishing the generation failed", replaced.error().detail(), staged);
  }

  if (fault_should_fail(hooks, FaultPoint::kAfterPublishBeforePointer, target, fault)) {
    return fault;
  }
  return Status::success();
}

/// Writes the pointer record, flushes it, and atomically replaces CURRENT with
/// it. The caller removes the staged file when this fails.
[[nodiscard]] Status stage_pointer(const std::filesystem::path& staged, std::uint64_t sequence,
                                   ContentDigest container_digest, const PublishFaultHooks& hooks) {
  std::array<std::byte, kFixedRecordBytes> record{};
  write_pointer_record(record, sequence, container_digest);
  const Status written = write_new_file(staged, record);
  if (!written.ok()) {
    return commit_failed("writing the pointer record failed", written.error().detail(), staged);
  }

  Status fault;
  if (fault_should_fail(hooks, FaultPoint::kAfterPointerBeforeFlush, staged, fault)) {
    return fault;
  }

  const Status synced = sync_file(staged);
  if (!synced.ok()) {
    return commit_failed("flushing the pointer record failed", synced.error().detail(), staged);
  }

  const std::filesystem::path pointer_path = staged.parent_path() / std::string(kPointerFileName);
  const Status replaced = replace_file(staged, pointer_path);
  if (!replaced.ok()) {
    return commit_failed("replacing the pointer record failed", replaced.error().detail(), pointer_path);
  }

  const Status directory_synced = sync_directory(staged.parent_path());
  if (!directory_synced.ok()) {
    return commit_failed("flushing the store directory failed", directory_synced.error().detail(),
                         staged.parent_path());
  }

  if (fault_should_fail(hooks, FaultPoint::kAfterPointerFlush, pointer_path, fault)) {
    return fault;
  }
  return Status::success();
}

/// A staged file is never authoritative, so every failure after it was created
/// removes it. An open removes any that a crash left behind.
void discard_staged(const std::filesystem::path& staged) noexcept {
  static_cast<void>(remove_file(staged));
}

}  // namespace

// ---------------------------------------------------------------------------
// StoreSession
// ---------------------------------------------------------------------------

StoreSession::~StoreSession() {
  if (lock_ != nullptr) {
    delete static_cast<FileLock*>(lock_);
    lock_ = nullptr;
  }
}

StoreSession::StoreSession(StoreSession&& other) noexcept
    : root_(std::move(other.root_)),
      limits_(other.limits_),
      faults_(other.faults_),
      sequence_(other.sequence_),
      read_only_(other.read_only_),
      open_(other.open_),
      lock_(other.lock_) {
  other.lock_ = nullptr;
  other.open_ = false;
}

StoreSession& StoreSession::operator=(StoreSession&& other) noexcept {
  if (this != &other) {
    if (lock_ != nullptr) {
      delete static_cast<FileLock*>(lock_);
    }
    root_ = std::move(other.root_);
    limits_ = other.limits_;
    faults_ = other.faults_;
    sequence_ = other.sequence_;
    read_only_ = other.read_only_;
    open_ = other.open_;
    lock_ = other.lock_;
    other.lock_ = nullptr;
    other.open_ = false;
  }
  return *this;
}

Result<StoreSession> StoreSession::open(const StoreOpenOptions& options, RecoveryReport& report,
                                        CoordinatorState& state) {
  try {
    const Status limits_status = options.limits.validate();
    if (!limits_status.ok()) {
      return Result<StoreSession>{limits_status.error()};
    }
    if (options.root.empty()) {
      return make_error<StoreSession>(ErrorCode::kStorePathInvalid,
                                      "StoreSession::open: the store root is empty");
    }

    StoreSession session;
    session.root_ = options.root;
    session.limits_ = options.limits;
    session.faults_ = options.faults;
    session.read_only_ = options.read_only;

    const std::filesystem::path root = options.root;
    if (!path_exists(root)) {
      if (options.read_only || !options.create_if_missing) {
        return store_failure_result<StoreSession>(ErrorCode::kStoreNotFound, "StoreSession::open", root,
                                                  "the store directory does not exist");
      }
      const Status created = ensure_directory(root);
      if (!created.ok()) {
        return Result<StoreSession>{created.error()};
      }
    } else {
      std::error_code error;
      if (!std::filesystem::is_directory(root, error)) {
        return store_failure_result<StoreSession>(ErrorCode::kStorePathInvalid, "StoreSession::open", root,
                                                  "the store root exists and is not a directory");
      }
    }

    // The writer lock is taken before anything is read or removed, and it is
    // held for the whole session. A read only session never takes it, which is
    // what lets an operator inspect a store while a writer is working.
    if (!options.read_only) {
      Result<FileLock> lock =
          FileLock::acquire(root / std::string(kWriterLockFileName), writer_record(options.writer_label));
      if (!lock) {
        return Result<StoreSession>{lock.error()};
      }
      session.lock_ = new FileLock(std::move(lock).value());
    }

    const Result<DirectoryScan> scan = scan_directory(root);
    if (!scan) {
      return Result<StoreSession>{scan.error()};
    }

    if (!options.read_only) {
      // A transient file is never authoritative: it is removed rather than
      // interpreted, and it is counted so the caller can see that a previous
      // attempt left something behind.
      for (const std::filesystem::path& transient : scan.value().transient_files) {
        const Status removed = remove_file(transient);
        if (!removed.ok()) {
          return Result<StoreSession>{removed.error()};
        }
        ++report.transient_files_removed;
      }
    }

    const std::filesystem::path pointer_path = root / std::string(kPointerFileName);
    if (!path_exists(pointer_path)) {
      if (options.read_only) {
        return store_failure_result<StoreSession>(ErrorCode::kStoreNotFound, "StoreSession::open", root,
                                                  "a read only session needs a published store, and there is no "
                                                  "CURRENT pointer record");
      }
      if (!scan.value().generations.empty()) {
        std::vector<std::string> names;
        names.reserve(scan.value().generations.size());
        for (const GenerationEntry& entry : scan.value().generations) {
          names.push_back(generation_file_name(entry.sequence));
        }
        return store_failure_result<StoreSession>(
            ErrorCode::kStoreRecoveryFailed, "StoreSession::open", root,
            "there is no CURRENT pointer record but " + std::to_string(names.size()) +
                " generation file(s) exist, so no published generation can be adopted: " + join_names(names));
      }
      if (!options.create_if_missing) {
        return store_failure_result<StoreSession>(ErrorCode::kStoreNotFound, "StoreSession::open", root,
                                                  "there is no store here and create_if_missing is false");
      }
      // A new store: an empty state under control epoch 1, with an identity
      // that is fresh by construction. Nothing is written yet; the first
      // publish creates the first generation.
      report.created_new_store = true;
      report.opened_existing_store = false;
      report.recovered = false;
      report.recovered_sequence = CommitSequence{0};
      report.previous_epoch = ControlEpoch{0};
      report.current_epoch = ControlEpoch{1};
      report.detail = "created a new store";
      state = CoordinatorState{};
      state.payload_version = kStateFormatVersion;
      state.limits = options.limits;
      state.control_epoch = ControlEpoch{1};
      state.incarnation = make_incarnation();
      state.commit_sequence = CommitSequence{0};
      state.observation_sequence = ObservationSequence{0};
      session.open_ = true;
      return std::move(session);
    }

    const Result<std::uint64_t> pointer_size = detail::file_size(pointer_path);
    if (!pointer_size) {
      return Result<StoreSession>{pointer_size.error()};
    }
    if (pointer_size.value() != kPointerRecordSize) {
      const ErrorCode code = pointer_size.value() < kPointerRecordSize ? ErrorCode::kStoreTruncated
                                                                       : ErrorCode::kStoreTrailingBytes;
      return store_failure_result<StoreSession>(code, "StoreSession::open: the pointer record", pointer_path,
                                                "the record is " + std::to_string(pointer_size.value()) +
                                                    " bytes, and the record is " +
                                                    std::to_string(kPointerRecordSize) + " bytes");
    }
    const Result<std::vector<std::byte>> pointer_bytes = read_sized_file(pointer_path, pointer_size.value());
    if (!pointer_bytes) {
      return Result<StoreSession>{pointer_bytes.error()};
    }
    PointerView pointer;
    const Status pointer_status =
        verify_pointer_record(pointer_bytes.value(), pointer_path, pointer);
    if (!pointer_status.ok()) {
      return Result<StoreSession>{pointer_status.error()};
    }

    const std::filesystem::path generation_path = root / generation_file_name(pointer.sequence);
    if (!path_exists(generation_path)) {
      return store_failure_result<StoreSession>(
          ErrorCode::kMissingGenerationFile, "StoreSession::open", generation_path,
          "the pointer record names sequence " + std::to_string(pointer.sequence) +
              " and that generation file does not exist");
    }

    // A generation above the published one means a publish wrote the generation
    // and did not replace the pointer. Whether that generation was meant to be
    // adopted is not knowable from the bytes, so the store is refused rather
    // than guessed at.
    std::vector<std::string> above;
    std::uint32_t retained = 0;
    std::uint32_t prunable = 0;
    const std::uint64_t oldest_kept = oldest_retained_generation(pointer.sequence);
    for (const GenerationEntry& entry : scan.value().generations) {
      if (entry.sequence > pointer.sequence) {
        above.push_back(generation_file_name(entry.sequence));
      } else if (entry.sequence < oldest_kept) {
        // Outside the retention bound: retained history no longer, and folded
        // away below once the published generation has been verified.
        ++prunable;
      } else if (entry.sequence < pointer.sequence) {
        ++retained;
      }
    }
    if (!above.empty()) {
      return store_failure_result<StoreSession>(
          ErrorCode::kStoreRecoveryFailed, "StoreSession::open", root,
          "the pointer record names sequence " + std::to_string(pointer.sequence) + " but " +
              std::to_string(above.size()) + " generation file(s) above it exist: " + join_names(above));
    }
    report.unpublished_generations = retained;

    const Result<std::uint64_t> generation_size = detail::file_size(generation_path);
    if (!generation_size) {
      return Result<StoreSession>{generation_size.error()};
    }
    if (generation_size.value() < kContainerHeaderSize) {
      return store_failure_result<StoreSession>(
          ErrorCode::kStoreTruncated, "StoreSession::open", generation_path,
          "the container is " + std::to_string(generation_size.value()) + " bytes, below the " +
              std::to_string(kContainerHeaderSize) + " byte header");
    }
    const std::uint64_t payload_bytes = generation_size.value() - kContainerHeaderSize;
    if (payload_bytes > options.limits.max_state_bytes) {
      return store_failure_result<StoreSession>(
          ErrorCode::kPayloadTooLarge, "StoreSession::open", generation_path,
          "the payload is " + std::to_string(payload_bytes) + " bytes, above the bound of " +
              std::to_string(options.limits.max_state_bytes) + " bytes");
    }
    const Result<std::vector<std::byte>> container = read_sized_file(generation_path, generation_size.value());
    if (!container) {
      return Result<StoreSession>{container.error()};
    }
    ContainerView view;
    const Status container_status = verify_container(container.value(), pointer.sequence, generation_path, view);
    if (!container_status.ok()) {
      return Result<StoreSession>{container_status.error()};
    }
    // The pointer holds a digest over the whole container file, header and
    // payload. It is checked last because it binds the bytes that were just
    // structurally verified to the record that names them.
    if (digest_bytes(container.value()) != pointer.container_digest) {
      return store_failure_result<StoreSession>(
          ErrorCode::kStoreChecksumMismatch, "StoreSession::open", generation_path,
          "the whole file digest does not match the digest recorded in the CURRENT pointer record");
    }

    Result<CoordinatorState> decoded = decode_state(view.payload, options.limits);
    if (!decoded) {
      return Result<StoreSession>{decoded.error()};
    }
    CoordinatorState recovered = std::move(decoded).value();

    const Status limit_status = check_requested_limits(options.limits, recovered.limits, root);
    if (!limit_status.ok()) {
      return Result<StoreSession>{limit_status.error()};
    }
    // The state carries the limits this session operates under from here on.
    recovered.limits = options.limits;

    if (!options.read_only && prunable != 0U) {
      // The retention bound is applied here as well as after a publish, so a
      // store written under a larger bound cannot grow for ever. It happens
      // only after the published generation has been read and verified: a store
      // whose pointer or container is damaged must not lose history before an
      // operator has seen what is wrong with it.
      const Status pruned = session.prune(pointer.sequence);
      if (!pruned.ok()) {
        return Result<StoreSession>{pruned.error()};
      }
      report.pruned_generations = prunable;
    }

    report.created_new_store = false;
    report.opened_existing_store = true;
    report.recovered_sequence = CommitSequence{pointer.sequence};

    if (options.read_only) {
      // A read only session never recovers: it reports the durable facts
      // exactly as they were written and advances nothing, because a reader
      // that bumped the control epoch in its own copy would see every live
      // grant as fenced.
      report.recovered = false;
      report.previous_epoch = recovered.control_epoch;
      report.current_epoch = recovered.control_epoch;
      report.detail = "opened an existing store read only at sequence " + std::to_string(pointer.sequence);
    } else {
      // The restart fences every grant by advancing the control epoch: a grant
      // binds the epoch it was issued under, so it stops being live without any
      // byte of history being rewritten. The incarnation is replaced because a
      // new process is a new authority, and a caller holding the old identity
      // must be told so rather than silently obeyed.
      const ControlEpoch previous_epoch = recovered.control_epoch;
      const ControlEpoch current_epoch{previous_epoch.value() + 1U};
      for (const PlanRecord& plan : recovered.plans) {
        if (grant_is_live(plan, previous_epoch)) {
          ++report.grants_fenced;
          if (derive_state(plan, previous_epoch) != derive_state(plan, current_epoch)) {
            ++report.plans_reopened;
          }
        }
      }
      report.recovered = true;
      report.previous_epoch = previous_epoch;
      report.current_epoch = current_epoch;
      report.detail = "recovered generation " + std::to_string(pointer.sequence) + " under control epoch " +
                      to_string(current_epoch);
      recovered.control_epoch = current_epoch;
      recovered.incarnation = make_incarnation();
    }

    state = std::move(recovered);
    session.sequence_ = pointer.sequence;
    session.open_ = true;
    return std::move(session);
  } catch (const std::filesystem::filesystem_error& error) {
    return make_error<StoreSession>(ErrorCode::kStoreIoError,
                                    "StoreSession::open: " + std::string{error.what()});
  } catch (const std::exception& error) {
    return make_error<StoreSession>(ErrorCode::kStoreIoError,
                                    "StoreSession::open: " + std::string{error.what()});
  }
}

Status StoreSession::publish(const CoordinatorState& state) {
  if (read_only_) {
    return store_failure(ErrorCode::kReadOnlyStore, "publish", root_,
                         "this session is read only, so it never publishes");
  }
  if (!open_) {
    return store_failure(ErrorCode::kInternal, "publish", root_,
                         "the store session is not open");
  }
  try {
    // The published sequence is the state's own commit sequence. A state that
    // would move the pointer backwards is refused outright, because adopting it
    // would resurrect a generation that was already superseded. Republishing
    // the same sequence is allowed: that is what a retry after a failure at or
    // after the pointer step looks like, and it rewrites one generation with
    // identical bytes.
    if (state.commit_sequence < CommitSequence{sequence_}) {
      return store_failure(ErrorCode::kInvalidGenerationOrder, "publish", root_,
                           "the state carries commit sequence " + to_string(state.commit_sequence) +
                               ", below the published sequence " + to_string(CommitSequence{sequence_}));
    }

    // Step 1: encode and validate the next state. A state that cannot be
    // decoded by this build is never written to a store.
    std::vector<std::byte> payload;
    const Status encoded = encode_state(state, limits_, payload);
    if (!encoded.ok()) {
      return encoded;
    }
    const std::uint64_t payload_bytes = static_cast<std::uint64_t>(payload.size());
    if (payload_bytes > limits_.max_state_bytes) {
      return store_failure(ErrorCode::kPayloadTooLarge, "publish", root_,
                           "the encoded state is " + std::to_string(payload_bytes) +
                               " bytes, above the bound of " + std::to_string(limits_.max_state_bytes) +
                               " bytes");
    }

    // The container is built in one fixed buffer: a 128 byte header followed by
    // the payload, so the bytes that are written are the bytes that were
    // digested and, later, the bytes that are verified.
    const std::size_t header_bytes = static_cast<std::size_t>(kContainerHeaderSize);
    std::vector<std::byte> container(header_bytes + payload.size());
    write_container_header(std::span<std::byte>{container.data(), header_bytes}, state.commit_sequence.value(),
                           payload, digest_bytes(payload));
    std::copy(payload.begin(), payload.end(), container.begin() + static_cast<std::ptrdiff_t>(header_bytes));

    // Steps 2 to 5: stage, flush, read back, publish the generation.
    const Status generation = write_generation(state.commit_sequence.value(), container, faults_);
    if (!generation.ok()) {
      return generation;
    }

    // Step 6: the pointer. Until it names the new generation, the new
    // generation does not exist as far as any reader is concerned.
    const Status pointer = write_pointer(state.commit_sequence.value(), digest_bytes(container), faults_);
    if (!pointer.ok()) {
      return pointer;
    }

    // The commit sequence is durable only now: CURRENT has been replaced and
    // flushed, so a crash before this point leaves the previous sequence, and
    // after it the new one.
    sequence_ = state.commit_sequence.value();

    // Step 7: prune generations outside the retention bound and any remaining
    // transient file. This runs after the commit is durable, so a pruning
    // failure is deliberately not reported as a failed commit: the durable
    // state is already the new generation, and reporting failure would invite a
    // retry of a commit that already happened. The next open folds the
    // directory down again, so nothing accumulates.
    static_cast<void>(prune(sequence_));
    return Status::success();
  } catch (const std::filesystem::filesystem_error& error) {
    return store_failure(ErrorCode::kStoreIoError, "publish", root_, error.what());
  } catch (const std::exception& error) {
    return store_failure(ErrorCode::kStoreIoError, "publish", root_, error.what());
  }
}

Status StoreSession::flush() {
  if (!open_) {
    return store_failure(ErrorCode::kInternal, "flush", root_, "the store session is not open");
  }
  if (read_only_) {
    // A read only session wrote nothing, so it has nothing of its own to make
    // durable; the store's writer owns that.
    return Status::success();
  }
  try {
    const std::filesystem::path generation = root_ / generation_file_name(sequence_);
    if (path_exists(generation)) {
      const Status synced = sync_file(generation);
      if (!synced.ok()) {
        return synced;
      }
    }
    return sync_directory(root_);
  } catch (const std::filesystem::filesystem_error& error) {
    return store_failure(ErrorCode::kStoreIoError, "flush", root_, error.what());
  } catch (const std::exception& error) {
    return store_failure(ErrorCode::kStoreIoError, "flush", root_, error.what());
  }
}

Status StoreSession::write_generation(std::uint64_t sequence, std::span<const std::byte> container,
                                      const PublishFaultHooks& hooks) {
  const std::filesystem::path staged = root_ / (std::string{kTransientFilePrefix} + random_token());
  const Status status = stage_generation(staged, sequence, container, hooks);
  if (!status.ok()) {
    discard_staged(staged);
  }
  return status;
}

Status StoreSession::write_pointer(std::uint64_t sequence, ContentDigest container_digest,
                                   const PublishFaultHooks& hooks) {
  const std::filesystem::path staged = root_ / (std::string{kTransientFilePrefix} + random_token());
  const Status status = stage_pointer(staged, sequence, container_digest, hooks);
  if (!status.ok()) {
    discard_staged(staged);
  }
  return status;
}

Status StoreSession::prune(std::uint64_t published_sequence) {
  const Result<DirectoryScan> scan = scan_directory(root_);
  if (!scan) {
    return Status::failure(scan.error());
  }
  // The published generation and the ones immediately below it are kept, so an
  // operator can still see recent history; everything older is removed and the
  // store therefore cannot grow without bound.
  const std::uint64_t oldest_kept = oldest_retained_generation(published_sequence);
  Status first_failure = Status::success();
  for (const GenerationEntry& entry : scan.value().generations) {
    if (entry.sequence >= oldest_kept && entry.sequence <= published_sequence) {
      continue;
    }
    const Status removed = remove_file(entry.path);
    if (!removed.ok() && first_failure.ok()) {
      first_failure = removed;
    }
  }
  for (const std::filesystem::path& transient : scan.value().transient_files) {
    const Status removed = remove_file(transient);
    if (!removed.ok() && first_failure.ok()) {
      first_failure = removed;
    }
  }
  return first_failure;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Read only inspection
// ---------------------------------------------------------------------------

Result<StoreInspection> inspect_store(const std::filesystem::path& root, const Limits& limits) {
  try {
    const Status limits_status = limits.validate();
    if (!limits_status.ok()) {
      return Result<StoreInspection>{limits_status.error()};
    }

    StoreInspection inspection;
    if (!detail::path_exists(root)) {
      // An absent store is not an error: the report says it is not there and
      // every other field keeps its documented default.
      return inspection;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
      return make_error<StoreInspection>(ErrorCode::kStorePathInvalid,
                                         "inspect_store: the path exists and is not a directory [path: " +
                                             detail::to_utf8(root) + "]");
    }
    inspection.exists = true;

    const Result<detail::DirectoryScan> scan = detail::scan_directory(root);
    if (!scan) {
      return Result<StoreInspection>{scan.error()};
    }
    inspection.generation_files = static_cast<std::uint64_t>(scan.value().generations.size());
    inspection.transient_files = static_cast<std::uint64_t>(scan.value().transient_files.size());
    for (const detail::GenerationEntry& entry : scan.value().generations) {
      inspection.bytes += entry.bytes;
    }
    inspection.bytes += scan.value().transient_bytes;
    inspection.writer_lock_present = scan.value().writer_lock_present;
    if (inspection.writer_lock_present) {
      const Result<std::string> record = detail::read_lock_record(root / std::string(kWriterLockFileName));
      if (record) {
        inspection.writer_lock_record = record.value();
      }
    }

    const std::filesystem::path pointer_path = root / std::string(kPointerFileName);
    if (!detail::path_exists(pointer_path)) {
      return inspection;
    }
    inspection.has_pointer = true;

    const Result<std::uint64_t> pointer_size = detail::file_size(pointer_path);
    if (!pointer_size) {
      return Result<StoreInspection>{pointer_size.error()};
    }
    if (pointer_size.value() != kPointerRecordSize) {
      const ErrorCode code = pointer_size.value() < kPointerRecordSize ? ErrorCode::kStoreTruncated
                                                                       : ErrorCode::kStoreTrailingBytes;
      return detail::store_failure_result<StoreInspection>(
          code, "inspect_store: the pointer record", pointer_path,
          "the record is " + std::to_string(pointer_size.value()) + " bytes, and the record is " +
              std::to_string(kPointerRecordSize) + " bytes");
    }
    const Result<std::vector<std::byte>> pointer_bytes = detail::read_sized_file(pointer_path, pointer_size.value());
    if (!pointer_bytes) {
      return Result<StoreInspection>{pointer_bytes.error()};
    }
    detail::PointerView pointer;
    const Status pointer_status = detail::verify_pointer_record(pointer_bytes.value(), pointer_path, pointer);
    if (!pointer_status.ok()) {
      return Result<StoreInspection>{pointer_status.error()};
    }
    inspection.sequence = CommitSequence{pointer.sequence};

    const std::filesystem::path generation_path = root / detail::generation_file_name(pointer.sequence);
    if (!detail::path_exists(generation_path)) {
      return detail::store_failure_result<StoreInspection>(
          ErrorCode::kMissingGenerationFile, "inspect_store", generation_path,
          "the pointer record names sequence " + std::to_string(pointer.sequence) +
              " and that generation file does not exist");
    }
    const Result<std::uint64_t> generation_size = detail::file_size(generation_path);
    if (!generation_size) {
      return Result<StoreInspection>{generation_size.error()};
    }
    if (generation_size.value() < kContainerHeaderSize) {
      return detail::store_failure_result<StoreInspection>(
          ErrorCode::kStoreTruncated, "inspect_store", generation_path,
          "the container is " + std::to_string(generation_size.value()) + " bytes, below the " +
              std::to_string(kContainerHeaderSize) + " byte header");
    }
    const std::uint64_t payload_bytes = generation_size.value() - kContainerHeaderSize;
    if (payload_bytes > limits.max_state_bytes) {
      return detail::store_failure_result<StoreInspection>(
          ErrorCode::kPayloadTooLarge, "inspect_store", generation_path,
          "the payload is " + std::to_string(payload_bytes) + " bytes, above the bound of " +
              std::to_string(limits.max_state_bytes) + " bytes");
    }
    const Result<std::vector<std::byte>> container =
        detail::read_sized_file(generation_path, generation_size.value());
    if (!container) {
      return Result<StoreInspection>{container.error()};
    }
    detail::ContainerView view;
    const Status container_status =
        detail::verify_container(container.value(), pointer.sequence, generation_path, view);
    if (!container_status.ok()) {
      return Result<StoreInspection>{container_status.error()};
    }
    if (digest_bytes(container.value()) != pointer.container_digest) {
      return detail::store_failure_result<StoreInspection>(
          ErrorCode::kStoreChecksumMismatch, "inspect_store", generation_path,
          "the whole file digest does not match the digest recorded in the CURRENT pointer record");
    }

    // The state is decoded in full, which is what makes the reported epoch and
    // state digest the ones the coordinator itself would derive from these
    // bytes. Inspection never writes: nothing here can change what is on disk.
    Result<detail::CoordinatorState> decoded = detail::decode_state(view.payload, limits);
    if (!decoded) {
      return Result<StoreInspection>{decoded.error()};
    }
    detail::CoordinatorState state = std::move(decoded).value();
    inspection.epoch = state.control_epoch;
    inspection.state_digest = detail::coordinator_digest(state);
    return inspection;
  } catch (const std::filesystem::filesystem_error& error) {
    return make_error<StoreInspection>(ErrorCode::kStoreIoError,
                                       "inspect_store: " + std::string{error.what()});
  } catch (const std::exception& error) {
    return make_error<StoreInspection>(ErrorCode::kStoreIoError,
                                       "inspect_store: " + std::string{error.what()});
  }
}

}  // namespace facilitydrain
