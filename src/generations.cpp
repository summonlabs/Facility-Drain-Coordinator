// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/generations.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace facilitydrain {
namespace {

/// The participating generations in the fixed comparison order. Every function
/// in this file walks this one table, so the field order used by equality, by
/// the difference report, by the ordering test and by the canonical text can
/// never drift apart.
struct FieldRef {
  std::string_view name;
  std::string_view short_name;
};

constexpr std::array<FieldRef, kGenerationFieldCount> kFields{{
    {"scope", "scope"},
    {"dependency", "dep"},
    {"reservation", "res"},
    {"obligation", "obl"},
    {"policy", "pol"},
    {"topology", "topo"},
    {"maintenance", "maint"},
    {"capacity", "cap"},
    {"hardware", "hw"},
    {"firmware", "fw"},
}};

std::array<std::uint64_t, kGenerationFieldCount> values_of(const GenerationSet& set) noexcept {
  return {set.scope.value(),       set.dependency.value(), set.reservation.value(),
          set.obligation.value(),  set.policy.value(),     set.topology.value(),
          set.maintenance.value(), set.capacity.value(),   set.hardware.value(),
          set.firmware.value()};
}

}  // namespace

bool GenerationSet::operator==(const GenerationSet& other) const noexcept {
  return values_of(*this) == values_of(other);
}

std::string_view generation_field_name(std::uint32_t index) noexcept {
  if (index >= kGenerationFieldCount) {
    return "unknown";
  }
  return kFields[index].name;
}

std::string_view GenerationSet::first_difference(const GenerationSet& other) const noexcept {
  const auto mine = values_of(*this);
  const auto theirs = values_of(other);
  for (std::uint32_t index = 0; index < kGenerationFieldCount; ++index) {
    if (mine[index] != theirs[index]) {
      return kFields[index].name;
    }
  }
  return {};
}

bool GenerationSet::is_at_least(const GenerationSet& other) const noexcept {
  const auto mine = values_of(*this);
  const auto theirs = values_of(other);
  for (std::uint32_t index = 0; index < kGenerationFieldCount; ++index) {
    if (mine[index] < theirs[index]) {
      return false;
    }
  }
  return true;
}

bool GenerationSet::is_complete() const noexcept {
  for (const std::uint64_t value : values_of(*this)) {
    // A default generation means never observed. A set containing one cannot
    // describe a world, so it can never be bound to a plan or matched by
    // evidence.
    if (value == 0) {
      return false;
    }
  }
  return true;
}

std::string GenerationSet::to_canonical() const {
  const auto values = values_of(*this);
  std::string text;
  for (std::uint32_t index = 0; index < kGenerationFieldCount; ++index) {
    if (index != 0) {
      text.push_back(' ');
    }
    text.append(kFields[index].short_name);
    text.push_back('=');
    text.append(std::to_string(values[index]));
  }
  return text;
}

}  // namespace facilitydrain
