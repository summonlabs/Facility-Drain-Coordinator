// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_DETAIL_UTF8_HPP
#define FACILITYDRAIN_DETAIL_UTF8_HPP

#include "facilitydrain/errors.hpp"

#include <cstdint>
#include <string_view>

namespace facilitydrain {
namespace detail {

/// Strict UTF-8 well-formedness over the whole string.
///
/// This is stricter than "the bytes decode": it rejects overlong encodings
/// (including 0xC0 and 0xC1 lead bytes), UTF-16 surrogate code points
/// (U+D800..U+DFFF), code points above U+10FFFF, truncated sequences, stray
/// continuation bytes, and the invalid lead bytes 0xF5..0xFF. Exactly one byte
/// sequence is accepted per code point, which is what makes a text field
/// comparable byte for byte.
///
/// Control characters are *not* a well-formedness problem and are not rejected
/// here; validate_text() applies that separate policy.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

/// Validates a text field against the supplied byte bound and the strict text
/// policy. The checks run in a fixed order and stop at the first violation, so
/// one input always yields one code and one detail:
///
///   1. byte bound         -> kFieldTooLong  ("field label is 900 bytes, the bound is 256")
///   2. UTF-8 well-formedness -> kInvalidText ("field label is not well-formed UTF-8")
///   3. control characters -> kInvalidText  ("field label contains the control character U+001F")
///
/// Policy steps 2 and 3 reject C0 control characters (U+0000..U+001F) and
/// U+007F: they are legal UTF-8, but a text field carrying them cannot be
/// rendered in a report, a log line or a CLI response without changing what
/// that output means, so they are refused at the boundary instead of being
/// escaped later and compared unequally. Tab, newline and carriage return are
/// therefore rejected as well.
[[nodiscard]] Status validate_text(std::string_view value, std::string_view field_name,
                                   std::uint32_t max_bytes);

}  // namespace detail
}  // namespace facilitydrain

#endif  // FACILITYDRAIN_DETAIL_UTF8_HPP
