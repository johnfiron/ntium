#pragma once

#include <cstdint>
#include <string>

namespace ntium::runtime {

enum class StateStoreStatus : std::uint8_t {
  kOk = 0U,
  kNotFound = 1U,
  kInvalidArgument = 2U,
  kIoError = 3U,
  kCorruptData = 4U,
};

struct StateSnapshot {
  std::uint32_t schema_version = 1U;
  std::uint64_t sequence = 0U;
  std::uint64_t saved_at_ms = 0U;
  std::string payload;
};

struct StateStorePaths {
  std::string primary_path;
  std::string temp_path;
  std::string backup_path;
};

struct StateStoreOptions {
  bool verify_after_write = true;
  bool maintain_backup = true;
  bool repair_primary_from_backup = true;
};

struct PersistResult {
  StateStoreStatus status = StateStoreStatus::kOk;
  bool backup_updated = false;
  bool rollback_applied = false;
};

struct RestoreResult {
  StateStoreStatus status = StateStoreStatus::kOk;
  bool used_backup = false;
  bool repaired_primary = false;
  StateSnapshot snapshot {};
};

class StateStore {
 public:
  explicit StateStore(
      StateStorePaths paths,
      StateStoreOptions options = {});

  static StateStorePaths DerivePathsFromPrimary(std::string primary_path);

  const StateStorePaths& paths() const { return paths_; }
  const StateStoreOptions& options() const { return options_; }

  PersistResult PersistAtomic(const StateSnapshot& snapshot) const;
  RestoreResult RestoreWithFallback() const;

  // Best-effort cleanup utility for test harnesses.
  StateStoreStatus RemoveAllState() const;

 private:
  static std::uint64_t ComputeChecksum(const std::string& payload);
  static StateStoreStatus SerializeSnapshot(
      const StateSnapshot& snapshot,
      std::string* out_serialized);
  static StateStoreStatus DeserializeSnapshot(
      const std::string& serialized,
      StateSnapshot* out_snapshot);

  static bool PathExists(const std::string& path);
  static StateStoreStatus ReadFileText(
      const std::string& path,
      std::string* out_content);
  static StateStoreStatus WriteFileText(
      const std::string& path,
      const std::string& content);
  static StateStoreStatus CopyFileReplace(
      const std::string& source_path,
      const std::string& destination_path);
  static StateStoreStatus ReplacePathAtomically(
      const std::string& source_tmp_path,
      const std::string& destination_path);
  static void BestEffortDelete(const std::string& path);

  StateStorePaths paths_;
  StateStoreOptions options_ {};
};

}  // namespace ntium::runtime
