// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_VERSION_HPP
#define FACILITYDRAIN_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace facilitydrain {

/// Semantic version of this library.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

/// "1.0.0".
[[nodiscard]] const char* version_string() noexcept;

/// The encoding version of the canonical coordinator state payload. A reader
/// accepts exactly this version: an unknown payload version is never guessed at.
inline constexpr std::uint32_t kStateFormatVersion = 1;

/// The version of the durable container file. A reader accepts exactly this
/// version and rejects everything else as kStoreVersionUnsupported.
inline constexpr std::uint32_t kContainerFormatVersion = 1;

/// The version of the durable store directory layout: the CURRENT pointer and
/// the generation file naming scheme.
inline constexpr std::uint32_t kStoreLayoutVersion = 1;

/// The version of the canonical text report format.
inline constexpr std::uint32_t kReportFormatVersion = 1;

/// Eight byte magic of a generation container file: "FDCDRN01".
inline constexpr std::string_view kContainerMagic = "FDCDRN01";

/// Eight byte magic of the CURRENT pointer file: "FDCDCUR1".
inline constexpr std::string_view kPointerMagic = "FDCDCUR1";

/// File name of the store pointer inside a store root.
inline constexpr std::string_view kPointerFileName = "CURRENT";

/// File name of the writer lock inside a store root.
inline constexpr std::string_view kWriterLockFileName = "writer.lock";

/// Generation file names are gen-<20 zero padded decimal>.fdcdrain.
inline constexpr std::string_view kGenerationFilePrefix = "gen-";
inline constexpr std::string_view kGenerationFileSuffix = ".fdcdrain";

/// Transient publish files. These are never authoritative, and an open removes
/// them rather than interpreting them.
inline constexpr std::string_view kTransientFilePrefix = "tmp-";

/// Fixed size of the container header, in bytes.
inline constexpr std::uint64_t kContainerHeaderSize = 128;

/// Fixed size of the CURRENT pointer record, in bytes.
inline constexpr std::uint64_t kPointerRecordSize = 128;

/// Number of committed generations retained after a publish. Older ones are
/// pruned, so a store cannot grow without bound.
inline constexpr std::uint32_t kRetainedGenerations = 4;

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_VERSION_HPP
