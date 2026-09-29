// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#ifndef FACILITYDRAIN_GENERATIONS_HPP
#define FACILITYDRAIN_GENERATIONS_HPP

#include "facilitydrain/export.hpp"
#include "facilitydrain/identity.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace facilitydrain {

// ---------------------------------------------------------------------------
// Generation sets
// ---------------------------------------------------------------------------
//
// A drain decision is only ever valid for one exact combination of the
// generations that described the world when that decision's evidence was
// produced. The set below is that combination. Two records are compatible when
// every participating generation is equal; anything else must be reconciled by
// an explicit plan revision, never by treating close enough as equal.

struct GenerationSet {
  /// The physical scope generation: which physical membership this scope had.
  ScopeGeneration scope{};
  /// The dependency graph generation the enumeration was taken against.
  DependencyGeneration dependency{};
  /// The reservation generation of the facility reservations in scope.
  ReservationGeneration reservation{};
  /// The obligation generation of the obligations in scope.
  ObligationGeneration obligation{};
  /// The policy generation under which the decision is permitted.
  PolicyGeneration policy{};
  /// The network topology generation DFI reported.
  TopologyGeneration topology{};
  /// The maintenance generation of the maintenance protections in scope.
  MaintenanceGeneration maintenance{};
  /// The capacity accounting generation the scope's capacity was booked under.
  CapacityGeneration capacity{};
  /// The accelerator hardware generation of the assets in scope.
  HardwareGeneration hardware{};
  /// The firmware generation of the assets in scope.
  FirmwareGeneration firmware{};

  [[nodiscard]] bool operator==(const GenerationSet& other) const noexcept;
  [[nodiscard]] bool operator!=(const GenerationSet& other) const noexcept { return !(*this == other); }

  /// The name of the first generation that differs, in the fixed field order
  /// listed above. Empty when the sets are equal. This ordering is part of the
  /// contract: the same pair of sets always names the same field.
  [[nodiscard]] FACILITYDRAIN_API std::string_view first_difference(const GenerationSet& other) const noexcept;

  /// True when this set is componentwise not older than the other. Used to
  /// decide whether an observation could in principle supersede another; it
  /// never by itself authorises anything.
  [[nodiscard]] bool is_at_least(const GenerationSet& other) const noexcept;

  /// True when every generation in the set is non default. A default
  /// generation means never observed, which is never compatible with a plan
  /// binding that names a real one.
  [[nodiscard]] bool is_complete() const noexcept;

  /// Canonical single line rendering, used by the report and by the CLI:
  /// "scope=1 dep=2 res=3 obl=4 pol=5 topo=6 maint=7 cap=8 hw=9 fw=10".
  [[nodiscard]] FACILITYDRAIN_API std::string to_canonical() const;
};

/// Names of the participating generations, in the fixed comparison order.
[[nodiscard]] FACILITYDRAIN_API std::string_view generation_field_name(std::uint32_t index) noexcept;

/// Number of participating generations in a GenerationSet.
inline constexpr std::uint32_t kGenerationFieldCount = 10;

}  // namespace facilitydrain

#endif  // FACILITYDRAIN_GENERATIONS_HPP
