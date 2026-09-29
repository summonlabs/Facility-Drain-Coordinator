// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.

#include "facilitydrain/report.hpp"

#include "facilitydrain/consumer.hpp"
#include "facilitydrain/evidence.hpp"
#include "facilitydrain/generations.hpp"
#include "facilitydrain/identity.hpp"
#include "facilitydrain/plan.hpp"
#include "facilitydrain/requests.hpp"
#include "facilitydrain/residual.hpp"
#include "facilitydrain/scope.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace facilitydrain {
namespace {

// ===========================================================================
// The canonical report grammar
// ===========================================================================
//
// One grammar, two spellings. Every fact has exactly one name in both, the
// order of the facts is fixed by the code below, and no fact is derived from a
// clock reading, a pointer, an address or an iteration order that could differ
// between two runs over the same durable state. There is no floating point
// value anywhere in either spelling.
//
// Text
// ----
//   * The first line is exactly "format facility-drain-coordinator/1".
//   * A section starts with one blank line and then a header, either
//     "<keyword> <identity>" ("plan 11") or the bare keyword ("coordinator").
//     The blocks inside a plan (domains, consumers, residuals, requests,
//     grant, fence, history, evaluation) are separated by one blank line each
//     as well, so one section never runs into the next.
//   * A scalar fact is one line, "key=value".
//   * A composite fact is one line, "key <canonical rendering>", used only
//     where the value is itself a canonical multi field rendering, such as
//     "targets asset:41|asset:42" or "generations scope=1 dep=2".
//   * A record is one line, "<keyword> key=value key=value ...".
//   * An absent optional sub record is stated, never silently dropped:
//     "enumeration domain=asi present=no", "grant present=no live=no".
//   * A bounded or suppressed list states what it left out:
//     "<list> total=5 shown=2 omitted=3 omitted-reason=bound".
//
// JSON
// ----
//   * Strict JSON: string bodies are escaped exactly as json_escape escapes
//     them, there are no trailing commas, no comments and no non finite or
//     floating point numbers. pretty = true indents with two spaces per level;
//     pretty = false emits the whole document on one line.
//   * The same facts, in the same order, spelled with '_' where the text
//     spelling uses '-'.
//   * Every list member X is followed by X_total, X_shown, X_omitted and
//     X_omitted_reason, so a consumer always knows how much it is not seeing.
//   * A digest of zeros means "no digest was recorded", and is rendered as
//     null: it is never a value that could match another digest.

constexpr std::string_view kFormatLine = "format facility-drain-coordinator/1";
constexpr std::string_view kFormatToken = "facility-drain-coordinator/1";
// The hex digits of an escaped control character: "\u00" plus two of these.
constexpr std::string_view kHexDigits = "0123456789ABCDEF";

// Why a list is not complete. "none" means nothing was left out.
constexpr std::string_view kReasonNone = "none";
constexpr std::string_view kReasonBound = "bound";
constexpr std::string_view kReasonOptions = "options";

// The number of facts the plan provenance block reports. Stated explicitly when
// the options suppress the block.
constexpr std::uint64_t kProvenanceFieldCount = 6;

// A zero byte bound would render nothing at all, which is what "the bound is
// exhausted" means; it is never read as "unbounded".
constexpr std::uint64_t kMinimumReportBytes = 1;

[[nodiscard]] std::string_view yes_no(bool value) noexcept { return value ? "yes" : "no"; }

// A count that cannot be represented as a 32 bit unsigned value is clamped; it
// can only happen for an input that already exceeds every configured bound.
[[nodiscard]] std::uint32_t count_u32(std::size_t value) noexcept {
  constexpr std::uint32_t kMaximum = (std::numeric_limits<std::uint32_t>::max)();
  return value >= static_cast<std::size_t>(kMaximum) ? kMaximum : static_cast<std::uint32_t>(value);
}

// ---------------------------------------------------------------------------
// Bounded output
// ---------------------------------------------------------------------------
//
// Every byte the report produces goes through this buffer, and the buffer stops
// accepting bytes the moment the next one would exceed the caller's bound. The
// buffer therefore never grows past the bound plus one small append, no matter
// how large or how malformed the snapshot it is rendering is. Once the bound is
// exceeded the accumulated text is released: the caller gets a limit error and
// a report that was cut short is never returned as if it were complete.

class TextBuffer {
 public:
  explicit TextBuffer(std::uint64_t limit) noexcept : limit_(limit) {}

  void append(std::string_view text) {
    if (exceeded_ || text.empty()) {
      return;
    }
    if (text.size() > limit_ - text_.size()) {
      fail();
      return;
    }
    text_.append(text);
  }

  void push_back(char value) {
    if (exceeded_) {
      return;
    }
    if (text_.size() >= limit_) {
      fail();
      return;
    }
    text_.push_back(value);
  }

  [[nodiscard]] bool exceeded() const noexcept { return exceeded_; }
  [[nodiscard]] std::string take() noexcept { return std::move(text_); }

 private:
  void fail() noexcept {
    exceeded_ = true;
    std::string released;
    text_.swap(released);
  }

  std::string text_{};
  std::uint64_t limit_ = 0;
  bool exceeded_ = false;
};

// ---------------------------------------------------------------------------
// JSON escaping
// ---------------------------------------------------------------------------
//
// The rule is byte exact: '"' and '\' are escaped, every C0 control character
// (U+0000..U+001F) and U+007F are escaped as "\u00XX" with uppercase hex
// digits, and every other byte is emitted exactly as it arrived. That last part
// is deliberate: the library rejects malformed UTF-8 at every input boundary,
// so a byte that is not valid UTF-8 can only be there because a caller put it
// there on purpose, and passing it through unchanged is the only rendering that
// does not invent content.

template <typename Sink>
void append_json_body(Sink& sink, std::string_view text) {
  for (const char raw : text) {
    const auto byte = static_cast<unsigned char>(raw);
    if (raw == '"') {
      sink.append("\\\"");
    } else if (raw == '\\') {
      sink.append("\\\\");
    } else if (byte < 0x20U || byte == 0x7FU) {
      sink.append("\\u00");
      sink.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
      sink.push_back(kHexDigits[byte & 0x0FU]);
    } else {
      sink.push_back(raw);
    }
  }
}

// The strict JSON writer. Member order is the call order, which is fixed by the
// traversals below and never depends on a container's iteration order.
class JsonWriter {
 public:
  JsonWriter(TextBuffer& out, bool pretty) noexcept : out_(out), pretty_(pretty) {}

  void begin_object() {
    begin_value();
    out_.push_back('{');
    levels_.push_back(1);
  }

  void end_object() {
    const bool empty = levels_.empty() || levels_.back() != 0;
    if (!levels_.empty()) {
      levels_.pop_back();
    }
    close(empty, '}');
  }

  void begin_array() {
    begin_value();
    out_.push_back('[');
    levels_.push_back(1);
  }

  void end_array() {
    const bool empty = levels_.empty() || levels_.back() != 0;
    if (!levels_.empty()) {
      levels_.pop_back();
    }
    close(empty, ']');
  }

  void key(std::string_view name) {
    separate();
    out_.push_back('"');
    append_json_body(out_, name);
    out_.push_back('"');
    out_.push_back(':');
    if (pretty_) {
      out_.push_back(' ');
    }
    pending_key_ = true;
  }

  void string_value(std::string_view value) {
    begin_value();
    out_.push_back('"');
    append_json_body(out_, value);
    out_.push_back('"');
  }

  void number_value(std::uint64_t value) {
    begin_value();
    out_.append(format_strong(value));
  }

  void signed_value(std::int64_t value) {
    begin_value();
    out_.append(std::to_string(value));
  }

  void boolean_value(bool value) {
    begin_value();
    out_.append(value ? "true" : "false");
  }

  void null_value() {
    begin_value();
    out_.append("null");
  }

  // A digest of zeros is the absence of a digest, never a value.
  void digest_value(const ContentDigest& digest) {
    if (digest.is_zero()) {
      null_value();
    } else {
      string_value(digest.to_hex());
    }
  }

 private:
  void close(bool empty, char bracket) {
    if (!empty) {
      newline_indent(levels_.size());
    }
    out_.push_back(bracket);
  }

  void begin_value() {
    if (pending_key_) {
      pending_key_ = false;
      return;
    }
    separate();
  }

  void separate() {
    if (levels_.empty()) {
      return;
    }
    if (levels_.back() != 0) {
      levels_.back() = 0;
    } else {
      out_.push_back(',');
    }
    newline_indent(levels_.size());
  }

  void newline_indent(std::size_t depth) {
    if (!pretty_) {
      return;
    }
    out_.push_back('\n');
    for (std::size_t index = 0; index < depth; ++index) {
      out_.append("  ");
    }
  }

  TextBuffer& out_;
  bool pretty_ = true;
  bool pending_key_ = false;
  std::vector<char> levels_{};
};

// ---------------------------------------------------------------------------
// Text primitives
// ---------------------------------------------------------------------------

// One blank line, the separator between two sections.
void text_break(TextBuffer& out) { out.push_back('\n'); }

void text_section(TextBuffer& out, std::string_view keyword, std::string_view identity) {
  out.push_back('\n');
  out.append(keyword);
  if (!identity.empty()) {
    out.push_back(' ');
    out.append(identity);
  }
  out.push_back('\n');
}

void text_fact(TextBuffer& out, std::string_view key, std::string_view value) {
  out.append(key);
  out.push_back('=');
  out.append(value);
  out.push_back('\n');
}

void text_number(TextBuffer& out, std::string_view key, std::uint64_t value) {
  text_fact(out, key, format_strong(value));
}

void text_signed(TextBuffer& out, std::string_view key, std::int64_t value) {
  text_fact(out, key, std::to_string(value));
}

void text_boolean(TextBuffer& out, std::string_view key, bool value) {
  text_fact(out, key, yes_no(value));
}

// A fact whose value is itself a canonical multi field rendering.
void text_composite(TextBuffer& out, std::string_view key, std::string_view value) {
  out.append(key);
  out.push_back(' ');
  out.append(value);
  out.push_back('\n');
}

void text_digest(TextBuffer& out, std::string_view key, const ContentDigest& digest) {
  const std::string rendered = digest.is_zero() ? std::string{"unset"} : digest.to_hex();
  text_fact(out, key, rendered);
}

// One record line: a keyword followed by the record's fields, all on one line.
class TextRecord {
 public:
  TextRecord(TextBuffer& out, std::string_view keyword) : out_(out) { out_.append(keyword); }

  TextRecord& token(std::string_view value) {
    out_.push_back(' ');
    out_.append(value);
    return *this;
  }

  TextRecord& field(std::string_view key, std::string_view value) {
    out_.push_back(' ');
    out_.append(key);
    out_.push_back('=');
    out_.append(value);
    return *this;
  }

  TextRecord& number(std::string_view key, std::uint64_t value) { return field(key, format_strong(value)); }

  TextRecord& signed_number(std::string_view key, std::int64_t value) {
    return field(key, std::to_string(value));
  }

  TextRecord& boolean(std::string_view key, bool value) { return field(key, yes_no(value)); }

  TextRecord& digest(std::string_view key, const ContentDigest& value) {
    const std::string rendered = value.is_zero() ? std::string{"unset"} : value.to_hex();
    return field(key, rendered);
  }

  // A composite field: "generations scope=1 dep=2".
  TextRecord& composite(std::string_view key, std::string_view value) {
    out_.push_back(' ');
    out_.append(key);
    out_.push_back(' ');
    out_.append(value);
    return *this;
  }

  void end() { out_.push_back('\n'); }

 private:
  TextBuffer& out_;
};

// ---------------------------------------------------------------------------
// Shared report decisions
// ---------------------------------------------------------------------------
//
// Both spellings are driven by these, so the two formats can never disagree
// about what is rendered and what was left out.

struct ListBounds {
  std::uint32_t total = 0;
  std::uint32_t shown = 0;
  std::uint32_t omitted = 0;
  std::string_view reason = kReasonNone;
};

// bound == 0 means "no additional bound"; included == false means the options
// asked for the whole list to be left out.
[[nodiscard]] ListBounds bound_list(std::size_t total, std::uint32_t bound, bool included) noexcept {
  ListBounds bounds;
  bounds.total = count_u32(total);
  if (!included) {
    bounds.omitted = bounds.total;
    bounds.reason = kReasonOptions;
    return bounds;
  }
  const std::uint32_t cap = bound == 0 ? bounds.total : (std::min)(bound, bounds.total);
  bounds.shown = cap;
  bounds.omitted = bounds.total - cap;
  bounds.reason = bounds.omitted == 0 ? kReasonNone : kReasonBound;
  return bounds;
}

// "Zero means the snapshot bound applies": the option can narrow the durable
// bound but can never widen it.
[[nodiscard]] ListBounds bound_plans(const CoordinatorSnapshot& snapshot, const ReportOptions& options) noexcept {
  const std::uint32_t durable = snapshot.limits.max_plans;
  const std::uint32_t bound =
      options.max_plans == 0 ? durable : (std::min)(options.max_plans, durable);
  return bound_list(snapshot.plans.size(), bound, true);
}

// The canonical plan id order. A snapshot is already stored in that order; the
// explicit sort makes the report independent of how a caller assembled one.
[[nodiscard]] std::vector<std::size_t> canonical_plan_order(const CoordinatorSnapshot& snapshot) {
  std::vector<std::size_t> order(snapshot.plans.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    order[index] = index;
  }
  std::stable_sort(order.begin(), order.end(), [&snapshot](std::size_t lhs, std::size_t rhs) {
    return snapshot.plans[lhs].spec.id < snapshot.plans[rhs].spec.id;
  });
  return order;
}

// The epoch a rendered evaluation is stamped with, and where that epoch came
// from. The verdict itself is independent of it: a grant carries its own epoch
// and its liveness is reported separately as the snapshot recorded it. The
// epoch selects only the value the evaluation reports.
struct EvaluationContext {
  ControlEpoch epoch{};
  std::string_view source = "unspecified";
};

[[nodiscard]] EvaluationContext coordinator_evaluation_context(const CoordinatorSnapshot& snapshot) noexcept {
  EvaluationContext context;
  context.epoch = snapshot.control_epoch;
  context.source = "coordinator";
  return context;
}

// A single plan snapshot does not carry the coordinator's current control
// epoch, so the evaluation is stamped with the epoch the plan itself records.
[[nodiscard]] EvaluationContext plan_evaluation_context(const DrainPlanSnapshot& plan) noexcept {
  EvaluationContext context;
  context.epoch = plan.spec.bindings.facility_epoch;
  context.source = "plan";
  return context;
}

[[nodiscard]] std::uint32_t domain_consumer_count(const DrainPlanSnapshot& plan, OwnerDomain domain,
                                                  bool mandatory_only) noexcept {
  std::uint32_t total = 0;
  for (const ConsumerRecord& record : plan.consumers) {
    if (record.domain() != domain) {
      continue;
    }
    if (mandatory_only && record.strength != ObligationStrength::kMandatory) {
      continue;
    }
    ++total;
  }
  return total;
}

[[nodiscard]] std::string bound_manifest_text(const std::optional<ContentDigest>& digest) {
  return digest.has_value() ? digest->to_hex() : std::string{"unbound"};
}

// ---------------------------------------------------------------------------
// Text: one plan
// ---------------------------------------------------------------------------

void render_plan_text(TextBuffer& out, const DrainPlanSnapshot& plan, const ReportOptions& options,
                      const EvaluationContext& context) {
  text_section(out, "plan", to_string(plan.spec.id));
  text_fact(out, "id", to_string(plan.spec.id));
  text_fact(out, "scope", format_scope(plan.spec.scope));
  text_number(out, "target-count", plan.spec.targets.size());
  text_composite(out, "targets", plan.spec.targets.to_canonical());
  text_fact(out, "targets-digest", plan.spec.targets.digest().to_hex());
  text_fact(out, "state", to_token(plan.state));
  text_number(out, "revision", plan.spec.revision.value());
  text_fact(out, "plan-digest", plan.plan_digest.to_hex());
  text_number(out, "last-observation", plan.last_observation.value());
  text_number(out, "last-commit", plan.last_commit.value());
  text_boolean(out, "cancelled", plan.cancelled);
  text_boolean(out, "failed", plan.failed);
  text_fact(out, "cancellation-detail", plan.cancellation_detail);
  text_fact(out, "failure-detail", plan.failure_detail);
  text_composite(out, "required-domains", plan.required_domains.to_canonical());
  text_number(out, "required-domain-count", plan.required_domains.count());
  text_composite(out, "generations", plan.spec.bindings.generations.to_canonical());
  {
    TextRecord record{out, "bound-manifests"};
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      const OwnerDomain domain = owner_domain_at(index);
      record.field(to_token(domain), bound_manifest_text(plan.spec.bindings.manifest_digest(domain)));
    }
    record.end();
  }
  if (options.include_provenance) {
    text_fact(out, "label", plan.spec.label);
    text_fact(out, "requested-by", plan.spec.requested_by);
    text_signed(out, "created-at-milliseconds", plan.spec.created_at_milliseconds);
    text_number(out, "facility-epoch", plan.spec.bindings.facility_epoch.value());
    text_number(out, "policy-id", plan.spec.bindings.policy_id.value());
    text_digest(out, "policy-digest", plan.spec.bindings.policy_digest);
  } else {
    TextRecord record{out, "provenance"};
    record.number("omitted", kProvenanceFieldCount);
    record.end();
  }

  // -- domains -------------------------------------------------------------
  text_break(out);
  {
    std::uint32_t required = 0;
    std::uint32_t consumers = 0;
    std::uint32_t mandatory = 0;
    for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
      const OwnerDomain domain = owner_domain_at(index);
      if (plan.required_domains.contains(domain)) {
        ++required;
      }
      consumers += domain_consumer_count(plan, domain, false);
      mandatory += domain_consumer_count(plan, domain, true);
    }
    TextRecord record{out, "domains"};
    record.number("count", kOwnerDomainCount);
    record.number("required", required);
    record.number("consumers", consumers);
    record.number("mandatory-consumers", mandatory);
    record.end();
  }
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    const DomainEnumerationSnapshot& enumeration = plan.enumerations[index];
    const DomainCompletionSnapshot& completion = plan.completions[index];

    TextRecord domain_record{out, "domain"};
    domain_record.token(to_token(domain));
    domain_record.boolean("required", plan.required_domains.contains(domain));
    domain_record.number("consumers", domain_consumer_count(plan, domain, false));
    domain_record.number("mandatory-consumers", domain_consumer_count(plan, domain, true));
    domain_record.number("open-residuals", plan.residuals.open_count(domain));
    domain_record.number("unknown-residuals", plan.residuals.unknown_count(domain));
    domain_record.end();

    if (!enumeration.present) {
      TextRecord record{out, "enumeration"};
      record.field("domain", to_token(domain));
      record.boolean("present", false);
      record.end();
    } else {
      TextRecord record{out, "enumeration"};
      record.field("domain", to_token(domain));
      record.boolean("present", true);
      record.boolean("accepted", enumeration.accepted);
      record.field("rejection", to_token(enumeration.rejection));
      record.number("generation", enumeration.evidence.generation.value());
      record.field("coverage", to_token(enumeration.evidence.coverage));
      record.number("observed-at", enumeration.evidence.observed_at.value());
      record.digest("manifest-digest", enumeration.evidence.manifest_digest);
      record.digest("scope-manifest-digest", enumeration.evidence.scope_manifest_digest);
      record.number("consumer-count", enumeration.consumers.size());
      record.signed_number("observed-at-milliseconds", enumeration.evidence.observed_at_milliseconds);
      record.field("source", enumeration.evidence.source);
      record.field("annotation", enumeration.evidence.annotation);
      record.composite("generations", enumeration.evidence.generations.to_canonical());
      record.end();
    }

    if (!completion.present) {
      TextRecord record{out, "completion"};
      record.field("domain", to_token(domain));
      record.boolean("present", false);
      record.end();
    } else {
      TextRecord record{out, "completion"};
      record.field("domain", to_token(domain));
      record.boolean("present", true);
      record.boolean("compatible", completion.compatible);
      record.field("rejection", to_token(completion.rejection));
      record.number("id", completion.evidence.id.value());
      record.field("state", to_token(completion.evidence.state));
      record.number("generation", completion.evidence.generation.value());
      record.number("observed-at", completion.evidence.observed_at.value());
      record.digest("payload-digest", completion.evidence.payload_digest);
      record.digest("manifest-digest", completion.evidence.manifest_digest);
      record.digest("scope-manifest-digest", completion.evidence.scope_manifest_digest);
      record.boolean("residual-count-known", completion.evidence.residual_count_known);
      if (completion.evidence.residual_count_known) {
        record.number("residual-count", completion.evidence.residual_count);
      } else {
        record.field("residual-count", "unknown");
      }
      record.signed_number("observed-at-milliseconds", completion.evidence.observed_at_milliseconds);
      record.field("source", completion.evidence.source);
      record.field("annotation", completion.evidence.annotation);
      record.composite("generations", completion.evidence.generations.to_canonical());
      record.end();
    }

    TextRecord summary{out, "residual-summary"};
    summary.field("domain", to_token(domain));
    summary.number("open", plan.residuals.open_count(domain));
    summary.number("unknown", plan.residuals.unknown_count(domain));
    summary.number("known-obligations", plan.residuals.known_obligation_count(domain));
    summary.number("relinquished-obligations", plan.residuals.relinquished_obligation_count(domain));
    summary.end();
  }

  // -- consumer manifest of record -----------------------------------------
  const ListBounds consumers = bound_list(plan.consumers.size(), 0, options.include_consumers);
  text_break(out);
  {
    TextRecord record{out, "consumers"};
    record.number("total", consumers.total);
    record.number("shown", consumers.shown);
    record.number("omitted", consumers.omitted);
    record.field("omitted-reason", consumers.reason);
    record.end();
  }
  for (std::uint32_t index = 0; index < consumers.shown; ++index) {
    const ConsumerRecord& consumer = plan.consumers[index];
    TextRecord record{out, "consumer"};
    record.field("domain", to_token(consumer.domain()));
    record.number("obligation", consumer.obligation.value());
    record.field("category", to_token(consumer.category));
    record.number("generation", consumer.generation.value());
    record.number("reservation", consumer.reservation.value());
    record.field("strength", to_token(consumer.strength));
    record.field("label", consumer.label);
    record.field("source", consumer.source);
    record.end();
  }

  // -- residual ledger ------------------------------------------------------
  const ListBounds residuals =
      bound_list(plan.residuals.entries.size(), options.max_residuals, options.include_residuals);
  text_break(out);
  {
    TextRecord record{out, "residuals"};
    record.number("total", residuals.total);
    record.number("shown", residuals.shown);
    record.number("omitted", residuals.omitted);
    record.field("omitted-reason", residuals.reason);
    record.number("open", plan.residuals.open_count());
    record.number("unknown", plan.residuals.unknown_count());
    record.end();
  }
  for (std::uint32_t index = 0; index < residuals.shown; ++index) {
    const ResidualEntry& entry = plan.residuals.entries[index];
    TextRecord record{out, "residual"};
    record.field("domain", to_token(entry.domain));
    if (entry.obligation.is_default()) {
      record.field("obligation", "scope");
    } else {
      record.number("obligation", entry.obligation.value());
    }
    record.field("kind", to_token(entry.kind));
    record.field("state", to_token(entry.state));
    record.number("generation", entry.generation.value());
    record.number("resolution-generation", entry.resolution_evidence_generation.value());
    record.number("recorded-at", entry.recorded_at.value());
    record.number("resolved-at", entry.resolved_at.value());
    record.digest("detail-digest", entry.detail_digest);
    record.field("detail", entry.detail);
    record.end();
  }

  // -- requests -------------------------------------------------------------
  const ListBounds requests = bound_list(plan.requests.size(), options.max_requests, options.include_requests);
  text_break(out);
  {
    TextRecord record{out, "requests"};
    record.number("total", requests.total);
    record.number("shown", requests.shown);
    record.number("omitted", requests.omitted);
    record.field("omitted-reason", requests.reason);
    record.end();
  }
  for (std::uint32_t index = 0; index < requests.shown; ++index) {
    const DrainRequest& request = plan.requests[index];
    TextRecord record{out, "request"};
    record.number("id", request.id.value());
    record.field("domain", to_token(request.key.domain));
    record.field("scope", format_scope(request.key.scope));
    record.field("state", to_token(request.state));
    record.number("attempt", request.key.attempt.value());
    record.number("bound-operations", request.bound_operations);
    record.digest("idempotency-key", request.idempotency_key);
    record.digest("obligation-digest", request.key.obligation_digest);
    record.number("policy-generation", request.key.policy_generation.value());
    record.number("staged-at", request.staged_at.value());
    record.number("issued-at", request.issued_at.value());
    record.number("acknowledged-at", request.acknowledged_at.value());
    record.number("settled-at", request.settled_at.value());
    record.signed_number("staged-at-milliseconds", request.staged_at_milliseconds);
    record.signed_number("issued-at-milliseconds", request.issued_at_milliseconds);
    record.field("target-system", request.target_system);
    record.field("instruction", request.instruction);
    record.field("acknowledgement-source", request.acknowledgement_source);
    record.field("settlement-detail", request.settlement_detail);
    record.end();
  }

  // -- grant ----------------------------------------------------------------
  text_break(out);
  if (!plan.grant.has_value()) {
    TextRecord record{out, "grant"};
    record.boolean("present", false);
    record.boolean("live", false);
    record.end();
  } else {
    const SafeToRemoveGrant& grant = plan.grant.value();
    TextRecord record{out, "grant"};
    record.boolean("present", true);
    record.boolean("live", plan.grant_live);
    record.number("plan", grant.plan.value());
    record.number("revision", grant.revision.value());
    record.number("epoch", grant.epoch.value());
    record.number("observation-floor", grant.observation_floor.value());
    record.number("granted-commit", grant.granted_commit.value());
    record.digest("evidence-digest", grant.evidence_digest);
    record.digest("manifest-digest", grant.manifest_digest);
    record.signed_number("granted-at-milliseconds", grant.granted_at_milliseconds);
    record.field("granted-by", grant.granted_by);
    record.composite("generations", grant.generations.to_canonical());
    record.end();
  }

  // -- fence ----------------------------------------------------------------
  text_break(out);
  if (!plan.fence.has_value()) {
    TextRecord record{out, "fence"};
    record.boolean("present", false);
    record.end();
  } else {
    const FenceRecord& fence = plan.fence.value();
    TextRecord record{out, "fence"};
    record.boolean("present", true);
    record.field("reason", to_token(fence.reason));
    record.number("floor", fence.floor.value());
    record.number("revision", fence.revision.value());
    record.number("epoch", fence.epoch.value());
    record.number("commit", fence.commit.value());
    record.signed_number("recorded-at-milliseconds", fence.recorded_at_milliseconds);
    record.field("detail", fence.detail);
    record.end();
  }

  // -- history --------------------------------------------------------------
  const ListBounds history = bound_list(plan.history.size(), 0, options.include_history);
  text_break(out);
  {
    TextRecord record{out, "history"};
    record.number("total", history.total);
    record.number("shown", history.shown);
    record.number("omitted", history.omitted);
    record.field("omitted-reason", history.reason);
    record.end();
  }
  for (std::uint32_t index = 0; index < history.shown; ++index) {
    const PlanHistoryEntry& entry = plan.history[index];
    TextRecord record{out, "history"};
    record.field("from", to_token(entry.from_state));
    record.field("to", to_token(entry.to_state));
    record.field("cause", to_token(entry.cause));
    record.number("observed-at", entry.observed_at.value());
    record.number("commit", entry.commit.value());
    record.signed_number("recorded-at-milliseconds", entry.recorded_at_milliseconds);
    record.field("detail", entry.detail);
    record.end();
  }

  // -- evaluation -----------------------------------------------------------
  text_break(out);
  if (!options.include_evaluations) {
    TextRecord record{out, "evaluation"};
    record.number("omitted", 1);
    record.end();
    return;
  }
  const SafeToRemoveEvaluation evaluation = evaluate_safe_to_remove(plan, context.epoch);
  {
    TextRecord record{out, "evaluation"};
    record.number("plan", evaluation.plan.value());
    record.number("revision", evaluation.revision.value());
    record.number("epoch", evaluation.epoch.value());
    record.field("epoch-source", context.source);
    record.field("verdict", to_token(evaluation.verdict));
    record.field("primary-blocking-code", to_token(evaluation.primary_blocking_code));
    record.number("required-domains", evaluation.required_domain_count);
    record.boolean("all-required-proven", evaluation.all_required_proven);
    record.boolean("enumeration-complete", evaluation.enumeration_complete_for_required);
    record.boolean("fenced", evaluation.fenced);
    record.number("fence-floor", evaluation.fence_floor.value());
    record.number("open-residuals", evaluation.open_residuals);
    record.number("unknown-residuals", evaluation.unknown_residuals);
    record.digest("evidence-digest", evaluation.evidence_digest);
    record.digest("manifest-digest", evaluation.manifest_digest);
    record.number("evaluated-commit", evaluation.evaluated_commit.value());
    record.composite("generations", evaluation.generations.to_canonical());
    record.end();
  }
  if (!evaluation.blocking_codes.empty()) {
    TextRecord record{out, "blocking"};
    for (const ErrorCode code : evaluation.blocking_codes) {
      record.token(to_token(code));
    }
    record.end();
  }
  for (const DomainAssessment& assessment : evaluation.domains) {
    TextRecord record{out, "assessment"};
    record.field("domain", to_token(assessment.domain));
    record.field("verdict", to_token(assessment.verdict));
    record.field("blocking-code", to_token(assessment.blocking_code));
    record.boolean("required", assessment.required);
    record.boolean("enumeration-present", assessment.enumeration_present);
    record.boolean("enumeration-complete", assessment.enumeration_complete);
    record.boolean("enumeration-manifest-matches", assessment.enumeration_manifest_matches);
    record.boolean("enumeration-after-floor", assessment.enumeration_after_floor);
    record.boolean("completion-present", assessment.completion_present);
    record.boolean("completion-compatible", assessment.completion_compatible);
    record.boolean("completion-manifest-matches", assessment.completion_manifest_matches);
    record.boolean("completion-after-floor", assessment.completion_after_floor);
    record.boolean("residual-count-known", assessment.residual_count_known);
    record.number("residual-count", assessment.residual_count);
    record.number("open-residuals", assessment.open_residuals);
    record.number("unknown-residuals", assessment.unknown_residuals);
    record.number("known-obligations", assessment.known_obligations);
    record.number("relinquished-obligations", assessment.relinquished_obligations);
    record.number("mandatory-obligations", assessment.mandatory_obligations);
    record.number("enumeration-generation", assessment.enumeration_generation.value());
    record.number("completion-generation", assessment.completion_generation.value());
    record.composite("evidence-generations", assessment.evidence_generations.to_canonical());
    record.field("reason", assessment.reason);
    record.end();
  }
}

// ---------------------------------------------------------------------------
// JSON: one plan
// ---------------------------------------------------------------------------

void write_generations_json(JsonWriter& json, std::string_view name, const GenerationSet& generations) {
  json.key(name);
  json.begin_object();
  json.key("scope");
  json.number_value(generations.scope.value());
  json.key("dependency");
  json.number_value(generations.dependency.value());
  json.key("reservation");
  json.number_value(generations.reservation.value());
  json.key("obligation");
  json.number_value(generations.obligation.value());
  json.key("policy");
  json.number_value(generations.policy.value());
  json.key("topology");
  json.number_value(generations.topology.value());
  json.key("maintenance");
  json.number_value(generations.maintenance.value());
  json.key("capacity");
  json.number_value(generations.capacity.value());
  json.key("hardware");
  json.number_value(generations.hardware.value());
  json.key("firmware");
  json.number_value(generations.firmware.value());
  json.end_object();
}

void write_list_metadata_json(JsonWriter& json, std::string_view name, const ListBounds& bounds) {
  json.key(std::string{name} + "_total");
  json.number_value(bounds.total);
  json.key(std::string{name} + "_shown");
  json.number_value(bounds.shown);
  json.key(std::string{name} + "_omitted");
  json.number_value(bounds.omitted);
  json.key(std::string{name} + "_omitted_reason");
  json.string_value(bounds.reason);
}

void write_plan_json(JsonWriter& json, const DrainPlanSnapshot& plan, const ReportOptions& options,
                     const EvaluationContext& context) {
  json.begin_object();
  json.key("id");
  json.number_value(plan.spec.id.value());
  json.key("scope");
  json.string_value(format_scope(plan.spec.scope));
  json.key("target_count");
  json.number_value(plan.spec.targets.size());
  json.key("targets");
  json.begin_array();
  for (const DrainScope& target : plan.spec.targets.targets()) {
    json.string_value(format_scope(target));
  }
  json.end_array();
  json.key("targets_digest");
  json.string_value(plan.spec.targets.digest().to_hex());
  json.key("state");
  json.string_value(to_token(plan.state));
  json.key("revision");
  json.number_value(plan.spec.revision.value());
  json.key("plan_digest");
  json.string_value(plan.plan_digest.to_hex());
  json.key("last_observation");
  json.number_value(plan.last_observation.value());
  json.key("last_commit");
  json.number_value(plan.last_commit.value());
  json.key("cancelled");
  json.boolean_value(plan.cancelled);
  json.key("cancellation_detail");
  json.string_value(plan.cancellation_detail);
  json.key("failed");
  json.boolean_value(plan.failed);
  json.key("failure_detail");
  json.string_value(plan.failure_detail);
  json.key("required_domains");
  json.begin_array();
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    if (plan.required_domains.contains(domain)) {
      json.string_value(to_token(domain));
    }
  }
  json.end_array();
  json.key("required_domain_count");
  json.number_value(plan.required_domains.count());
  write_generations_json(json, "generations", plan.spec.bindings.generations);
  json.key("bound_manifests");
  json.begin_array();
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    json.begin_object();
    json.key("domain");
    json.string_value(to_token(domain));
    json.key("digest");
    const std::optional<ContentDigest>& digest = plan.spec.bindings.manifest_digest(domain);
    if (digest.has_value()) {
      json.digest_value(digest.value());
    } else {
      json.null_value();
    }
    json.end_object();
  }
  json.end_array();
  if (options.include_provenance) {
    json.key("provenance");
    json.begin_object();
    json.key("label");
    json.string_value(plan.spec.label);
    json.key("requested_by");
    json.string_value(plan.spec.requested_by);
    json.key("created_at_milliseconds");
    json.signed_value(plan.spec.created_at_milliseconds);
    json.key("facility_epoch");
    json.number_value(plan.spec.bindings.facility_epoch.value());
    json.key("policy_id");
    json.number_value(plan.spec.bindings.policy_id.value());
    json.key("policy_digest");
    json.digest_value(plan.spec.bindings.policy_digest);
    json.end_object();
  } else {
    json.key("provenance");
    json.null_value();
    json.key("provenance_omitted");
    json.number_value(kProvenanceFieldCount);
  }

  json.key("domains");
  json.begin_array();
  for (std::uint32_t index = 0; index < kOwnerDomainCount; ++index) {
    const OwnerDomain domain = owner_domain_at(index);
    const DomainEnumerationSnapshot& enumeration = plan.enumerations[index];
    const DomainCompletionSnapshot& completion = plan.completions[index];

    json.begin_object();
    json.key("domain");
    json.string_value(to_token(domain));
    json.key("required");
    json.boolean_value(plan.required_domains.contains(domain));
    json.key("consumer_count");
    json.number_value(domain_consumer_count(plan, domain, false));
    json.key("mandatory_consumer_count");
    json.number_value(domain_consumer_count(plan, domain, true));
    json.key("open_residuals");
    json.number_value(plan.residuals.open_count(domain));
    json.key("unknown_residuals");
    json.number_value(plan.residuals.unknown_count(domain));
    json.key("known_obligations");
    json.number_value(plan.residuals.known_obligation_count(domain));
    json.key("relinquished_obligations");
    json.number_value(plan.residuals.relinquished_obligation_count(domain));
    json.key("enumeration");
    if (!enumeration.present) {
      json.null_value();
    } else {
      json.begin_object();
      json.key("generation");
      json.number_value(enumeration.evidence.generation.value());
      json.key("coverage");
      json.string_value(to_token(enumeration.evidence.coverage));
      json.key("observed_at");
      json.number_value(enumeration.evidence.observed_at.value());
      json.key("manifest_digest");
      json.digest_value(enumeration.evidence.manifest_digest);
      json.key("scope_manifest_digest");
      json.digest_value(enumeration.evidence.scope_manifest_digest);
      json.key("accepted");
      json.boolean_value(enumeration.accepted);
      json.key("rejection");
      json.string_value(to_token(enumeration.rejection));
      json.key("consumer_count");
      json.number_value(enumeration.consumers.size());
      json.key("observed_at_milliseconds");
      json.signed_value(enumeration.evidence.observed_at_milliseconds);
      json.key("source");
      json.string_value(enumeration.evidence.source);
      json.key("annotation");
      json.string_value(enumeration.evidence.annotation);
      write_generations_json(json, "generations", enumeration.evidence.generations);
      json.end_object();
    }
    json.key("completion");
    if (!completion.present) {
      json.null_value();
    } else {
      json.begin_object();
      json.key("id");
      json.number_value(completion.evidence.id.value());
      json.key("state");
      json.string_value(to_token(completion.evidence.state));
      json.key("generation");
      json.number_value(completion.evidence.generation.value());
      json.key("observed_at");
      json.number_value(completion.evidence.observed_at.value());
      json.key("payload_digest");
      json.digest_value(completion.evidence.payload_digest);
      json.key("manifest_digest");
      json.digest_value(completion.evidence.manifest_digest);
      json.key("scope_manifest_digest");
      json.digest_value(completion.evidence.scope_manifest_digest);
      json.key("compatible");
      json.boolean_value(completion.compatible);
      json.key("rejection");
      json.string_value(to_token(completion.rejection));
      json.key("residual_count_known");
      json.boolean_value(completion.evidence.residual_count_known);
      json.key("residual_count");
      if (completion.evidence.residual_count_known) {
        json.number_value(completion.evidence.residual_count);
      } else {
        json.null_value();
      }
      json.key("observed_at_milliseconds");
      json.signed_value(completion.evidence.observed_at_milliseconds);
      json.key("source");
      json.string_value(completion.evidence.source);
      json.key("annotation");
      json.string_value(completion.evidence.annotation);
      write_generations_json(json, "generations", completion.evidence.generations);
      json.end_object();
    }
    json.end_object();
  }
  json.end_array();

  // -- consumer manifest of record -----------------------------------------
  const ListBounds consumers = bound_list(plan.consumers.size(), 0, options.include_consumers);
  json.key("consumers");
  json.begin_array();
  for (std::uint32_t index = 0; index < consumers.shown; ++index) {
    const ConsumerRecord& consumer = plan.consumers[index];
    json.begin_object();
    json.key("domain");
    json.string_value(to_token(consumer.domain()));
    json.key("obligation");
    json.number_value(consumer.obligation.value());
    json.key("category");
    json.string_value(to_token(consumer.category));
    json.key("generation");
    json.number_value(consumer.generation.value());
    json.key("reservation");
    json.number_value(consumer.reservation.value());
    json.key("strength");
    json.string_value(to_token(consumer.strength));
    json.key("label");
    json.string_value(consumer.label);
    json.key("source");
    json.string_value(consumer.source);
    json.end_object();
  }
  json.end_array();
  write_list_metadata_json(json, "consumers", consumers);

  // -- residual ledger ------------------------------------------------------
  const ListBounds residuals =
      bound_list(plan.residuals.entries.size(), options.max_residuals, options.include_residuals);
  json.key("residuals");
  json.begin_array();
  for (std::uint32_t index = 0; index < residuals.shown; ++index) {
    const ResidualEntry& entry = plan.residuals.entries[index];
    json.begin_object();
    json.key("domain");
    json.string_value(to_token(entry.domain));
    json.key("obligation");
    json.number_value(entry.obligation.value());
    json.key("scope_wide");
    json.boolean_value(entry.obligation.is_default());
    json.key("kind");
    json.string_value(to_token(entry.kind));
    json.key("state");
    json.string_value(to_token(entry.state));
    json.key("generation");
    json.number_value(entry.generation.value());
    json.key("resolution_generation");
    json.number_value(entry.resolution_evidence_generation.value());
    json.key("recorded_at");
    json.number_value(entry.recorded_at.value());
    json.key("resolved_at");
    json.number_value(entry.resolved_at.value());
    json.key("detail_digest");
    json.digest_value(entry.detail_digest);
    json.key("detail");
    json.string_value(entry.detail);
    json.end_object();
  }
  json.end_array();
  json.key("residuals_open");
  json.number_value(plan.residuals.open_count());
  json.key("residuals_unknown");
  json.number_value(plan.residuals.unknown_count());
  write_list_metadata_json(json, "residuals", residuals);

  // -- requests -------------------------------------------------------------
  const ListBounds requests = bound_list(plan.requests.size(), options.max_requests, options.include_requests);
  json.key("requests");
  json.begin_array();
  for (std::uint32_t index = 0; index < requests.shown; ++index) {
    const DrainRequest& request = plan.requests[index];
    json.begin_object();
    json.key("id");
    json.number_value(request.id.value());
    json.key("domain");
    json.string_value(to_token(request.key.domain));
    json.key("scope");
    json.string_value(format_scope(request.key.scope));
    json.key("state");
    json.string_value(to_token(request.state));
    json.key("attempt");
    json.number_value(request.key.attempt.value());
    json.key("bound_operations");
    json.number_value(request.bound_operations);
    json.key("idempotency_key");
    json.digest_value(request.idempotency_key);
    json.key("obligation_digest");
    json.digest_value(request.key.obligation_digest);
    json.key("policy_generation");
    json.number_value(request.key.policy_generation.value());
    json.key("staged_at");
    json.number_value(request.staged_at.value());
    json.key("issued_at");
    json.number_value(request.issued_at.value());
    json.key("acknowledged_at");
    json.number_value(request.acknowledged_at.value());
    json.key("settled_at");
    json.number_value(request.settled_at.value());
    json.key("staged_at_milliseconds");
    json.signed_value(request.staged_at_milliseconds);
    json.key("issued_at_milliseconds");
    json.signed_value(request.issued_at_milliseconds);
    json.key("target_system");
    json.string_value(request.target_system);
    json.key("instruction");
    json.string_value(request.instruction);
    json.key("acknowledgement_source");
    json.string_value(request.acknowledgement_source);
    json.key("settlement_detail");
    json.string_value(request.settlement_detail);
    json.end_object();
  }
  json.end_array();
  write_list_metadata_json(json, "requests", requests);

  // -- grant ----------------------------------------------------------------
  json.key("grant");
  if (!plan.grant.has_value()) {
    json.null_value();
  } else {
    const SafeToRemoveGrant& grant = plan.grant.value();
    json.begin_object();
    json.key("plan");
    json.number_value(grant.plan.value());
    json.key("revision");
    json.number_value(grant.revision.value());
    json.key("epoch");
    json.number_value(grant.epoch.value());
    json.key("observation_floor");
    json.number_value(grant.observation_floor.value());
    json.key("granted_commit");
    json.number_value(grant.granted_commit.value());
    json.key("evidence_digest");
    json.digest_value(grant.evidence_digest);
    json.key("manifest_digest");
    json.digest_value(grant.manifest_digest);
    json.key("granted_at_milliseconds");
    json.signed_value(grant.granted_at_milliseconds);
    json.key("granted_by");
    json.string_value(grant.granted_by);
    write_generations_json(json, "generations", grant.generations);
    json.end_object();
  }
  json.key("grant_live");
  json.boolean_value(plan.grant_live);

  // -- fence ----------------------------------------------------------------
  json.key("fence");
  if (!plan.fence.has_value()) {
    json.null_value();
  } else {
    const FenceRecord& fence = plan.fence.value();
    json.begin_object();
    json.key("reason");
    json.string_value(to_token(fence.reason));
    json.key("floor");
    json.number_value(fence.floor.value());
    json.key("revision");
    json.number_value(fence.revision.value());
    json.key("epoch");
    json.number_value(fence.epoch.value());
    json.key("commit");
    json.number_value(fence.commit.value());
    json.key("recorded_at_milliseconds");
    json.signed_value(fence.recorded_at_milliseconds);
    json.key("detail");
    json.string_value(fence.detail);
    json.end_object();
  }

  // -- history --------------------------------------------------------------
  const ListBounds history = bound_list(plan.history.size(), 0, options.include_history);
  json.key("history");
  json.begin_array();
  for (std::uint32_t index = 0; index < history.shown; ++index) {
    const PlanHistoryEntry& entry = plan.history[index];
    json.begin_object();
    json.key("from_state");
    json.string_value(to_token(entry.from_state));
    json.key("to_state");
    json.string_value(to_token(entry.to_state));
    json.key("cause");
    json.string_value(to_token(entry.cause));
    json.key("observed_at");
    json.number_value(entry.observed_at.value());
    json.key("commit");
    json.number_value(entry.commit.value());
    json.key("recorded_at_milliseconds");
    json.signed_value(entry.recorded_at_milliseconds);
    json.key("detail");
    json.string_value(entry.detail);
    json.end_object();
  }
  json.end_array();
  write_list_metadata_json(json, "history", history);

  // -- evaluation -----------------------------------------------------------
  json.key("evaluation");
  if (!options.include_evaluations) {
    json.null_value();
    json.key("evaluation_omitted");
    json.number_value(1);
    json.end_object();
    return;
  }
  const SafeToRemoveEvaluation evaluation = evaluate_safe_to_remove(plan, context.epoch);
  json.begin_object();
  json.key("plan");
  json.number_value(evaluation.plan.value());
  json.key("revision");
  json.number_value(evaluation.revision.value());
  json.key("epoch");
  json.number_value(evaluation.epoch.value());
  json.key("epoch_source");
  json.string_value(context.source);
  json.key("verdict");
  json.string_value(to_token(evaluation.verdict));
  json.key("primary_blocking_code");
  json.string_value(to_token(evaluation.primary_blocking_code));
  json.key("blocking_codes");
  json.begin_array();
  for (const ErrorCode code : evaluation.blocking_codes) {
    json.string_value(to_token(code));
  }
  json.end_array();
  json.key("required_domain_count");
  json.number_value(evaluation.required_domain_count);
  json.key("all_required_proven");
  json.boolean_value(evaluation.all_required_proven);
  json.key("enumeration_complete_for_required");
  json.boolean_value(evaluation.enumeration_complete_for_required);
  json.key("fenced");
  json.boolean_value(evaluation.fenced);
  json.key("fence_floor");
  json.number_value(evaluation.fence_floor.value());
  json.key("open_residuals");
  json.number_value(evaluation.open_residuals);
  json.key("unknown_residuals");
  json.number_value(evaluation.unknown_residuals);
  json.key("evidence_digest");
  json.digest_value(evaluation.evidence_digest);
  json.key("manifest_digest");
  json.digest_value(evaluation.manifest_digest);
  json.key("evaluated_commit");
  json.number_value(evaluation.evaluated_commit.value());
  write_generations_json(json, "generations", evaluation.generations);
  json.key("domains");
  json.begin_array();
  for (const DomainAssessment& assessment : evaluation.domains) {
    json.begin_object();
    json.key("domain");
    json.string_value(to_token(assessment.domain));
    json.key("verdict");
    json.string_value(to_token(assessment.verdict));
    json.key("blocking_code");
    json.string_value(to_token(assessment.blocking_code));
    json.key("required");
    json.boolean_value(assessment.required);
    json.key("enumeration_present");
    json.boolean_value(assessment.enumeration_present);
    json.key("enumeration_complete");
    json.boolean_value(assessment.enumeration_complete);
    json.key("enumeration_manifest_matches");
    json.boolean_value(assessment.enumeration_manifest_matches);
    json.key("enumeration_after_floor");
    json.boolean_value(assessment.enumeration_after_floor);
    json.key("completion_present");
    json.boolean_value(assessment.completion_present);
    json.key("completion_compatible");
    json.boolean_value(assessment.completion_compatible);
    json.key("completion_manifest_matches");
    json.boolean_value(assessment.completion_manifest_matches);
    json.key("completion_after_floor");
    json.boolean_value(assessment.completion_after_floor);
    json.key("residual_count_known");
    json.boolean_value(assessment.residual_count_known);
    json.key("residual_count");
    json.number_value(assessment.residual_count);
    json.key("open_residuals");
    json.number_value(assessment.open_residuals);
    json.key("unknown_residuals");
    json.number_value(assessment.unknown_residuals);
    json.key("known_obligations");
    json.number_value(assessment.known_obligations);
    json.key("relinquished_obligations");
    json.number_value(assessment.relinquished_obligations);
    json.key("mandatory_obligations");
    json.number_value(assessment.mandatory_obligations);
    json.key("enumeration_generation");
    json.number_value(assessment.enumeration_generation.value());
    json.key("completion_generation");
    json.number_value(assessment.completion_generation.value());
    write_generations_json(json, "evidence_generations", assessment.evidence_generations);
    json.key("reason");
    json.string_value(assessment.reason);
    json.end_object();
  }
  json.end_array();
  json.end_object();
  json.end_object();
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

void render_coordinator_text(TextBuffer& out, const CoordinatorSnapshot& snapshot, const ReportOptions& options) {
  out.append(kFormatLine);
  out.push_back('\n');
  text_section(out, "coordinator", "");
  text_number(out, "format-version", snapshot.report_format_version);
  text_boolean(out, "durable", snapshot.durable);
  text_fact(out, "store-root", snapshot.store_root);
  text_number(out, "incarnation", snapshot.incarnation.value());
  text_number(out, "control-epoch", snapshot.control_epoch.value());
  text_number(out, "commit-sequence", snapshot.commit_sequence.value());
  text_number(out, "observation-sequence", snapshot.observation_sequence.value());
  const ListBounds plans = bound_plans(snapshot, options);
  text_number(out, "plan-count", plans.total);
  text_number(out, "plan-shown", plans.shown);
  text_number(out, "plan-omitted", plans.omitted);
  text_fact(out, "plan-omitted-reason", plans.reason);
  text_digest(out, "state-digest", snapshot.state_digest);

  const std::vector<std::size_t> order = canonical_plan_order(snapshot);
  const EvaluationContext context = coordinator_evaluation_context(snapshot);
  for (std::uint32_t index = 0; index < plans.shown; ++index) {
    render_plan_text(out, snapshot.plans[order[index]], options, context);
  }
}

void render_coordinator_json(TextBuffer& out, const CoordinatorSnapshot& snapshot, const ReportOptions& options) {
  const ListBounds plans = bound_plans(snapshot, options);
  const std::vector<std::size_t> order = canonical_plan_order(snapshot);
  const EvaluationContext context = coordinator_evaluation_context(snapshot);

  JsonWriter json{out, options.pretty};
  json.begin_object();
  json.key("format");
  json.string_value(kFormatToken);
  json.key("coordinator");
  json.begin_object();
  json.key("format_version");
  json.number_value(snapshot.report_format_version);
  json.key("durable");
  json.boolean_value(snapshot.durable);
  json.key("store_root");
  json.string_value(snapshot.store_root);
  json.key("incarnation");
  json.number_value(snapshot.incarnation.value());
  json.key("control_epoch");
  json.number_value(snapshot.control_epoch.value());
  json.key("commit_sequence");
  json.number_value(snapshot.commit_sequence.value());
  json.key("observation_sequence");
  json.number_value(snapshot.observation_sequence.value());
  json.key("plan_count");
  json.number_value(plans.total);
  json.key("plans_shown");
  json.number_value(plans.shown);
  json.key("plans_omitted");
  json.number_value(plans.omitted);
  json.key("plans_omitted_reason");
  json.string_value(plans.reason);
  json.key("state_digest");
  json.digest_value(snapshot.state_digest);
  json.end_object();
  json.key("plans");
  json.begin_array();
  for (std::uint32_t index = 0; index < plans.shown; ++index) {
    write_plan_json(json, snapshot.plans[order[index]], options, context);
  }
  json.end_array();
  json.end_object();
}

void render_plan_text_document(TextBuffer& out, const DrainPlanSnapshot& plan, const ReportOptions& options) {
  out.append(kFormatLine);
  out.push_back('\n');
  render_plan_text(out, plan, options, plan_evaluation_context(plan));
}

void render_plan_json_document(TextBuffer& out, const DrainPlanSnapshot& plan, const ReportOptions& options) {
  JsonWriter json{out, options.pretty};
  json.begin_object();
  json.key("format");
  json.string_value(kFormatToken);
  json.key("plan");
  write_plan_json(json, plan, options, plan_evaluation_context(plan));
  json.end_object();
}

// The byte bound of a rendered report. A coordinator snapshot carries the
// limits its durable state was written with; a single plan snapshot does not,
// so the plan report uses the library's default bound.
[[nodiscard]] std::uint64_t plan_report_limit() noexcept {
  return (std::max)(static_cast<std::uint64_t>(Limits{}.max_state_bytes), kMinimumReportBytes);
}

[[nodiscard]] std::uint64_t coordinator_report_limit(const CoordinatorSnapshot& snapshot) noexcept {
  return (std::max)(snapshot.limits.max_state_bytes, kMinimumReportBytes);
}

[[nodiscard]] Result<std::string> finish_report(TextBuffer& out, std::uint64_t limit,
                                                std::string_view what) {
  if (out.exceeded()) {
    return make_error<std::string>(ErrorCode::kTooManyEntries,
                                   std::string{"rendered "} + std::string{what} +
                                       " exceeds max_state_bytes " + format_strong(limit));
  }
  return Result<std::string>{out.take()};
}

template <typename Render>
[[nodiscard]] Result<std::string> render_bounded(std::uint64_t limit, std::string_view what,
                                                 Render&& render) {
  TextBuffer out{limit};
  render(out);
  return finish_report(out, limit, what);
}

}  // namespace

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  append_json_body(out, text);
  return out;
}

Result<std::string> export_plan_text(const DrainPlanSnapshot& plan, const ReportOptions& options) {
  const std::uint64_t limit = plan_report_limit();
  return render_bounded(limit, "plan report",
                        [&plan, &options](TextBuffer& out) { render_plan_text_document(out, plan, options); });
}

Result<std::string> export_plan_json(const DrainPlanSnapshot& plan, const ReportOptions& options) {
  const std::uint64_t limit = plan_report_limit();
  return render_bounded(limit, "plan report",
                        [&plan, &options](TextBuffer& out) { render_plan_json_document(out, plan, options); });
}

Result<std::string> export_coordinator_text(const CoordinatorSnapshot& snapshot, const ReportOptions& options) {
  const std::uint64_t limit = coordinator_report_limit(snapshot);
  return render_bounded(limit, "coordinator report",
                        [&snapshot, &options](TextBuffer& out) { render_coordinator_text(out, snapshot, options); });
}

Result<std::string> export_coordinator_json(const CoordinatorSnapshot& snapshot, const ReportOptions& options) {
  const std::uint64_t limit = coordinator_report_limit(snapshot);
  return render_bounded(limit, "coordinator report",
                        [&snapshot, &options](TextBuffer& out) { render_coordinator_json(out, snapshot, options); });
}

}  // namespace facilitydrain
