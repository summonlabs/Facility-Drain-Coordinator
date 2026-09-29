// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/version.hpp"

#include <cstdint>

namespace facilitydrain {

// The text form and the numeric constants must never disagree: a caller that
// records version_string() in a report and kVersionMajor in a header has to be
// describing the same release.
static_assert(kVersionMajor == 1U && kVersionMinor == 0U && kVersionPatch == 0U,
              "version_string() must match the declared version constants");

const char* version_string() noexcept { return "1.0.0"; }

}  // namespace facilitydrain
