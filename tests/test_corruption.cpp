// Facility Drain Coordinator — DCCP physical fleet lifecycle.
// Copyright 2026 Summon Software Labs.
// Apache License 2.0. No telemetry transmission.
//
// Corruption. The durable format is the last line of defence for a decision
// that authorises removing physical capacity, so every byte level attack that
// can be mounted with a text editor is mounted here: a flipped bit, a truncated
// generation, an appended byte, a pointer that names something that is not
// there, and a generation that was written but never published.

#include "fdc_test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace facilitydrain;
using fdc_test::Scaffold;
using fdc_test::TempDir;
using fdc_test::workload;

namespace {

constexpr PlanId kPlan{91};

[[nodiscard]] std::vector<std::byte> read_all(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  std::vector<std::byte> bytes;
  char value = 0;
  while (stream.get(value)) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }
  return bytes;
}

void write_all(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  for (const std::byte value : bytes) {
    stream.put(static_cast<char>(std::to_integer<unsigned char>(value)));
  }
}

void flip_byte(const std::filesystem::path& path, std::uint64_t offset) {
  std::vector<std::byte> bytes = read_all(path);
  if (offset >= bytes.size()) {
    fdc_test::fail_now("the file is too short to flip the requested byte");
  }
  const auto index = static_cast<std::size_t>(offset);
  bytes[index] = static_cast<std::byte>(std::to_integer<unsigned char>(bytes[index]) ^ 0x40U);
  write_all(path, bytes);
}

[[nodiscard]] std::filesystem::path generation_path(const std::filesystem::path& root, std::uint64_t sequence) {
  std::string name{kGenerationFilePrefix};
  std::string digits = std::to_string(sequence);
  name.append(20U - digits.size(), '0');
  name.append(digits);
  name.append(kGenerationFileSuffix);
  return root / name;
}

/// Builds a store holding one committed generation and returns its path.
[[nodiscard]] std::filesystem::path seeded_store(const TempDir& dir) {
  const std::filesystem::path root = dir.sub("store");
  {
    Scaffold scaffold = Scaffold::durable(root);
    scaffold.create_plan(kPlan, {workload(1001)});
    scaffold.enumerate(kPlan, OwnerDomain::kAsi, CoverageState::kComplete, 1, {workload(1001)});
  }
  return root;
}

[[nodiscard]] std::uint64_t committed_sequence(const std::filesystem::path& root) {
  const auto inspection = inspect_store(root, Limits{});
  if (!inspection) {
    fdc_test::fail_now("inspect_store failed: " + inspection.error().to_text());
  }
  return inspection.value().sequence.value();
}

[[nodiscard]] ErrorCode open_error(const std::filesystem::path& root) {
  CoordinatorOpenRequest request;
  request.root = root;
  request.create_if_missing = false;
  auto outcome = Coordinator::open(request);
  if (outcome) {
    fdc_test::fail_now("the store opened although it should have been refused");
  }
  return outcome.error().code();
}

}  // namespace

FDC_TEST(corruption, a_flipped_bit_in_the_payload_is_detected) {
  TempDir dir{"corruption-payload"};
  const std::filesystem::path root = seeded_store(dir);
  const std::filesystem::path generation = generation_path(root, committed_sequence(root));

  const std::vector<std::byte> bytes = read_all(generation);
  FDC_CHECK(bytes.size() > kContainerHeaderSize + 8U);
  // Deep inside the payload, past every structural field.
  flip_byte(generation, kContainerHeaderSize + 40U);

  const ErrorCode code = open_error(root);
  FDC_CHECK(code == ErrorCode::kStoreChecksumMismatch || code == ErrorCode::kStoreCorrupt);
}

FDC_TEST(corruption, a_truncated_generation_is_detected) {
  TempDir dir{"corruption-truncated"};
  const std::filesystem::path root = seeded_store(dir);
  const std::filesystem::path generation = generation_path(root, committed_sequence(root));

  std::vector<std::byte> bytes = read_all(generation);
  bytes.resize(bytes.size() - 16U);
  write_all(generation, bytes);

  const ErrorCode code = open_error(root);
  FDC_CHECK(code == ErrorCode::kStoreTruncated || code == ErrorCode::kStoreChecksumMismatch ||
            code == ErrorCode::kStoreCorrupt);
}

FDC_TEST(corruption, trailing_bytes_are_refused_rather_than_ignored) {
  TempDir dir{"corruption-trailing"};
  const std::filesystem::path root = seeded_store(dir);
  const std::filesystem::path generation = generation_path(root, committed_sequence(root));

  std::vector<std::byte> bytes = read_all(generation);
  bytes.push_back(std::byte{0x5A});
  write_all(generation, bytes);

  const ErrorCode code = open_error(root);
  FDC_CHECK(code == ErrorCode::kStoreTrailingBytes || code == ErrorCode::kStoreChecksumMismatch);
}

FDC_TEST(corruption, a_flipped_bit_in_the_pointer_is_detected) {
  TempDir dir{"corruption-pointer"};
  const std::filesystem::path root = seeded_store(dir);
  flip_byte(root / std::string{kPointerFileName}, 20U);

  const ErrorCode code = open_error(root);
  FDC_CHECK(code == ErrorCode::kStoreCorrupt || code == ErrorCode::kStoreChecksumMismatch ||
            code == ErrorCode::kMissingGenerationFile);
}

FDC_TEST(corruption, a_pointer_that_names_a_missing_generation_is_reported) {
  TempDir dir{"corruption-missing"};
  const std::filesystem::path root = seeded_store(dir);
  std::error_code error;
  std::filesystem::remove(generation_path(root, committed_sequence(root)), error);
  FDC_CHECK(!error);

  const ErrorCode code = open_error(root);
  FDC_CHECK(code == ErrorCode::kMissingGenerationFile || code == ErrorCode::kStoreRecoveryFailed);
}

FDC_TEST(corruption, a_broken_magic_is_reported_as_corruption) {
  TempDir dir{"corruption-magic"};
  const std::filesystem::path root = seeded_store(dir);
  const std::filesystem::path generation = generation_path(root, committed_sequence(root));
  std::vector<std::byte> bytes = read_all(generation);
  bytes[0] = std::byte{'X'};
  write_all(generation, bytes);

  const ErrorCode code = open_error(root);
  FDC_CHECK(code == ErrorCode::kStoreCorrupt || code == ErrorCode::kStoreChecksumMismatch);
}

FDC_TEST(corruption, an_unpublished_generation_is_refused_rather_than_adopted) {
  TempDir dir{"corruption-unpublished"};
  const std::filesystem::path root = seeded_store(dir);
  const std::uint64_t published = committed_sequence(root);

  // A complete, valid looking generation that the pointer does not name. It
  // could be the result of a publish that never finished, and whether the
  // mutation happened is not knowable from the bytes.
  std::vector<std::byte> bytes = read_all(generation_path(root, published));
  write_all(generation_path(root, published + 1U), bytes);

  const ErrorCode code = open_error(root);
  FDC_CHECK(code == ErrorCode::kStoreRecoveryFailed);
}

FDC_TEST(corruption, unrelated_files_are_ignored_and_malformed_names_are_not_interpreted) {
  TempDir dir{"corruption-foreign"};
  const std::filesystem::path root = seeded_store(dir);
  {
    std::ofstream unrelated{root / "notes.txt", std::ios::binary};
    unrelated << "an operator left this here";
  }
  {
    std::ofstream malformed{root / "gen-not-a-sequence.fdcdrain", std::ios::binary};
    malformed << "neither is this";
  }
  {
    std::ofstream wrong_suffix{root / "gen-00000000000000000099.txt", std::ios::binary};
    wrong_suffix << "nor this";
  }

  CoordinatorOpenRequest request;
  request.root = root;
  auto outcome = Coordinator::open(request);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK(outcome.value().plan(kPlan).has_value());
  FDC_CHECK_EQ(outcome.value().plan(kPlan).value().spec.revision.value(), 1U);
}

FDC_TEST(corruption, a_transient_file_left_by_a_failed_publish_is_removed_not_read) {
  TempDir dir{"corruption-transient"};
  const std::filesystem::path root = seeded_store(dir);
  const std::filesystem::path transient = root / (std::string{kTransientFilePrefix} + "leftover");
  {
    std::ofstream file{transient, std::ios::binary};
    file << "half a header";
  }

  CoordinatorOpenRequest request;
  request.root = root;
  auto outcome = Coordinator::open(request);
  FDC_REQUIRE_OK(outcome);
  FDC_CHECK_EQ(outcome.value().recovery().transient_files_removed, 1U);
  FDC_CHECK(!std::filesystem::exists(transient));
  FDC_CHECK(outcome.value().plan(kPlan).has_value());
}

FDC_TEST(corruption, a_corrupted_staged_container_never_reaches_the_pointer) {
  TempDir dir{"corruption-staged"};
  const std::filesystem::path root = dir.sub("store");
  {
    PublishFaultHooks faults;
    faults.corrupt_staged = true;
    faults.corrupt_staged_offset = kContainerHeaderSize + 4U;
    Scaffold scaffold = Scaffold::durable(root, Limits{}, faults);
    FDC_CHECK_EQ(scaffold.coordinator().commit_sequence().value(), 0U);
  }
  // Nothing was published, and the directory is not claimed as a store.
  FDC_CHECK(!std::filesystem::exists(root / std::string{kPointerFileName}));
}
