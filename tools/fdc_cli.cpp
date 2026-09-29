// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// fdc: the Facility Drain Coordinator command line tool.
//
// One invocation opens one durable store exactly once, fills the mutation
// context from the live facts, performs exactly one operation and exits. The
// tool never invents a verdict and never invents a second output format:
// --json prints the canonical export from the library, and a rejected
// operation prints "<token>: <detail>" using the code the library returned.

#include "facilitydrain/clock.hpp"
#include "facilitydrain/consumer.hpp"
#include "facilitydrain/coordinator.hpp"
#include "facilitydrain/digest.hpp"
#include "facilitydrain/errors.hpp"
#include "facilitydrain/evaluation.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/limits.hpp"
#include "facilitydrain/persistence.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/report.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"
#include "facilitydrain/scope.hpp"
#include "facilitydrain/snapshot.hpp"
#include "facilitydrain/version.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace facilitydrain;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitRejected = 3;
constexpr int kExitStore = 4;

/// The fixed clock the reproducible scenarios run under: the report they print
/// is byte identical on every machine.
constexpr std::int64_t kReproducibleMilliseconds = 1700000000000;

// ---------------------------------------------------------------------------
// Small deterministic text helpers
// ---------------------------------------------------------------------------

[[nodiscard]] bool is_ascii_space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\v' || value == '\f';
}

[[nodiscard]] std::string strip_ascii_space(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_ascii_space(text[begin])) {
    ++begin;
  }
  while (end > begin && is_ascii_space(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

void split_ascii_space(const std::string& text, std::vector<std::string>& out) {
  std::size_t index = 0;
  while (index < text.size()) {
    while (index < text.size() && is_ascii_space(text[index])) {
      ++index;
    }
    const std::size_t start = index;
    while (index < text.size() && !is_ascii_space(text[index])) {
      ++index;
    }
    if (index > start) {
      out.push_back(text.substr(start, index - start));
    }
  }
}

[[nodiscard]] const char* yes_no(bool value) noexcept { return value ? "yes" : "no"; }

void print_text(const std::string& text) {
  std::cout << text;
  if (text.empty() || text.back() != '\n') {
    std::cout << '\n';
  }
}

int usage_failure(const std::string& message) {
  std::cout.flush();
  std::cerr << "fdc: " << message << '\n';
  std::cerr << "fdc: run 'fdc --help' for the accepted commands and options\n";
  return kExitUsage;
}

int report_error(const Error& error, bool json) {
  if (json) {
    std::cout.flush();
    std::cerr << error.to_text() << '\n';
  } else {
    std::cout << error.to_text() << '\n';
  }
  return is_store_error(error.code()) ? kExitStore : kExitRejected;
}

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------
//
// There is no argument parsing library here on purpose: the accepted option set
// is one fixed table, so an unknown or misspelled option is a usage error
// rather than a silently ignored word.

struct OptionSpec {
  std::string_view name;
  bool takes_value = true;
  /// Repeatable options accumulate: a plan has many targets and a revision may
  /// move several generations at once.
  bool repeatable = false;
};

constexpr OptionSpec kOptionTable[] = {
    {"--root", true},
    {"--plan", true},
    {"--scope", true},
    {"--target", true, true},
    {"--required", true},
    {"--scope-generation", true},
    {"--dependency-generation", true},
    {"--reservation-generation", true},
    {"--obligation-generation", true},
    {"--policy-generation", true},
    {"--topology-generation", true},
    {"--maintenance-generation", true},
    {"--capacity-generation", true},
    {"--hardware-generation", true},
    {"--firmware-generation", true},
    {"--policy-id", true},
    {"--policy-digest", true},
    {"--label", true},
    {"--requested-by", true},
    {"--consumer-file", true},
    {"--observation", true},
    {"--expected-revision", true},
    {"--reason", true},
    {"--detail", true},
    {"--generation-field", true, true},
    {"--domain", true},
    {"--coverage", true},
    {"--generation", true},
    {"--observed", true},
    {"--source", true},
    {"--annotation", true},
    {"--request", true},
    {"--bound", true},
    {"--reference", true},
    {"--system", true},
    {"--state", true},
    {"--payload-digest", true},
    {"--manifest-digest", true},
    {"--residual-count", true},
    {"--obligation", true},
    {"--kind", true},
    {"--evidence-generation", true},
    {"--by", true},
    {"--json", false},
    {"--help", false},
    {"--residual-count-unknown", false},
};

struct Options {
  std::vector<std::pair<std::string, std::string>> given;

  [[nodiscard]] bool has(std::string_view name) const {
    for (const auto& entry : given) {
      if (entry.first == name) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] const std::string* value(std::string_view name) const {
    for (const auto& entry : given) {
      if (entry.first == name) {
        return &entry.second;
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::vector<std::string> all(std::string_view name) const {
    std::vector<std::string> values;
    for (const auto& entry : given) {
      if (entry.first == name) {
        values.push_back(entry.second);
      }
    }
    return values;
  }
};

[[nodiscard]] const OptionSpec* find_option_spec(std::string_view word) {
  for (const OptionSpec& spec : kOptionTable) {
    if (spec.name == word) {
      return &spec;
    }
  }
  return nullptr;
}

void collect_words(int argc, char** argv, std::vector<std::string>& out) {
  for (int index = 1; index < argc; ++index) {
    out.emplace_back(argv[index]);
  }
}

bool parse_options(const std::vector<std::string>& words, Options& out, std::string& error) {
  std::size_t index = 0;
  while (index < words.size()) {
    const std::string& word = words[index];
    if (word.size() < 2 || word[0] != '-') {
      error = "unexpected argument '" + word + "'";
      return false;
    }
    const OptionSpec* spec = find_option_spec(word);
    if (spec == nullptr) {
      error = "unknown option '" + word + "'";
      return false;
    }
    if (!spec->repeatable && out.has(spec->name)) {
      error = "option '" + word + "' was given more than once";
      return false;
    }
    if (!spec->takes_value) {
      out.given.emplace_back(word, std::string{});
      ++index;
      continue;
    }
    if (index + 1 >= words.size()) {
      error = "option '" + word + "' requires a value";
      return false;
    }
    const std::string& value = words[index + 1];
    if (value.empty()) {
      error = "option '" + word + "' requires a non-empty value";
      return false;
    }
    if (find_option_spec(value) != nullptr) {
      error = "option '" + word + "' requires a value, but '" + value + "' is an option";
      return false;
    }
    out.given.emplace_back(word, value);
    index += 2;
  }
  return true;
}

bool check_allowed(const Options& options, std::initializer_list<std::string_view> allowed,
                   std::string& error) {
  for (const auto& entry : options.given) {
    if (entry.first == "--help") {
      continue;
    }
    bool accepted = false;
    for (const std::string_view name : allowed) {
      if (entry.first == name) {
        accepted = true;
        break;
      }
    }
    if (!accepted) {
      error = "option '" + entry.first + "' is not accepted by this command";
      return false;
    }
  }
  return true;
}

[[nodiscard]] const std::string* require_value(const Options& options, std::string_view name,
                                               std::string& error) {
  const std::string* value = options.value(name);
  if (value == nullptr) {
    error = "option '" + std::string{name} + "' is required";
  }
  return value;
}

// ---------------------------------------------------------------------------
// Canonical value readers
// ---------------------------------------------------------------------------

bool to_u64(const std::string& text, std::string_view what, std::uint64_t& out, std::string& error) {
  std::uint64_t raw = 0;
  const IdentityParseError status = parse_strong(text, raw);
  if (status != IdentityParseError::kOk) {
    error = std::string{what} + " '" + text + "' is not a canonical count: " +
            std::string{to_string(status)};
    return false;
  }
  out = raw;
  return true;
}

template <typename Tag>
bool to_identity(const std::string& text, std::string_view what, StrongValue<Tag>& out,
                 std::string& error) {
  std::uint64_t raw = 0;
  const IdentityParseError status = parse_strong(text, raw);
  if (status != IdentityParseError::kOk) {
    error = std::string{what} + " '" + text + "' is not a canonical identity: " +
            std::string{to_string(status)};
    return false;
  }
  out = StrongValue<Tag>{raw};
  return true;
}

bool to_digest(const std::string& text, std::string_view what, ContentDigest& out, std::string& error) {
  const std::optional<ContentDigest> digest = ContentDigest::from_hex(text);
  if (!digest.has_value()) {
    error = std::string{what} + " '" + text +
            "' is not a digest: exactly 64 lowercase hexadecimal characters are required";
    return false;
  }
  out = digest.value();
  return true;
}

bool to_scope(const std::string& text, std::string_view what, DrainScope& out, std::string& error) {
  Result<DrainScope> parsed = parse_scope(text);
  if (!parsed) {
    error = std::string{what} + " '" + text + "' is not a scope: " + parsed.error().to_text();
    return false;
  }
  out = parsed.value();
  return true;
}

bool to_mask(const std::string& text, std::string_view what, DomainMask& out, std::string& error) {
  Result<DomainMask> parsed = DomainMask::parse(text);
  if (!parsed) {
    error = std::string{what} + " '" + text + "' is not a domain mask: " + parsed.error().to_text();
    return false;
  }
  out = parsed.value();
  return true;
}

bool read_generation_option(const Options& options, std::string_view name, std::uint64_t& out,
                            std::string& error) {
  const std::string* text = require_value(options, name, error);
  if (text == nullptr) {
    return false;
  }
  return to_u64(*text, name, out, error);
}

[[nodiscard]] GenerationSet complete_generations() {
  GenerationSet set;
  set.scope = ScopeGeneration{1};
  set.dependency = DependencyGeneration{2};
  set.reservation = ReservationGeneration{3};
  set.obligation = ObligationGeneration{4};
  set.policy = PolicyGeneration{5};
  set.topology = TopologyGeneration{6};
  set.maintenance = MaintenanceGeneration{7};
  set.capacity = CapacityGeneration{8};
  set.hardware = HardwareGeneration{9};
  set.firmware = FirmwareGeneration{10};
  return set;
}

bool apply_generation_fields(const Options& options, GenerationSet& set, std::string& error) {
  for (const std::string& text : options.all("--generation-field")) {
    const std::size_t equals = text.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= text.size()) {
      error = "generation field '" + text + "' is not NAME=VALUE";
      return false;
    }
    const std::string_view name{text.data(), equals};
    std::uint64_t value = 0;
    if (!to_u64(text.substr(equals + 1), "generation field value", value, error)) {
      return false;
    }
    std::uint32_t index = kGenerationFieldCount;
    for (std::uint32_t candidate = 0; candidate < kGenerationFieldCount; ++candidate) {
      if (generation_field_name(candidate) == name) {
        index = candidate;
        break;
      }
    }
    if (index == kGenerationFieldCount) {
      error = "generation field '" + std::string{name} +
              "' is not one of scope, dependency, reservation, obligation, policy, topology, "
              "maintenance, capacity, hardware, firmware";
      return false;
    }
    switch (index) {
      case 0: set.scope = ScopeGeneration{value}; break;
      case 1: set.dependency = DependencyGeneration{value}; break;
      case 2: set.reservation = ReservationGeneration{value}; break;
      case 3: set.obligation = ObligationGeneration{value}; break;
      case 4: set.policy = PolicyGeneration{value}; break;
      case 5: set.topology = TopologyGeneration{value}; break;
      case 6: set.maintenance = MaintenanceGeneration{value}; break;
      case 7: set.capacity = CapacityGeneration{value}; break;
      case 8: set.hardware = HardwareGeneration{value}; break;
      case 9: set.firmware = FirmwareGeneration{value}; break;
      default: break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// The consumer file
// ---------------------------------------------------------------------------
//
// Line oriented, deliberately strict: '<category-token> <obligation-id>
// <generation> <reservation> <mandatory|advisory> [label] [source]'. A line
// that does not parse is a usage error naming the line number, because a
// silently dropped obligation would change what the plan means.

bool read_consumer_file(const std::string& path, std::vector<ConsumerRecord>& out, std::string& error) {
  std::ifstream input(std::filesystem::path(path), std::ios::binary);
  if (!input.is_open()) {
    error = "consumer file '" + path + "' cannot be opened";
    return false;
  }
  std::string line;
  std::uint64_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::string stripped = strip_ascii_space(line);
    if (stripped.empty() || stripped.front() == '#') {
      continue;
    }
    std::vector<std::string> fields;
    split_ascii_space(stripped, fields);
    const std::string prefix =
        "consumer file '" + path + "' line " + std::to_string(line_number) + ": ";
    if (fields.size() < 5 || fields.size() > 7) {
      error = prefix + "expected 5 to 7 fields, found " + std::to_string(fields.size());
      return false;
    }
    ConsumerRecord record;
    const std::optional<ConsumerCategory> category = consumer_category_from_token(fields[0]);
    if (!category.has_value()) {
      error = prefix + "unknown category token '" + fields[0] + "'";
      return false;
    }
    record.category = category.value();
    std::string field_error;
    if (!to_identity(fields[1], "obligation id", record.obligation, field_error)) {
      error = prefix + field_error;
      return false;
    }
    std::uint64_t generation = 0;
    if (!to_u64(fields[2], "obligation generation", generation, field_error)) {
      error = prefix + field_error;
      return false;
    }
    record.generation = ObligationGeneration{generation};
    std::uint64_t reservation = 0;
    if (!to_u64(fields[3], "reservation generation", reservation, field_error)) {
      error = prefix + field_error;
      return false;
    }
    record.reservation = ReservationGeneration{reservation};
    const std::optional<ObligationStrength> strength = obligation_strength_from_token(fields[4]);
    if (!strength.has_value()) {
      error = prefix + "unknown strength token '" + fields[4] + "'";
      return false;
    }
    record.strength = strength.value();
    if (fields.size() >= 6) {
      record.label = fields[5];
    }
    if (fields.size() >= 7) {
      record.source = fields[6];
    }
    out.push_back(std::move(record));
  }
  return true;
}

// ---------------------------------------------------------------------------
// Store sessions
// ---------------------------------------------------------------------------

struct OpenOutcome {
  std::optional<Coordinator> coordinator{};
  Error error{};
};

OpenOutcome open_store(const std::string& root, bool read_only, bool reproducible_clock) {
  OpenOutcome outcome;
  CoordinatorOpenRequest request;
  request.root = std::filesystem::path(root);
  request.writer_label = "fdc";
  request.read_only = read_only;
  request.create_if_missing = !read_only;
  if (reproducible_clock) {
    request.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(kReproducibleMilliseconds)};
  }
  Result<Coordinator> opened = Coordinator::open(request);
  if (!opened) {
    outcome.error = opened.error();
    return outcome;
  }
  outcome.coordinator.emplace(std::move(opened).value());
  return outcome;
}

/// Fills the creation context from the live facts. Creation names revision 1,
/// and its default observation is the coordinator observation sequence plus one:
/// a plan that does not exist yet has no last observation to advance.
bool build_create_context(const Coordinator& coordinator, PlanId plan, const Options& options,
                          const char* principal, MutationContext& out, std::string& error) {
  std::uint64_t observation = coordinator.observation_sequence().value() + 1U;
  if (const std::string* text = options.value("--observation"); text != nullptr) {
    if (!to_u64(*text, "observation", observation, error)) {
      return false;
    }
  }
  Revision revision{1};
  if (const std::string* text = options.value("--expected-revision"); text != nullptr) {
    std::uint64_t raw = 0;
    if (!to_u64(*text, "expected-revision", raw, error)) {
      return false;
    }
    revision = Revision{raw};
  }
  out.plan = plan;
  out.expected_revision = revision;
  out.incarnation = coordinator.incarnation();
  out.expected_epoch = coordinator.control_epoch();
  out.observation = ObservationSequence{observation};
  out.principal = principal;
  return true;
}

/// Fills the mutation context from the live facts. The default observation is
/// the plan's last accepted observation plus one, or the coordinator
/// observation sequence plus one when the plan does not exist yet.
bool build_context(const Coordinator& coordinator, PlanId plan, const Options& options,
                   const char* principal, MutationContext& out, std::string& error) {
  std::uint64_t observation = coordinator.observation_sequence().value() + 1U;
  Revision revision{1};
  Result<DrainPlanSnapshot> snapshot = coordinator.plan(plan);
  if (snapshot) {
    observation = snapshot.value().last_observation.value() + 1U;
    revision = snapshot.value().spec.revision;
  }
  if (const std::string* text = options.value("--observation"); text != nullptr) {
    if (!to_u64(*text, "observation", observation, error)) {
      return false;
    }
  }
  if (const std::string* text = options.value("--expected-revision"); text != nullptr) {
    std::uint64_t raw = 0;
    if (!to_u64(*text, "expected-revision", raw, error)) {
      return false;
    }
    revision = Revision{raw};
  }
  out.plan = plan;
  out.expected_revision = revision;
  out.incarnation = coordinator.incarnation();
  out.expected_epoch = coordinator.control_epoch();
  out.observation = ObservationSequence{observation};
  out.principal = principal;
  return true;
}

void print_plan_line(const DrainPlanSnapshot& plan) {
  std::cout << "plan " << to_string(plan.spec.id) << " revision " << to_string(plan.spec.revision)
            << " scope " << format_scope(plan.spec.scope) << " state " << to_token(plan.state)
            << " required " << plan.required_domains.to_canonical()
            << " residuals open=" << plan.residuals.open_count()
            << " unknown=" << plan.residuals.unknown_count()
            << " requests " << plan.requests.size() << " grant "
            << (plan.grant_live ? "live" : (plan.grant.has_value() ? "recorded" : "none")) << " fence "
            << (plan.fence.has_value() ? std::string{to_token(plan.fence->reason)}
                                       : std::string{"none"})
            << '\n';
}

struct TextOutcome {
  bool ok = false;
  std::string text{};
  Error error{};
};

TextOutcome export_plan_json_text(Coordinator& coordinator, PlanId plan) {
  TextOutcome outcome;
  Result<std::string> text = coordinator.export_plan_json(plan);
  if (!text) {
    outcome.error = text.error();
    return outcome;
  }
  outcome.ok = true;
  outcome.text = std::move(text).value();
  return outcome;
}

TextOutcome export_plan_text_text(Coordinator& coordinator, PlanId plan) {
  TextOutcome outcome;
  Result<std::string> text = coordinator.export_plan_text(plan);
  if (!text) {
    outcome.error = text.error();
    return outcome;
  }
  outcome.ok = true;
  outcome.text = std::move(text).value();
  return outcome;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

int cli_plan_create(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--scope", "--target", "--required", "--scope-generation",
                      "--dependency-generation", "--reservation-generation", "--obligation-generation",
                      "--policy-generation", "--topology-generation", "--maintenance-generation",
                      "--capacity-generation", "--hardware-generation", "--firmware-generation",
                      "--policy-id", "--policy-digest", "--label", "--requested-by", "--consumer-file",
                      "--observation", "--expected-revision", "--json"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* scope_text = require_value(options, "--scope", error);
  const std::string* required_text = require_value(options, "--required", error);
  const std::string* policy_id_text = require_value(options, "--policy-id", error);
  const std::string* policy_digest_text = require_value(options, "--policy-digest", error);
  if (root == nullptr || plan_text == nullptr || scope_text == nullptr || required_text == nullptr ||
      policy_id_text == nullptr || policy_digest_text == nullptr) {
    return usage_failure(error);
  }

  CreatePlanRequest request;
  if (!to_identity(*plan_text, "plan", request.id, error) ||
      !to_scope(*scope_text, "scope", request.scope, error) ||
      !to_mask(*required_text, "required", request.declared_required_domains, error) ||
      !to_identity(*policy_id_text, "policy-id", request.policy_id, error) ||
      !to_digest(*policy_digest_text, "policy-digest", request.policy_digest, error)) {
    return usage_failure(error);
  }
  std::uint64_t raw = 0;
  if (!read_generation_option(options, "--scope-generation", raw, error)) return usage_failure(error);
  request.generations.scope = ScopeGeneration{raw};
  if (!read_generation_option(options, "--dependency-generation", raw, error)) return usage_failure(error);
  request.generations.dependency = DependencyGeneration{raw};
  if (!read_generation_option(options, "--reservation-generation", raw, error)) return usage_failure(error);
  request.generations.reservation = ReservationGeneration{raw};
  if (!read_generation_option(options, "--obligation-generation", raw, error)) return usage_failure(error);
  request.generations.obligation = ObligationGeneration{raw};
  if (!read_generation_option(options, "--policy-generation", raw, error)) return usage_failure(error);
  request.generations.policy = PolicyGeneration{raw};
  if (!read_generation_option(options, "--topology-generation", raw, error)) return usage_failure(error);
  request.generations.topology = TopologyGeneration{raw};
  if (!read_generation_option(options, "--maintenance-generation", raw, error)) return usage_failure(error);
  request.generations.maintenance = MaintenanceGeneration{raw};
  if (!read_generation_option(options, "--capacity-generation", raw, error)) return usage_failure(error);
  request.generations.capacity = CapacityGeneration{raw};
  if (!read_generation_option(options, "--hardware-generation", raw, error)) return usage_failure(error);
  request.generations.hardware = HardwareGeneration{raw};
  if (!read_generation_option(options, "--firmware-generation", raw, error)) return usage_failure(error);
  request.generations.firmware = FirmwareGeneration{raw};

  for (const std::string& text : options.all("--target")) {
    DrainScope target;
    if (!to_scope(text, "target", target, error)) {
      return usage_failure(error);
    }
    request.targets.push_back(target);
  }
  if (request.targets.empty()) {
    request.targets.push_back(request.scope);
  }
  if (const std::string* label = options.value("--label"); label != nullptr) {
    request.label = *label;
  }
  if (const std::string* author = options.value("--requested-by"); author != nullptr) {
    request.requested_by = *author;
  }
  if (const std::string* file = options.value("--consumer-file"); file != nullptr) {
    if (!read_consumer_file(*file, request.consumers, error)) {
      return usage_failure(error);
    }
  }

  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  if (!build_create_context(coordinator, request.id, options,
                            request.requested_by.empty() ? "fdc" : request.requested_by.c_str(),
                            request.context, error)) {
    return usage_failure(error);
  }

  Result<CreatePlanOutcome> outcome = coordinator.create_plan(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  const DrainPlanSnapshot& plan = outcome.value().plan;
  if (json) {
    const TextOutcome text = export_plan_json_text(coordinator, request.id);
    if (!text.ok) {
      return report_error(text.error, json);
    }
    print_text(text.text);
    return kExitOk;
  }
  std::cout << "plan " << to_string(plan.spec.id) << " created revision "
            << to_string(plan.spec.revision) << " scope " << format_scope(plan.spec.scope) << " state "
            << to_token(plan.state) << " commit " << to_string(plan.last_commit) << '\n';
  std::cout << "targets " << plan.spec.targets.to_canonical() << '\n';
  std::cout << "required " << plan.required_domains.to_canonical() << " bound";
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    const std::optional<ContentDigest>& digest = plan.spec.bindings.manifest_digest(domain);
    std::cout << ' ' << to_token(domain) << '='
              << (digest.has_value() ? digest.value().to_hex() : std::string{"unbound"});
  }
  std::cout << '\n';
  return kExitOk;
}

int cli_plan_revise(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--target", "--required", "--generation-field", "--reason",
                      "--detail", "--observation", "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* reason_text = require_value(options, "--reason", error);
  if (root == nullptr || plan_text == nullptr || reason_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  const std::optional<FenceReason> reason = fence_reason_from_token(*reason_text);
  if (!reason.has_value() || (reason.value() != FenceReason::kPlanRevised &&
                              reason.value() != FenceReason::kScopeManifestChanged &&
                              reason.value() != FenceReason::kDependencyChange)) {
    return usage_failure("--reason accepts plan-revised, scope-manifest-changed or dependency-change "
                         "for a revision");
  }

  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  Result<DrainPlanSnapshot> snapshot = coordinator.plan(plan);
  if (!snapshot) {
    return report_error(snapshot.error(), json);
  }

  RevisePlanRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.reason = reason.value();
  if (const std::string* detail = options.value("--detail"); detail != nullptr) {
    request.detail = *detail;
  }
  const std::vector<std::string> targets = options.all("--target");
  if (!targets.empty()) {
    std::vector<DrainScope> manifest;
    for (const std::string& text : targets) {
      DrainScope target;
      if (!to_scope(text, "target", target, error)) {
        return usage_failure(error);
      }
      manifest.push_back(target);
    }
    request.targets = std::move(manifest);
  }
  if (const std::string* mask = options.value("--required"); mask != nullptr) {
    DomainMask parsed;
    if (!to_mask(*mask, "required", parsed, error)) {
      return usage_failure(error);
    }
    request.declared_required_domains = parsed;
  }
  if (options.has("--generation-field")) {
    GenerationSet generations = snapshot.value().spec.bindings.generations;
    if (!apply_generation_fields(options, generations, error)) {
      return usage_failure(error);
    }
    request.generations = generations;
  }

  Result<RevisePlanOutcome> outcome = coordinator.revise_plan(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  print_plan_line(outcome.value().plan);
  if (outcome.value().fence.has_value()) {
    const FenceRecord& fence = outcome.value().fence.value();
    std::cout << "fence reason " << to_token(fence.reason) << " floor " << to_string(fence.floor)
              << " commit " << to_string(fence.commit) << " detail " << fence.detail << '\n';
  }
  return kExitOk;
}

int cli_plan_show(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root", "--plan", "--json"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  if (root == nullptr || plan_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, true, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  const TextOutcome text =
      json ? export_plan_json_text(coordinator, plan) : export_plan_text_text(coordinator, plan);
  if (!text.ok) {
    return report_error(text.error, json);
  }
  print_text(text.text);
  return kExitOk;
}

int cli_plan_list(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root", "--json"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  if (root == nullptr) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, true, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  if (json) {
    Result<std::string> text = coordinator.export_json();
    if (!text) {
      return report_error(text.error(), json);
    }
    print_text(text.value());
    return kExitOk;
  }
  Result<CoordinatorSnapshot> snapshot = coordinator.snapshot();
  if (!snapshot) {
    return report_error(snapshot.error(), json);
  }
  if (snapshot.value().plans.empty()) {
    std::cout << "plans none\n";
    return kExitOk;
  }
  for (const DrainPlanSnapshot& plan : snapshot.value().plans) {
    print_plan_line(plan);
  }
  return kExitOk;
}

int cli_enumerate(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--domain", "--coverage", "--generation", "--observed",
                      "--source", "--consumer-file", "--annotation", "--observation",
                      "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* domain_text = require_value(options, "--domain", error);
  const std::string* coverage_text = require_value(options, "--coverage", error);
  const std::string* generation_text = require_value(options, "--generation", error);
  const std::string* observed_text = require_value(options, "--observed", error);
  const std::string* source = require_value(options, "--source", error);
  if (root == nullptr || plan_text == nullptr || domain_text == nullptr || coverage_text == nullptr ||
      generation_text == nullptr || observed_text == nullptr || source == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  const std::optional<OwnerDomain> domain = owner_domain_from_token(*domain_text);
  if (!domain.has_value()) {
    return usage_failure("unknown domain token '" + *domain_text + "'");
  }
  const std::optional<CoverageState> coverage = coverage_state_from_token(*coverage_text);
  if (!coverage.has_value()) {
    return usage_failure("unknown coverage token '" + *coverage_text + "'");
  }
  std::uint64_t generation = 0;
  std::uint64_t observed = 0;
  if (!to_u64(*generation_text, "generation", generation, error) ||
      !to_u64(*observed_text, "observed", observed, error)) {
    return usage_failure(error);
  }

  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  Result<DrainPlanSnapshot> snapshot = coordinator.plan(plan);
  if (!snapshot) {
    return report_error(snapshot.error(), json);
  }

  RecordEnumerationRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.domain = domain.value();
  request.coverage = coverage.value();
  request.generation = EvidenceGeneration{generation};
  request.observed_at = ObservationSequence{observed};
  request.scope_manifest_digest = snapshot.value().spec.targets.digest();
  request.generations = snapshot.value().spec.bindings.generations;
  request.source = *source;
  if (const std::string* annotation = options.value("--annotation"); annotation != nullptr) {
    request.annotation = *annotation;
  }
  if (const std::string* file = options.value("--consumer-file"); file != nullptr) {
    if (!read_consumer_file(*file, request.consumers, error)) {
      return usage_failure(error);
    }
  }

  Result<RecordEnumerationOutcome> outcome = coordinator.record_enumeration(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  const RecordEnumerationOutcome& recorded = outcome.value();
  std::cout << "plan " << to_string(plan) << " enumeration domain " << to_token(domain.value())
            << " generation " << to_string(recorded.evidence.generation) << " coverage "
            << to_token(recorded.evidence.coverage) << " consumers " << recorded.consumers_recorded
            << " accepted " << yes_no(recorded.accepted) << " bound-manifest "
            << yes_no(recorded.bound_manifest) << " fenced " << yes_no(recorded.fenced)
            << " manifest " << recorded.evidence.manifest_digest.to_hex() << " commit "
            << std::to_string(snapshot.value().last_commit.value() + 1U) << '\n';
  return kExitOk;
}

int cli_issue(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--domain", "--bound", "--observation", "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  if (root == nullptr || plan_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  DomainMask domains = DomainMask::none();
  if (const std::string* mask = options.value("--domain"); mask != nullptr) {
    if (!to_mask(*mask, "domain", domains, error)) {
      return usage_failure(error);
    }
  }
  std::uint64_t bound = 0;
  if (const std::string* text = options.value("--bound"); text != nullptr) {
    if (!to_u64(*text, "bound", bound, error)) {
      return usage_failure(error);
    }
    if (bound > 4294967295ULL) {
      return usage_failure("--bound exceeds the widest request bound");
    }
  }

  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  Result<DrainPlanSnapshot> snapshot = coordinator.plan(plan);
  if (!snapshot) {
    return report_error(snapshot.error(), json);
  }

  IssueRequestsRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.domains = domains.empty() ? snapshot.value().required_domains : domains;
  if (request.domains.empty()) {
    return usage_failure("the plan requires no domain, so there is nothing to request");
  }
  request.bound_operations = static_cast<std::uint32_t>(bound);

  Result<IssueRequestsOutcome> outcome = coordinator.issue_requests(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  const IssueRequestsOutcome& issued = outcome.value();
  std::vector<OwnerDomain> named;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (request.domains.contains(domain)) {
      named.push_back(domain);
    }
  }
  std::cout << "plan " << to_string(plan) << " issue staged " << issued.newly_staged << " duplicates "
            << issued.duplicates << '\n';
  for (std::size_t index = 0; index < issued.items.size(); ++index) {
    const IssueItem& item = issued.items[index];
    const std::string domain_text = index < named.size() ? std::string{to_token(named[index])}
                                                         : std::string{"unknown"};
    if (item.request.id.value() == 0) {
      // A domain with no obligations yields no request at all: there is nothing
      // to ask an owner to drain, and staging one would be an external effect
      // about an empty set.
      std::cout << "item domain " << domain_text << " duplicate " << yes_no(item.duplicate)
                << " deliver " << yes_no(item.deliver) << " request none detail " << item.detail << '\n';
      continue;
    }
    std::cout << "item domain " << domain_text << " duplicate " << yes_no(item.duplicate) << " deliver "
              << yes_no(item.deliver) << " request " << to_string(item.request.id) << " state "
              << to_token(item.request.state) << " attempt " << to_string(item.request.key.attempt)
              << " bound " << item.request.bound_operations << " key "
              << item.request.idempotency_key.to_hex() << " detail " << item.detail << '\n';
  }
  return kExitOk;
}

int cli_deliver(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--request", "--reference", "--observation",
                      "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* request_text = require_value(options, "--request", error);
  if (root == nullptr || plan_text == nullptr || request_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  DrainRequestId request_id;
  if (!to_identity(*plan_text, "plan", plan, error) ||
      !to_identity(*request_text, "request", request_id, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  ConfirmDeliveryRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.request = request_id;
  if (const std::string* reference = options.value("--reference"); reference != nullptr) {
    request.delivery_reference = *reference;
  }
  Result<ConfirmDeliveryOutcome> outcome = coordinator.confirm_delivery(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  std::cout << "plan " << to_string(plan) << " request " << to_string(outcome.value().request.id)
            << " state " << to_token(outcome.value().request.state) << " reference "
            << outcome.value().request.settlement_detail << " issued-at "
            << to_string(outcome.value().request.issued_at) << '\n';
  return kExitOk;
}

int cli_acknowledge(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--request", "--system", "--observation",
                      "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* request_text = require_value(options, "--request", error);
  const std::string* system = require_value(options, "--system", error);
  if (root == nullptr || plan_text == nullptr || request_text == nullptr || system == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  DrainRequestId request_id;
  if (!to_identity(*plan_text, "plan", plan, error) ||
      !to_identity(*request_text, "request", request_id, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  RecordAcknowledgementRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.request = request_id;
  request.acknowledging_system = *system;
  Result<RecordAcknowledgementOutcome> outcome = coordinator.record_acknowledgement(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  std::cout << "plan " << to_string(plan) << " request " << to_string(outcome.value().request.id)
            << " state " << to_token(outcome.value().request.state) << " system "
            << outcome.value().request.acknowledgement_source << " acknowledged-at "
            << to_string(outcome.value().request.acknowledged_at) << '\n';
  return kExitOk;
}

int cli_ingest(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--domain", "--state", "--generation", "--observed",
                      "--payload-digest", "--manifest-digest", "--residual-count",
                      "--residual-count-unknown", "--source", "--observation", "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* domain_text = require_value(options, "--domain", error);
  const std::string* state_text = require_value(options, "--state", error);
  const std::string* generation_text = require_value(options, "--generation", error);
  const std::string* observed_text = require_value(options, "--observed", error);
  const std::string* payload_text = require_value(options, "--payload-digest", error);
  if (root == nullptr || plan_text == nullptr || domain_text == nullptr || state_text == nullptr ||
      generation_text == nullptr || observed_text == nullptr || payload_text == nullptr) {
    return usage_failure(error);
  }
  if (options.has("--residual-count") && options.has("--residual-count-unknown")) {
    return usage_failure("--residual-count and --residual-count-unknown contradict each other");
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  const std::optional<OwnerDomain> domain = owner_domain_from_token(*domain_text);
  if (!domain.has_value()) {
    return usage_failure("unknown domain token '" + *domain_text + "'");
  }
  const std::optional<CompletionState> state = completion_state_from_token(*state_text);
  if (!state.has_value()) {
    return usage_failure("unknown state token '" + *state_text + "'");
  }
  std::uint64_t generation = 0;
  std::uint64_t observed = 0;
  ContentDigest payload;
  if (!to_u64(*generation_text, "generation", generation, error) ||
      !to_u64(*observed_text, "observed", observed, error) ||
      !to_digest(*payload_text, "payload-digest", payload, error)) {
    return usage_failure(error);
  }

  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  Result<DrainPlanSnapshot> snapshot = coordinator.plan(plan);
  if (!snapshot) {
    return report_error(snapshot.error(), json);
  }
  const DrainPlanSnapshot& view = snapshot.value();

  ContentDigest manifest;
  if (const std::string* text = options.value("--manifest-digest"); text != nullptr) {
    if (!to_digest(*text, "manifest-digest", manifest, error)) {
      return usage_failure(error);
    }
  } else {
    const std::optional<ContentDigest>& bound = view.spec.bindings.manifest_digest(domain.value());
    if (!bound.has_value()) {
      return usage_failure("no consumer manifest digest is bound for " +
                           std::string{to_token(domain.value())} +
                           ", so --manifest-digest is required");
    }
    manifest = bound.value();
  }

  IngestCompletionRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.domain = domain.value();
  request.state = state.value();
  request.generation = EvidenceGeneration{generation};
  request.observed_at = ObservationSequence{observed};
  request.payload_digest = payload;
  request.manifest_digest = manifest;
  request.scope_manifest_digest = view.spec.targets.digest();
  request.generations = view.spec.bindings.generations;
  request.source = options.value("--source") != nullptr ? *options.value("--source") : std::string{"cli"};
  if (const std::string* annotation = options.value("--annotation"); annotation != nullptr) {
    request.annotation = *annotation;
  }
  if (const std::string* count = options.value("--residual-count"); count != nullptr) {
    std::uint64_t value = 0;
    if (!to_u64(*count, "residual-count", value, error)) {
      return usage_failure(error);
    }
    request.residual_count_known = true;
    request.residual_count = value;
  } else if (!options.has("--residual-count-unknown")) {
    request.residual_count_known = false;
    request.residual_count = 0;
  }

  Result<IngestCompletionOutcome> outcome = coordinator.ingest_completion(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  const IngestCompletionOutcome& recorded = outcome.value();
  std::cout << "plan " << to_string(plan) << " completion domain " << to_token(domain.value())
            << " generation " << to_string(recorded.evidence.generation) << " state "
            << to_token(recorded.evidence.state) << " compatible " << yes_no(recorded.compatible)
            << " rejection " << to_token(recorded.rejection) << " residuals-recorded "
            << recorded.residuals_recorded << " fenced " << yes_no(recorded.fenced) << " commit "
            << std::to_string(view.last_commit.value() + 1U) << '\n';
  return kExitOk;
}

int cli_residual_record(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--domain", "--obligation", "--kind", "--detail",
                      "--observation", "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* domain_text = require_value(options, "--domain", error);
  const std::string* kind_text = require_value(options, "--kind", error);
  const std::string* detail = require_value(options, "--detail", error);
  if (root == nullptr || plan_text == nullptr || domain_text == nullptr || kind_text == nullptr ||
      detail == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  const std::optional<OwnerDomain> domain = owner_domain_from_token(*domain_text);
  if (!domain.has_value()) {
    return usage_failure("unknown domain token '" + *domain_text + "'");
  }
  const std::optional<ResidualKind> kind = residual_kind_from_token(*kind_text);
  if (!kind.has_value()) {
    return usage_failure("unknown residual kind token '" + *kind_text + "'");
  }
  std::uint64_t obligation = 0;
  if (const std::string* text = options.value("--obligation"); text != nullptr) {
    if (!to_u64(*text, "obligation", obligation, error)) {
      return usage_failure(error);
    }
  }

  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  RecordResidualRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.entry.domain = domain.value();
  request.entry.obligation = ObligationId{obligation};
  request.entry.kind = kind.value();
  request.entry.detail = *detail;
  Result<RecordResidualOutcome> outcome = coordinator.record_residual(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  std::cout << "plan " << to_string(plan) << " residual " << outcome.value().entry.to_canonical()
            << " fenced " << yes_no(outcome.value().fenced) << " observed "
            << to_string(request.context.observation) << '\n';
  return kExitOk;
}

int cli_residual_resolve(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--domain", "--obligation", "--kind",
                      "--evidence-generation", "--detail", "--observation", "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* domain_text = require_value(options, "--domain", error);
  const std::string* kind_text = require_value(options, "--kind", error);
  const std::string* evidence_text = require_value(options, "--evidence-generation", error);
  if (root == nullptr || plan_text == nullptr || domain_text == nullptr || kind_text == nullptr ||
      evidence_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  const std::optional<OwnerDomain> domain = owner_domain_from_token(*domain_text);
  if (!domain.has_value()) {
    return usage_failure("unknown domain token '" + *domain_text + "'");
  }
  const std::optional<ResidualKind> kind = residual_kind_from_token(*kind_text);
  if (!kind.has_value()) {
    return usage_failure("unknown residual kind token '" + *kind_text + "'");
  }
  std::uint64_t obligation = 0;
  std::uint64_t evidence_generation = 0;
  if (!to_u64(*evidence_text, "evidence-generation", evidence_generation, error)) {
    return usage_failure(error);
  }
  if (const std::string* text = options.value("--obligation"); text != nullptr) {
    if (!to_u64(*text, "obligation", obligation, error)) {
      return usage_failure(error);
    }
  }

  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  ResolveResidualRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.obligation = ObligationId{obligation};
  request.domain = domain.value();
  request.kind = kind.value();
  request.resolution_evidence_generation = EvidenceGeneration{evidence_generation};
  if (const std::string* detail = options.value("--detail"); detail != nullptr) {
    request.detail = *detail;
  }
  Result<ResolveResidualOutcome> outcome = coordinator.resolve_residual(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  std::cout << "plan " << to_string(plan) << " residual " << outcome.value().entry.to_canonical()
            << " observed " << to_string(request.context.observation) << '\n';
  return kExitOk;
}

int cli_residuals(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root", "--plan", "--json"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  if (root == nullptr || plan_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, true, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  if (json) {
    Result<std::string> text = coordinator.export_plan_json(plan);
    if (!text) {
      return report_error(text.error(), json);
    }
    print_text(text.value());
    return kExitOk;
  }
  Result<ResidualLedger> ledger = coordinator.residuals(plan);
  if (!ledger) {
    return report_error(ledger.error(), json);
  }
  const ResidualLedger& entries = ledger.value();
  std::cout << "plan " << to_string(plan) << " residuals total " << entries.entries.size() << " open "
            << entries.open_count() << " unknown " << entries.unknown_count() << '\n';
  for (const ResidualEntry& entry : entries.entries) {
    std::cout << "residual " << entry.to_canonical() << '\n';
  }
  return kExitOk;
}

int cli_evaluate(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root", "--plan", "--json"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  if (root == nullptr || plan_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, true, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  if (json) {
    const TextOutcome text = export_plan_json_text(coordinator, plan);
    if (!text.ok) {
      return report_error(text.error, json);
    }
    print_text(text.text);
    return kExitOk;
  }
  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(plan);
  if (!evaluation) {
    return report_error(evaluation.error(), json);
  }
  print_text(format_evaluation(evaluation.value()));
  return kExitOk;
}

int cli_grant(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--by", "--observation", "--expected-revision"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* by = require_value(options, "--by", error);
  if (root == nullptr || plan_text == nullptr || by == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  GrantSafeToRemoveRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.granted_by = *by;
  Result<GrantSafeToRemoveOutcome> outcome = coordinator.grant_safe_to_remove(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  if (!outcome.value().grant.has_value()) {
    return report_error(Error{ErrorCode::kNotSafeToRemove, "the coordinator recorded no grant"}, json);
  }
  const SafeToRemoveGrant& grant = outcome.value().grant.value();
  std::cout << "plan " << to_string(grant.plan) << " grant granted by " << grant.granted_by
            << " revision " << to_string(grant.revision) << " epoch " << to_string(grant.epoch)
            << " evidence " << grant.evidence_digest.to_hex() << " manifest "
            << grant.manifest_digest.to_hex() << " commit " << to_string(grant.granted_commit) << '\n';
  return kExitOk;
}

int cli_fence(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--reason", "--detail", "--observation",
                      "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* reason_text = require_value(options, "--reason", error);
  const std::string* detail = require_value(options, "--detail", error);
  if (root == nullptr || plan_text == nullptr || reason_text == nullptr || detail == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  const std::optional<FenceReason> reason = fence_reason_from_token(*reason_text);
  if (!reason.has_value() || (reason.value() != FenceReason::kOperatorFence &&
                              reason.value() != FenceReason::kNewObligation &&
                              reason.value() != FenceReason::kDependencyChange)) {
    return usage_failure("--reason accepts operator-fence, new-obligation or dependency-change for an "
                         "operator fence");
  }
  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  FenceSafeToRemoveRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.reason = reason.value();
  request.detail = *detail;
  Result<FenceSafeToRemoveOutcome> outcome = coordinator.fence_safe_to_remove(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  std::cout << "plan " << to_string(plan) << " fence reason "
            << to_token(outcome.value().fence.reason) << " floor " << to_string(outcome.value().fence.floor)
            << " commit " << to_string(outcome.value().fence.commit) << " had-live-grant "
            << yes_no(outcome.value().had_live_grant) << " detail " << outcome.value().fence.detail << '\n';
  return kExitOk;
}

int cli_cancel(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--reason", "--observation", "--expected-revision"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* reason = require_value(options, "--reason", error);
  if (root == nullptr || plan_text == nullptr || reason == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  CancelPlanRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.reason = *reason;
  Result<CancelPlanOutcome> outcome = coordinator.cancel_plan(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  std::cout << "plan " << to_string(plan) << " cancelled state " << to_token(outcome.value().plan.state)
            << " requests-cancelled " << outcome.value().requests_cancelled << " fenced "
            << yes_no(outcome.value().fenced) << " commit "
            << to_string(outcome.value().plan.last_commit) << '\n';
  return kExitOk;
}

int cli_fail(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--reason", "--observation", "--expected-revision"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* reason = require_value(options, "--reason", error);
  if (root == nullptr || plan_text == nullptr || reason == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  FailPlanRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.reason = *reason;
  Result<FailPlanOutcome> outcome = coordinator.fail_plan(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  std::cout << "plan " << to_string(plan) << " failed state " << to_token(outcome.value().plan.state)
            << " fenced " << yes_no(outcome.value().fenced) << " commit "
            << to_string(outcome.value().plan.last_commit) << '\n';
  return kExitOk;
}

int cli_supersede(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options,
                     {"--root", "--plan", "--request", "--reason", "--observation",
                      "--expected-revision"},
                     error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  const std::string* request_text = require_value(options, "--request", error);
  const std::string* reason = require_value(options, "--reason", error);
  if (root == nullptr || plan_text == nullptr || request_text == nullptr || reason == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  DrainRequestId request_id;
  if (!to_identity(*plan_text, "plan", plan, error) ||
      !to_identity(*request_text, "request", request_id, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, false, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  SupersedeRequestRequest request;
  if (!build_context(coordinator, plan, options, "fdc", request.context, error)) {
    return usage_failure(error);
  }
  request.request = request_id;
  request.reason = *reason;
  Result<SupersedeRequestOutcome> outcome = coordinator.supersede_request(request);
  if (!outcome) {
    return report_error(outcome.error(), json);
  }
  const SupersedeRequestOutcome& superseded = outcome.value();
  std::cout << "plan " << to_string(plan) << " request " << to_string(superseded.superseded.id)
            << " superseded attempt " << to_string(superseded.superseded.key.attempt) << " detail "
            << superseded.superseded.settlement_detail << '\n';
  std::cout << "plan " << to_string(plan) << " request " << to_string(superseded.replacement.id)
            << " staged attempt " << to_string(superseded.replacement.key.attempt) << " bound "
            << superseded.replacement.bound_operations << " key "
            << superseded.replacement.idempotency_key.to_hex() << " deliver yes\n";
  return kExitOk;
}

int cli_explain(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root", "--plan"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  const std::string* plan_text = require_value(options, "--plan", error);
  if (root == nullptr || plan_text == nullptr) {
    return usage_failure(error);
  }
  PlanId plan;
  if (!to_identity(*plan_text, "plan", plan, error)) {
    return usage_failure(error);
  }
  OpenOutcome opened = open_store(*root, true, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Result<std::string> text = opened.coordinator.value().explain(plan);
  if (!text) {
    return report_error(text.error(), json);
  }
  print_text(text.value());
  return kExitOk;
}

int cli_export(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root", "--plan", "--json"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  if (root == nullptr) {
    return usage_failure(error);
  }
  std::optional<PlanId> plan;
  if (const std::string* text = options.value("--plan"); text != nullptr) {
    PlanId parsed;
    if (!to_identity(*text, "plan", parsed, error)) {
      return usage_failure(error);
    }
    plan = parsed;
  }
  OpenOutcome opened = open_store(*root, true, false);
  if (!opened.coordinator.has_value()) {
    return report_error(opened.error, json);
  }
  Coordinator& coordinator = opened.coordinator.value();
  Result<std::string> text =
      plan.has_value() ? (json ? coordinator.export_plan_json(plan.value())
                               : coordinator.export_plan_text(plan.value()))
                       : (json ? coordinator.export_json() : coordinator.export_text());
  if (!text) {
    return report_error(text.error(), json);
  }
  print_text(text.value());
  return kExitOk;
}

int cli_store_inspect(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  if (root == nullptr) {
    return usage_failure(error);
  }
  Result<StoreInspection> inspection =
      inspect_store(std::filesystem::path(*root), Limits{});
  if (!inspection) {
    return report_error(inspection.error(), json);
  }
  const StoreInspection& view = inspection.value();
  std::cout << "store exists " << yes_no(view.exists) << " pointer " << yes_no(view.has_pointer)
            << " sequence " << to_string(view.sequence) << " epoch " << to_string(view.epoch)
            << " generation-files " << view.generation_files << " transient-files "
            << view.transient_files << " bytes " << view.bytes << " writer-lock "
            << (view.writer_lock_present ? "present" : "absent") << " digest "
            << (view.state_digest.is_zero() ? std::string{"unset"} : view.state_digest.to_hex())
            << " lock-record " << (view.writer_lock_record.empty() ? std::string{"none"}
                                                                   : view.writer_lock_record)
            << '\n';
  return kExitOk;
}

// ---------------------------------------------------------------------------
// The reproducible scenario
// ---------------------------------------------------------------------------
//
// One deterministic end to end drain, used both in memory (--self-check) and
// against a real durable store (scenario). Every expectation is stated before
// it is checked, and the summary names the verdicts that were verified.

struct CheckReport {
  std::uint32_t checks = 0;
  std::string failure{};
};

bool expect(bool condition, const std::string& what, CheckReport& report) {
  ++report.checks;
  if (!condition) {
    report.failure = what;
    return false;
  }
  return true;
}

[[nodiscard]] std::string state_token(const DrainPlanSnapshot& plan) {
  return std::string{to_token(plan.state)};
}

[[nodiscard]] std::string verdict_token(const SafeToRemoveEvaluation& evaluation) {
  return std::string{to_token(evaluation.verdict)};
}

[[nodiscard]] ConsumerRecord make_consumer(ConsumerCategory category, std::uint64_t obligation,
                                           std::uint64_t generation, ObligationStrength strength,
                                           std::string label, std::string source) {
  ConsumerRecord record;
  record.obligation = ObligationId{obligation};
  record.category = category;
  record.generation = ObligationGeneration{generation};
  record.reservation = ReservationGeneration{0};
  record.strength = strength;
  record.label = std::move(label);
  record.source = std::move(source);
  return record;
}

int self_check_failure(const CheckReport& report) {
  std::cout.flush();
  std::cerr << "self-check failed after " << report.checks << " checks: " << report.failure << '\n';
  return kExitRejected;
}

int run_self_check(bool json) {
  EphemeralOptions options;
  options.clock = std::shared_ptr<const Clock>{std::make_shared<FixedClock>(kReproducibleMilliseconds)};
  Result<Coordinator> opened = Coordinator::open_ephemeral(options);
  if (!opened) {
    return report_error(opened.error(), json);
  }
  Coordinator coordinator = std::move(opened).value();
  CheckReport report;

  const PlanId plan_id{1};
  const DrainScope scope{ScopeKind::kAsset, 17};
  const GenerationSet generations = complete_generations();
  const ContentDigest policy_digest = digest_text("self-check-policy");
  const std::vector<ConsumerRecord> consumers{
      make_consumer(ConsumerCategory::kWorkload, 1001, 1, ObligationStrength::kMandatory, "workload",
                    "self-check"),
      make_consumer(ConsumerCategory::kNetworkPath, 2001, 1, ObligationStrength::kMandatory, "path",
                    "self-check")};

  CreatePlanRequest create;
  create.context.plan = plan_id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "self-check";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  create.generations = generations;
  create.policy_id = PolicyId{9};
  create.policy_digest = policy_digest;
  create.consumers = consumers;
  create.label = "self-check";
  create.requested_by = "self-check";
  Result<CreatePlanOutcome> created = coordinator.create_plan(create);
  if (!created) {
    return report_error(created.error(), json);
  }
  if (!expect(created.value().plan.state == DrainState::kProposed,
              "creation derives the proposed state, got " + state_token(created.value().plan), report)) {
    return self_check_failure(report);
  }
  if (!expect(created.value().plan.spec.revision == Revision{1}, "creation names revision 1", report)) {
    return self_check_failure(report);
  }
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    if (!expect(created.value().plan.spec.bindings.manifest_digest(owner_domain_at(index)).has_value(),
                "creation binds every domain manifest digest", report)) {
      return self_check_failure(report);
    }
  }
  Result<SafeToRemoveEvaluation> first = coordinator.evaluate_safe_to_remove(plan_id);
  if (!first) {
    return report_error(first.error(), json);
  }
  if (!expect(first.value().verdict == SafeToRemoveVerdict::kDenied &&
                  first.value().primary_blocking_code == ErrorCode::kIncompleteEnumeration,
              "before any evidence the verdict is denied for an incomplete enumeration, got " +
                  verdict_token(first.value()) + "/" + std::string{to_token(first.value().primary_blocking_code)},
              report)) {
    return self_check_failure(report);
  }

  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();
  std::uint64_t generation = 1;
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!create.declared_required_domains.contains(domain)) {
      continue;
    }
    Result<DrainPlanSnapshot> view = coordinator.plan(plan_id);
    if (!view) {
      return report_error(view.error(), json);
    }
    RecordEnumerationRequest enumeration;
    enumeration.context.plan = plan_id;
    enumeration.context.expected_revision = view.value().spec.revision;
    enumeration.context.incarnation = coordinator.incarnation();
    enumeration.context.expected_epoch = coordinator.control_epoch();
    enumeration.context.observation = ObservationSequence{view.value().last_observation.value() + 1U};
    enumeration.context.principal = "self-check";
    enumeration.domain = domain;
    enumeration.coverage = CoverageState::kComplete;
    enumeration.generation = EvidenceGeneration{generation};
    enumeration.observed_at = enumeration.context.observation;
    enumeration.scope_manifest_digest = scope_digest;
    enumeration.generations = generations;
    enumeration.source = "self-check";
    for (const ConsumerRecord& consumer : consumers) {
      if (consumer.domain() == domain) {
        enumeration.consumers.push_back(consumer);
      }
    }
    Result<RecordEnumerationOutcome> recorded = coordinator.record_enumeration(enumeration);
    if (!recorded) {
      return report_error(recorded.error(), json);
    }
    if (!expect(recorded.value().accepted, "the enumeration for the declared manifest is accepted",
                report)) {
      return self_check_failure(report);
    }
  }

  Result<DrainPlanSnapshot> before_issue = coordinator.plan(plan_id);
  if (!before_issue) {
    return report_error(before_issue.error(), json);
  }
  IssueRequestsRequest issue;
  issue.context.plan = plan_id;
  issue.context.expected_revision = before_issue.value().spec.revision;
  issue.context.incarnation = coordinator.incarnation();
  issue.context.expected_epoch = coordinator.control_epoch();
  issue.context.observation = ObservationSequence{before_issue.value().last_observation.value() + 1U};
  issue.context.principal = "self-check";
  issue.domains = before_issue.value().required_domains;
  Result<IssueRequestsOutcome> issued = coordinator.issue_requests(issue);
  if (!issued) {
    return report_error(issued.error(), json);
  }
  if (!expect(issued.value().newly_staged == 2 && issued.value().to_deliver.size() == 2,
              "one bounded request is staged per domain with obligations, got " +
                  std::to_string(issued.value().newly_staged),
              report)) {
    return self_check_failure(report);
  }

  for (const DrainRequest& staged : issued.value().to_deliver) {
    Result<DrainPlanSnapshot> view = coordinator.plan(plan_id);
    if (!view) {
      return report_error(view.error(), json);
    }
    ConfirmDeliveryRequest delivery;
    delivery.context.plan = plan_id;
    delivery.context.expected_revision = view.value().spec.revision;
    delivery.context.incarnation = coordinator.incarnation();
    delivery.context.expected_epoch = coordinator.control_epoch();
    delivery.context.observation = ObservationSequence{view.value().last_observation.value() + 1U};
    delivery.context.principal = "self-check";
    delivery.request = staged.id;
    delivery.delivery_reference = "self-check-delivery";
    Result<ConfirmDeliveryOutcome> delivered = coordinator.confirm_delivery(delivery);
    if (!delivered) {
      return report_error(delivered.error(), json);
    }
    if (!expect(delivered.value().request.state == RequestState::kIssued,
                "confirming delivery moves the request to issued", report)) {
      return self_check_failure(report);
    }

    Result<DrainPlanSnapshot> after_delivery = coordinator.plan(plan_id);
    if (!after_delivery) {
      return report_error(after_delivery.error(), json);
    }
    RecordAcknowledgementRequest acknowledgement;
    acknowledgement.context.plan = plan_id;
    acknowledgement.context.expected_revision = after_delivery.value().spec.revision;
    acknowledgement.context.incarnation = coordinator.incarnation();
    acknowledgement.context.expected_epoch = coordinator.control_epoch();
    acknowledgement.context.observation =
        ObservationSequence{after_delivery.value().last_observation.value() + 1U};
    acknowledgement.context.principal = "self-check";
    acknowledgement.request = staged.id;
    acknowledgement.acknowledging_system = std::string{to_token(staged.key.domain)} + "-controller";
    Result<RecordAcknowledgementOutcome> acknowledged = coordinator.record_acknowledgement(acknowledgement);
    if (!acknowledged) {
      return report_error(acknowledged.error(), json);
    }
    if (!expect(acknowledged.value().request.state == RequestState::kAcknowledged,
                "an acknowledgement is recorded but never completes the domain", report)) {
      return self_check_failure(report);
    }
  }

  Result<DrainPlanSnapshot> acknowledged_plan = coordinator.plan(plan_id);
  if (!acknowledged_plan) {
    return report_error(acknowledged_plan.error(), json);
  }
  if (!expect(acknowledged_plan.value().state == DrainState::kDraining,
              "an acknowledged request derives the draining state, got " +
                  state_token(acknowledged_plan.value()),
              report)) {
    return self_check_failure(report);
  }

  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!create.declared_required_domains.contains(domain)) {
      continue;
    }
    Result<DrainPlanSnapshot> view = coordinator.plan(plan_id);
    if (!view) {
      return report_error(view.error(), json);
    }
    IngestCompletionRequest completion;
    completion.context.plan = plan_id;
    completion.context.expected_revision = view.value().spec.revision;
    completion.context.incarnation = coordinator.incarnation();
    completion.context.expected_epoch = coordinator.control_epoch();
    completion.context.observation = ObservationSequence{view.value().last_observation.value() + 1U};
    completion.context.principal = "self-check";
    completion.domain = domain;
    completion.state = CompletionState::kDrained;
    completion.generation = EvidenceGeneration{2};
    completion.observed_at = completion.context.observation;
    completion.payload_digest = digest_text(std::string{"drained-report-"} + std::string{to_token(domain)});
    completion.manifest_digest = view.value().spec.bindings.manifest_digest(domain).value();
    completion.scope_manifest_digest = scope_digest;
    completion.generations = generations;
    completion.residual_count_known = true;
    completion.residual_count = 0;
    completion.source = "self-check";
    Result<IngestCompletionOutcome> ingested = coordinator.ingest_completion(completion);
    if (!ingested) {
      return report_error(ingested.error(), json);
    }
    if (!expect(ingested.value().compatible && ingested.value().rejection == ErrorCode::kOk,
                "a completion that agrees with the binding is compatible", report)) {
      return self_check_failure(report);
    }
  }

  Result<SafeToRemoveEvaluation> granted = coordinator.evaluate_safe_to_remove(plan_id);
  if (!granted) {
    return report_error(granted.error(), json);
  }
  if (!expect(granted.value().verdict == SafeToRemoveVerdict::kGranted,
              "complete compatible evidence grants the verdict, got " + verdict_token(granted.value()),
              report)) {
    return self_check_failure(report);
  }
  Result<DrainPlanSnapshot> drained = coordinator.plan(plan_id);
  if (!drained) {
    return report_error(drained.error(), json);
  }
  if (!expect(drained.value().state == DrainState::kDrained,
              "proven domains derive the drained state, got " + state_token(drained.value()), report)) {
    return self_check_failure(report);
  }

  GrantSafeToRemoveRequest grant;
  grant.context.plan = plan_id;
  grant.context.expected_revision = drained.value().spec.revision;
  grant.context.incarnation = coordinator.incarnation();
  grant.context.expected_epoch = coordinator.control_epoch();
  grant.context.observation = ObservationSequence{drained.value().last_observation.value() + 1U};
  grant.context.principal = "self-check";
  grant.granted_by = "self-check";
  Result<GrantSafeToRemoveOutcome> granted_outcome = coordinator.grant_safe_to_remove(grant);
  if (!granted_outcome) {
    return report_error(granted_outcome.error(), json);
  }
  Result<DrainPlanSnapshot> safe = coordinator.plan(plan_id);
  if (!safe) {
    return report_error(safe.error(), json);
  }
  if (!expect(safe.value().state == DrainState::kSafeToRemove && safe.value().grant_live,
              "a recorded grant derives the safe to remove state, got " + state_token(safe.value()),
              report)) {
    return self_check_failure(report);
  }

  FenceSafeToRemoveRequest fence;
  fence.context.plan = plan_id;
  fence.context.expected_revision = safe.value().spec.revision;
  fence.context.incarnation = coordinator.incarnation();
  fence.context.expected_epoch = coordinator.control_epoch();
  fence.context.observation = ObservationSequence{safe.value().last_observation.value() + 1U};
  fence.context.principal = "self-check";
  fence.reason = FenceReason::kOperatorFence;
  fence.detail = "self-check fence";
  Result<FenceSafeToRemoveOutcome> fenced = coordinator.fence_safe_to_remove(fence);
  if (!fenced) {
    return report_error(fenced.error(), json);
  }
  if (!expect(fenced.value().had_live_grant, "the fence withdrew a live grant", report)) {
    return self_check_failure(report);
  }
  Result<SafeToRemoveEvaluation> after_fence = coordinator.evaluate_safe_to_remove(plan_id);
  if (!after_fence) {
    return report_error(after_fence.error(), json);
  }
  if (!expect(after_fence.value().verdict == SafeToRemoveVerdict::kDenied &&
                  after_fence.value().primary_blocking_code == ErrorCode::kStaleEvidence,
              "evidence at or before the fence floor cannot support a new verdict, got " +
                  verdict_token(after_fence.value()) + "/" +
                  std::string{to_token(after_fence.value().primary_blocking_code)},
              report)) {
    return self_check_failure(report);
  }

  std::cout << "self-check ok checks=" << report.checks << " plan=" << to_string(plan_id)
            << " grant-verdict=" << verdict_token(granted.value())
            << " grant-state=" << state_token(safe.value())
            << " fence-verdict=" << verdict_token(after_fence.value())
            << " fence-primary=" << to_token(after_fence.value().primary_blocking_code)
            << " evidence=" << granted.value().evidence_digest.to_hex() << '\n';
  return kExitOk;
}

// ---------------------------------------------------------------------------
// The durable scenario
// ---------------------------------------------------------------------------

struct PhaseOutcome {
  bool ok = false;
  /// Only the verdict phase sets this: true when the recovered plan carries a
  /// granted safe to remove verdict.
  bool granted = false;
  std::string text{};
  Error error{};
};

[[nodiscard]] PlanId scenario_plan_id() { return PlanId{1}; }

[[nodiscard]] DrainScope scenario_scope() { return DrainScope{ScopeKind::kAsset, 1}; }

PhaseOutcome scenario_phase_create(const std::string& root) {
  PhaseOutcome phase;
  OpenOutcome opened = open_store(root, false, true);
  if (!opened.coordinator.has_value()) {
    phase.error = opened.error;
    return phase;
  }
  Coordinator& coordinator = opened.coordinator.value();
  const PlanId plan_id = scenario_plan_id();
  Result<DrainPlanSnapshot> existing = coordinator.plan(plan_id);
  if (existing) {
    phase.ok = true;
    phase.text = "phase create plan=" + to_string(plan_id) + " created=no revision " +
                 to_string(existing.value().spec.revision) + " state " +
                 state_token(existing.value());
    return phase;
  }

  const DrainScope scope = scenario_scope();
  const GenerationSet generations = complete_generations();
  const std::vector<ConsumerRecord> consumers{
      make_consumer(ConsumerCategory::kWorkload, 1001, 1, ObligationStrength::kMandatory, "workload",
                    "scenario"),
      make_consumer(ConsumerCategory::kNetworkPath, 2001, 1, ObligationStrength::kMandatory, "path",
                    "scenario")};

  CreatePlanRequest create;
  create.context.plan = plan_id;
  create.context.expected_revision = Revision{1};
  create.context.incarnation = coordinator.incarnation();
  create.context.expected_epoch = coordinator.control_epoch();
  create.context.observation = ObservationSequence{coordinator.observation_sequence().value() + 1U};
  create.context.principal = "scenario";
  create.id = plan_id;
  create.scope = scope;
  create.targets.push_back(scope);
  create.declared_required_domains = DomainMask::of(OwnerDomain::kAsi).with(OwnerDomain::kDfi);
  create.generations = generations;
  create.policy_id = PolicyId{7};
  create.policy_digest = digest_text("scenario-policy");
  create.consumers = consumers;
  create.label = "scenario";
  create.requested_by = "scenario";
  Result<CreatePlanOutcome> created = coordinator.create_plan(create);
  if (!created) {
    phase.error = created.error();
    return phase;
  }
  const ContentDigest scope_digest = created.value().plan.spec.targets.digest();
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!create.declared_required_domains.contains(domain)) {
      continue;
    }
    Result<DrainPlanSnapshot> view = coordinator.plan(plan_id);
    if (!view) {
      phase.error = view.error();
      return phase;
    }
    RecordEnumerationRequest enumeration;
    enumeration.context.plan = plan_id;
    enumeration.context.expected_revision = view.value().spec.revision;
    enumeration.context.incarnation = coordinator.incarnation();
    enumeration.context.expected_epoch = coordinator.control_epoch();
    enumeration.context.observation = ObservationSequence{view.value().last_observation.value() + 1U};
    enumeration.context.principal = "scenario";
    enumeration.domain = domain;
    enumeration.coverage = CoverageState::kComplete;
    enumeration.generation = EvidenceGeneration{1};
    enumeration.observed_at = enumeration.context.observation;
    enumeration.scope_manifest_digest = scope_digest;
    enumeration.generations = generations;
    enumeration.source = "scenario";
    for (const ConsumerRecord& consumer : consumers) {
      if (consumer.domain() == domain) {
        enumeration.consumers.push_back(consumer);
      }
    }
    Result<RecordEnumerationOutcome> recorded = coordinator.record_enumeration(enumeration);
    if (!recorded) {
      phase.error = recorded.error();
      return phase;
    }
  }

  Result<DrainPlanSnapshot> before_issue = coordinator.plan(plan_id);
  if (!before_issue) {
    phase.error = before_issue.error();
    return phase;
  }
  IssueRequestsRequest issue;
  issue.context.plan = plan_id;
  issue.context.expected_revision = before_issue.value().spec.revision;
  issue.context.incarnation = coordinator.incarnation();
  issue.context.expected_epoch = coordinator.control_epoch();
  issue.context.observation = ObservationSequence{before_issue.value().last_observation.value() + 1U};
  issue.context.principal = "scenario";
  issue.domains = before_issue.value().required_domains;
  Result<IssueRequestsOutcome> issued = coordinator.issue_requests(issue);
  if (!issued) {
    phase.error = issued.error();
    return phase;
  }
  for (const DrainRequest& staged : issued.value().to_deliver) {
    Result<DrainPlanSnapshot> view = coordinator.plan(plan_id);
    if (!view) {
      phase.error = view.error();
      return phase;
    }
    ConfirmDeliveryRequest delivery;
    delivery.context.plan = plan_id;
    delivery.context.expected_revision = view.value().spec.revision;
    delivery.context.incarnation = coordinator.incarnation();
    delivery.context.expected_epoch = coordinator.control_epoch();
    delivery.context.observation = ObservationSequence{view.value().last_observation.value() + 1U};
    delivery.context.principal = "scenario";
    delivery.request = staged.id;
    delivery.delivery_reference = "scenario-delivery";
    Result<ConfirmDeliveryOutcome> delivered = coordinator.confirm_delivery(delivery);
    if (!delivered) {
      phase.error = delivered.error();
      return phase;
    }
    Result<DrainPlanSnapshot> after_delivery = coordinator.plan(plan_id);
    if (!after_delivery) {
      phase.error = after_delivery.error();
      return phase;
    }
    RecordAcknowledgementRequest acknowledgement;
    acknowledgement.context.plan = plan_id;
    acknowledgement.context.expected_revision = after_delivery.value().spec.revision;
    acknowledgement.context.incarnation = coordinator.incarnation();
    acknowledgement.context.expected_epoch = coordinator.control_epoch();
    acknowledgement.context.observation =
        ObservationSequence{after_delivery.value().last_observation.value() + 1U};
    acknowledgement.context.principal = "scenario";
    acknowledgement.request = staged.id;
    acknowledgement.acknowledging_system = std::string{to_token(staged.key.domain)} + "-controller";
    Result<RecordAcknowledgementOutcome> acknowledged = coordinator.record_acknowledgement(acknowledgement);
    if (!acknowledged) {
      phase.error = acknowledged.error();
      return phase;
    }
  }

  Result<DrainPlanSnapshot> final_view = coordinator.plan(plan_id);
  if (!final_view) {
    phase.error = final_view.error();
    return phase;
  }
  phase.ok = true;
  phase.text = "phase create plan=" + to_string(plan_id) + " created=yes revision " +
               to_string(final_view.value().spec.revision) + " state " + state_token(final_view.value());
  return phase;
}

PhaseOutcome scenario_phase_evidence(const std::string& root) {
  PhaseOutcome phase;
  OpenOutcome opened = open_store(root, false, true);
  if (!opened.coordinator.has_value()) {
    phase.error = opened.error;
    return phase;
  }
  Coordinator& coordinator = opened.coordinator.value();
  const PlanId plan_id = scenario_plan_id();
  const RecoveryReport& recovery = coordinator.recovery();
  std::uint32_t ingested = 0;
  std::uint32_t recorded_enumerations = 0;

  Result<DrainPlanSnapshot> view = coordinator.plan(plan_id);
  if (!view) {
    phase.error = view.error();
    return phase;
  }
  const ContentDigest scope_digest = view.value().spec.targets.digest();
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (!view.value().required_domains.contains(domain)) {
      continue;
    }
    const DomainEnumerationSnapshot& enumeration = view.value().enumerations[domain_index(domain)];
    if (!enumeration.present || !enumeration.accepted ||
        enumeration.evidence.coverage != CoverageState::kComplete) {
      Result<DrainPlanSnapshot> fresh = coordinator.plan(plan_id);
      if (!fresh) {
        phase.error = fresh.error();
        return phase;
      }
      RecordEnumerationRequest request;
      request.context.plan = plan_id;
      request.context.expected_revision = fresh.value().spec.revision;
      request.context.incarnation = coordinator.incarnation();
      request.context.expected_epoch = coordinator.control_epoch();
      request.context.observation = ObservationSequence{fresh.value().last_observation.value() + 1U};
      request.context.principal = "scenario";
      request.domain = domain;
      request.coverage = CoverageState::kComplete;
      std::uint64_t generation = 0;
      if (enumeration.present) {
        generation = enumeration.evidence.generation.value();
      }
      request.generation = EvidenceGeneration{generation + 1U};
      request.observed_at = request.context.observation;
      request.scope_manifest_digest = scope_digest;
      request.generations = fresh.value().spec.bindings.generations;
      request.source = "scenario";
      for (const ConsumerRecord& consumer : fresh.value().consumers) {
        if (consumer.domain() == domain) {
          request.consumers.push_back(consumer);
        }
      }
      Result<RecordEnumerationOutcome> recorded = coordinator.record_enumeration(request);
      if (!recorded) {
        phase.error = recorded.error();
        return phase;
      }
      ++recorded_enumerations;
    }

    Result<DrainPlanSnapshot> current = coordinator.plan(plan_id);
    if (!current) {
      phase.error = current.error();
      return phase;
    }
    const DomainCompletionSnapshot& completion = current.value().completions[domain_index(domain)];
    if (completion.present && completion.compatible) {
      continue;
    }
    std::uint64_t generation = current.value().completions[domain_index(domain)].present
                                   ? completion.evidence.generation.value()
                                   : 0;
    const std::uint64_t enumeration_generation =
        current.value().enumerations[domain_index(domain)].present
            ? current.value().enumerations[domain_index(domain)].evidence.generation.value()
            : 0;
    if (enumeration_generation > generation) {
      generation = enumeration_generation;
    }
    const std::optional<ContentDigest>& bound = current.value().spec.bindings.manifest_digest(domain);
    if (!bound.has_value()) {
      phase.error = Error{ErrorCode::kConsumerDigestMismatch,
                          "no consumer manifest digest is bound for " + std::string{to_token(domain)}};
      return phase;
    }
    IngestCompletionRequest request;
    request.context.plan = plan_id;
    request.context.expected_revision = current.value().spec.revision;
    request.context.incarnation = coordinator.incarnation();
    request.context.expected_epoch = coordinator.control_epoch();
    request.context.observation = ObservationSequence{current.value().last_observation.value() + 1U};
    request.context.principal = "scenario";
    request.domain = domain;
    request.state = CompletionState::kDrained;
    request.generation = EvidenceGeneration{generation + 1U};
    request.observed_at = request.context.observation;
    request.payload_digest = digest_text(std::string{"scenario-drained-"} + std::string{to_token(domain)});
    request.manifest_digest = bound.value();
    request.scope_manifest_digest = scope_digest;
    request.generations = current.value().spec.bindings.generations;
    request.residual_count_known = true;
    request.residual_count = 0;
    request.source = "scenario";
    Result<IngestCompletionOutcome> recorded = coordinator.ingest_completion(request);
    if (!recorded) {
      phase.error = recorded.error();
      return phase;
    }
    ++ingested;
  }

  phase.ok = true;
  phase.text = "phase evidence recovered=" + std::string{yes_no(recovery.recovered)} + " sequence " +
               to_string(recovery.recovered_sequence) + " previous-epoch " +
               to_string(recovery.previous_epoch) + " current-epoch " + to_string(recovery.current_epoch) +
               " grants-fenced " + std::to_string(recovery.grants_fenced) + " enumerations " +
               std::to_string(recorded_enumerations) + " ingests " + std::to_string(ingested);
  return phase;
}

PhaseOutcome scenario_phase_verdict(const std::string& root, bool want_json) {
  PhaseOutcome phase;
  OpenOutcome opened = open_store(root, false, true);
  if (!opened.coordinator.has_value()) {
    phase.error = opened.error;
    return phase;
  }
  Coordinator& coordinator = opened.coordinator.value();
  const PlanId plan_id = scenario_plan_id();
  Result<SafeToRemoveEvaluation> evaluation = coordinator.evaluate_safe_to_remove(plan_id);
  if (!evaluation) {
    phase.error = evaluation.error();
    return phase;
  }
  Result<DrainPlanSnapshot> view = coordinator.plan(plan_id);
  if (!view) {
    phase.error = view.error();
    return phase;
  }

  bool regranted = false;
  if (evaluation.value().verdict == SafeToRemoveVerdict::kGranted && !view.value().grant_live) {
    GrantSafeToRemoveRequest grant;
    grant.context.plan = plan_id;
    grant.context.expected_revision = view.value().spec.revision;
    grant.context.incarnation = coordinator.incarnation();
    grant.context.expected_epoch = coordinator.control_epoch();
    grant.context.observation = ObservationSequence{view.value().last_observation.value() + 1U};
    grant.context.principal = "scenario";
    grant.granted_by = "scenario";
    Result<GrantSafeToRemoveOutcome> granted = coordinator.grant_safe_to_remove(grant);
    if (!granted) {
      phase.error = granted.error();
      return phase;
    }
    regranted = true;
  }

  Result<DrainPlanSnapshot> final_view = coordinator.plan(plan_id);
  if (!final_view) {
    phase.error = final_view.error();
    return phase;
  }
  Result<SafeToRemoveEvaluation> final_evaluation = coordinator.evaluate_safe_to_remove(plan_id);
  if (!final_evaluation) {
    phase.error = final_evaluation.error();
    return phase;
  }
  phase.granted = final_evaluation.value().verdict == SafeToRemoveVerdict::kGranted;
  if (want_json) {
    Result<std::string> text = coordinator.export_json();
    if (!text) {
      phase.error = text.error();
      return phase;
    }
    phase.ok = true;
    phase.text = std::move(text).value();
    return phase;
  }
  phase.ok = true;
  phase.text = "scenario plan=" + to_string(plan_id) + " revision " +
               to_string(final_view.value().spec.revision) + " state " + state_token(final_view.value()) +
               " verdict " + verdict_token(final_evaluation.value()) + " evidence " +
               final_evaluation.value().evidence_digest.to_hex() + " grant-live " +
               yes_no(final_view.value().grant_live) + " re-granted " + yes_no(regranted) + " commit " +
               to_string(final_view.value().last_commit) + " root " + root;
  return phase;
}

int cli_scenario(const Options& options, bool json) {
  std::string error;
  if (!check_allowed(options, {"--root", "--json"}, error)) {
    return usage_failure(error);
  }
  const std::string* root = require_value(options, "--root", error);
  if (root == nullptr) {
    return usage_failure(error);
  }
  // The phase lines are progress, not the report. In --json mode they go to
  // stderr so that stdout stays one JSON document.
  std::ostream& progress = json ? std::cerr : std::cout;
  const PhaseOutcome create = scenario_phase_create(*root);
  if (!create.ok) {
    return report_error(create.error, json);
  }
  progress << create.text << '\n';
  const PhaseOutcome evidence = scenario_phase_evidence(*root);
  if (!evidence.ok) {
    return report_error(evidence.error, json);
  }
  progress << evidence.text << '\n';
  const PhaseOutcome verdict = scenario_phase_verdict(*root, json);
  if (!verdict.ok) {
    return report_error(verdict.error, json);
  }
  print_text(verdict.text);
  if (!verdict.granted) {
    std::cout.flush();
    std::cerr << "fdc: the recovered plan reached no granted verdict, which is the point of the "
                 "scenario and is reported rather than hidden\n";
    return kExitRejected;
  }
  return kExitOk;
}

// ---------------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------------

struct CommandHelp {
  std::string_view path;
  std::string_view synopsis;
};

constexpr CommandHelp kCommandHelp[] = {
    {"plan create",
     "fdc plan create --root DIR --plan N --scope KIND:ID [--target KIND:ID]... --required asi,dfi "
     "--scope-generation N --dependency-generation N --reservation-generation N "
     "--obligation-generation N --policy-generation N --topology-generation N --maintenance-generation N "
     "--capacity-generation N --hardware-generation N --firmware-generation N --policy-id N "
     "--policy-digest HEX [--label L] [--requested-by WHO] [--consumer-file FILE] [--observation N] "
     "[--json]"},
    {"plan revise",
     "fdc plan revise --root DIR --plan N [--target KIND:ID]... [--required MASK] "
     "[--generation-field NAME=VALUE]... --reason plan-revised|scope-manifest-changed|dependency-change "
     "[--detail D] [--observation N]"},
    {"plan show", "fdc plan show --root DIR --plan N [--json]"},
    {"plan list", "fdc plan list --root DIR [--json]"},
    {"enumerate",
     "fdc enumerate --root DIR --plan N --domain asi|dfi|facility|monitoring "
     "--coverage complete|partial|failed --generation N --observed N --source S "
     "[--consumer-file FILE] [--annotation A] [--observation N]"},
    {"issue", "fdc issue --root DIR --plan N [--domain MASK] [--bound N] [--observation N]"},
    {"deliver", "fdc deliver --root DIR --plan N --request ID [--reference R] [--observation N]"},
    {"acknowledge", "fdc acknowledge --root DIR --plan N --request ID --system S [--observation N]"},
    {"ingest",
     "fdc ingest --root DIR --plan N --domain D --state STATE --generation N --observed N "
     "--payload-digest HEX [--manifest-digest HEX] [--residual-count N | --residual-count-unknown] "
     "[--source S] [--observation N]"},
    {"residual record",
     "fdc residual record --root DIR --plan N --domain D [--obligation N] --kind KIND --detail TEXT "
     "[--observation N]"},
    {"residual resolve",
     "fdc residual resolve --root DIR --plan N --domain D [--obligation N] --kind KIND "
     "--evidence-generation N [--detail D] [--observation N]"},
    {"residuals", "fdc residuals --root DIR --plan N [--json]"},
    {"evaluate", "fdc evaluate --root DIR --plan N [--json]"},
    {"grant", "fdc grant --root DIR --plan N --by WHO [--observation N]"},
    {"fence",
     "fdc fence --root DIR --plan N --reason operator-fence|new-obligation|dependency-change "
     "--detail D [--observation N]"},
    {"cancel", "fdc cancel --root DIR --plan N --reason R [--observation N]"},
    {"fail", "fdc fail --root DIR --plan N --reason R [--observation N]"},
    {"supersede", "fdc supersede --root DIR --plan N --request ID --reason R [--observation N]"},
    {"explain", "fdc explain --root DIR --plan N"},
    {"export", "fdc export --root DIR [--plan N] [--json]"},
    {"store inspect", "fdc store inspect --root DIR"},
    {"scenario", "fdc scenario --root DIR [--json]"},
};

void print_help() {
  std::cout << "usage: fdc <command> [options]\n";
  std::cout << "       fdc --help | --version | --self-check\n\n";
  std::cout << "The tool opens one durable store exactly once per invocation, reads the live\n";
  std::cout << "incarnation, control epoch, plan revision and observation sequence, fills the\n";
  std::cout << "mutation context from them and performs exactly one operation.\n\n";
  std::cout << "commands:\n";
  for (const CommandHelp& entry : kCommandHelp) {
    std::cout << "  " << entry.synopsis << '\n';
  }
  std::cout << "\nnotes:\n";
  std::cout << "  exit codes: 0 ok, 2 usage, 3 rejected by the coordinator, 4 durable or io failure.\n";
  std::cout << "  --observation N overrides the default, which is the plan's last accepted\n";
  std::cout << "    observation plus one, or the coordinator observation sequence plus one for\n";
  std::cout << "    plan creation.\n";
  std::cout << "  --expected-revision N overrides the default of the plan's current revision.\n";
  std::cout << "  --generation-field NAME=VALUE accepts scope, dependency, reservation, obligation,\n";
  std::cout << "    policy, topology, maintenance, capacity, hardware and firmware.\n";
  std::cout << "  --consumer-file FILE is line oriented: '#' starts a comment, blank lines are\n";
  std::cout << "    ignored, and every other line is\n";
  std::cout << "    '<category-token> <obligation-id> <generation> <reservation> <mandatory|advisory>\n";
  std::cout << "     [label] [source]'.\n";
  std::cout << "  --json prints the canonical export from the library and sends a rejection to\n";
  std::cout << "    stderr, so stdout stays one JSON document.\n";
  std::cout << "  --self-check runs the whole scenario in memory, exits 0 only when every\n";
  std::cout << "    expectation holds and exits 3 when one does not.\n";
}

void print_version() {
  std::cout << "fdc " << version_string() << '\n';
  std::cout << "report-format " << kReportFormatVersion << " state-format " << kStateFormatVersion
            << " container-format " << kContainerFormatVersion << " store-layout "
            << kStoreLayoutVersion << '\n';
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

using Handler = int (*)(const Options&, bool);

struct CommandEntry {
  std::string_view path;
  Handler handler;
};

constexpr CommandEntry kCommands[] = {
    {"plan create", &cli_plan_create},   {"plan revise", &cli_plan_revise},
    {"plan show", &cli_plan_show},       {"plan list", &cli_plan_list},
    {"enumerate", &cli_enumerate},       {"issue", &cli_issue},
    {"deliver", &cli_deliver},           {"acknowledge", &cli_acknowledge},
    {"ingest", &cli_ingest},             {"residual record", &cli_residual_record},
    {"residual resolve", &cli_residual_resolve},
    {"residuals", &cli_residuals},       {"evaluate", &cli_evaluate},
    {"grant", &cli_grant},               {"fence", &cli_fence},
    {"cancel", &cli_cancel},             {"fail", &cli_fail},
    {"supersede", &cli_supersede},       {"explain", &cli_explain},
    {"export", &cli_export},             {"store inspect", &cli_store_inspect},
    {"scenario", &cli_scenario},
};

[[nodiscard]] const CommandEntry* find_command(std::string_view path) {
  for (const CommandEntry& entry : kCommands) {
    if (entry.path == path) {
      return &entry;
    }
  }
  return nullptr;
}

[[nodiscard]] const CommandHelp* find_help(std::string_view path) {
  for (const CommandHelp& entry : kCommandHelp) {
    if (entry.path == path) {
      return &entry;
    }
  }
  return nullptr;
}

int fdc_main(int argc, char** argv) {
  std::vector<std::string> words;
  collect_words(argc, argv, words);
  bool json_requested = false;
  for (const std::string& word : words) {
    if (word == "--json") {
      json_requested = true;
    }
  }
  if (words.empty()) {
    std::cout.flush();
    std::cerr << "fdc: no command was given\n";
    std::cerr << "fdc: run 'fdc --help' for the accepted commands and options\n";
    return kExitUsage;
  }
  const std::string& first = words[0];
  if (first == "--help" || first == "-h") {
    print_help();
    return kExitOk;
  }
  if (first == "--version") {
    print_version();
    return kExitOk;
  }
  if (first == "--self-check") {
    if (words.size() != 1) {
      return usage_failure("--self-check takes no further argument");
    }
    return run_self_check(json_requested);
  }

  std::string path = first;
  std::size_t offset = 1;
  if (first == "plan" || first == "residual" || first == "store") {
    if (words.size() < 2) {
      return usage_failure("'" + first + "' requires a subcommand");
    }
    path = first + " " + words[1];
    offset = 2;
  }
  const CommandEntry* command = find_command(path);
  if (command == nullptr) {
    return usage_failure("unknown command '" + path + "'");
  }

  std::vector<std::string> tail;
  tail.reserve(words.size() - offset);
  for (std::size_t index = offset; index < words.size(); ++index) {
    tail.push_back(words[index]);
  }
  Options options;
  std::string error;
  if (!parse_options(tail, options, error)) {
    return usage_failure(error);
  }
  if (options.has("--help")) {
    const CommandHelp* help = find_help(path);
    if (help != nullptr) {
      std::cout << help->synopsis << '\n';
    }
    return kExitOk;
  }
  return command->handler(options, json_requested);
}

}  // namespace

int main(int argc, char** argv) { return fdc_main(argc, argv); }
