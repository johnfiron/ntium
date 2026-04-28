#include "src/runtime/StateStore.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace ntium::runtime {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::string EscapePayload(const std::string& payload) {
  std::string escaped;
  escaped.reserve(payload.size());
  for (const char c : payload) {
    switch (c) {
      case '\\':
        escaped.append("\\\\");
        break;
      case '\n':
        escaped.append("\\n");
        break;
      case '\r':
        escaped.append("\\r");
        break;
      default:
        escaped.push_back(c);
        break;
    }
  }
  return escaped;
}

bool UnescapePayload(const std::string& escaped, std::string* out_payload) {
  if (out_payload == nullptr) {
    return false;
  }
  std::string payload;
  payload.reserve(escaped.size());
  for (std::size_t i = 0; i < escaped.size(); ++i) {
    const char c = escaped[i];
    if (c != '\\') {
      payload.push_back(c);
      continue;
    }
    if (i + 1U >= escaped.size()) {
      return false;
    }
    const char next = escaped[++i];
    switch (next) {
      case '\\':
        payload.push_back('\\');
        break;
      case 'n':
        payload.push_back('\n');
        break;
      case 'r':
        payload.push_back('\r');
        break;
      default:
        return false;
    }
  }
  *out_payload = std::move(payload);
  return true;
}

StateStoreStatus RemovePath(const std::string& path) {
  if (path.empty()) {
    return StateStoreStatus::kInvalidArgument;
  }
  std::error_code error;
  (void)std::filesystem::remove(path, error);
  if (error) {
    return StateStoreStatus::kIoError;
  }
  return StateStoreStatus::kOk;
}

}  // namespace

StateStore::StateStore(StateStorePaths paths, StateStoreOptions options)
    : paths_(std::move(paths)), options_(options) {}

StateStorePaths StateStore::DerivePathsFromPrimary(std::string primary_path) {
  StateStorePaths paths;
  paths.primary_path = std::move(primary_path);
  paths.temp_path = paths.primary_path + ".tmp";
  paths.backup_path = paths.primary_path + ".bak";
  return paths;
}

PersistResult StateStore::PersistAtomic(const StateSnapshot& snapshot) const {
  PersistResult result;
  if (paths_.primary_path.empty() || paths_.temp_path.empty()) {
    result.status = StateStoreStatus::kInvalidArgument;
    return result;
  }

  std::string serialized;
  result.status = SerializeSnapshot(snapshot, &serialized);
  if (result.status != StateStoreStatus::kOk) {
    return result;
  }

  result.status = WriteFileText(paths_.temp_path, serialized);
  if (result.status != StateStoreStatus::kOk) {
    BestEffortDelete(paths_.temp_path);
    return result;
  }

  if (options_.verify_after_write) {
    std::string tmp_content;
    result.status = ReadFileText(paths_.temp_path, &tmp_content);
    if (result.status != StateStoreStatus::kOk || tmp_content != serialized) {
      result.status = StateStoreStatus::kIoError;
      BestEffortDelete(paths_.temp_path);
      return result;
    }
  }

  const bool had_primary_before = PathExists(paths_.primary_path);
  if (options_.maintain_backup && had_primary_before &&
      !paths_.backup_path.empty()) {
    result.status = CopyFileReplace(paths_.primary_path, paths_.backup_path);
    if (result.status != StateStoreStatus::kOk) {
      BestEffortDelete(paths_.temp_path);
      return result;
    }
    result.backup_updated = true;
  }

  result.status = ReplacePathAtomically(paths_.temp_path, paths_.primary_path);
  if (result.status != StateStoreStatus::kOk) {
    // Roll back to previous primary from backup if replacement failed.
    if (result.backup_updated && PathExists(paths_.backup_path)) {
      const StateStoreStatus rollback_status =
          CopyFileReplace(paths_.backup_path, paths_.primary_path);
      result.rollback_applied = rollback_status == StateStoreStatus::kOk;
    }
    BestEffortDelete(paths_.temp_path);
    return result;
  }

  return result;
}

RestoreResult StateStore::RestoreWithFallback() const {
  RestoreResult result;
  if (paths_.primary_path.empty()) {
    result.status = StateStoreStatus::kInvalidArgument;
    return result;
  }

  auto try_restore = [this](const std::string& path,
                            StateSnapshot* out_snapshot) -> StateStoreStatus {
    if (!PathExists(path)) {
      return StateStoreStatus::kNotFound;
    }
    std::string content;
    StateStoreStatus status = ReadFileText(path, &content);
    if (status != StateStoreStatus::kOk) {
      return status;
    }
    return DeserializeSnapshot(content, out_snapshot);
  };

  StateStoreStatus primary_status = try_restore(paths_.primary_path, &result.snapshot);
  if (primary_status == StateStoreStatus::kOk) {
    result.status = StateStoreStatus::kOk;
    return result;
  }

  if (primary_status != StateStoreStatus::kCorruptData ||
      paths_.backup_path.empty()) {
    result.status = primary_status;
    return result;
  }

  StateSnapshot backup_snapshot;
  StateStoreStatus backup_status = try_restore(paths_.backup_path, &backup_snapshot);
  if (backup_status != StateStoreStatus::kOk) {
    result.status = backup_status;
    return result;
  }

  result.used_backup = true;
  result.snapshot = std::move(backup_snapshot);
  result.status = StateStoreStatus::kOk;

  if (options_.repair_primary_from_backup) {
    PersistResult persist_result = PersistAtomic(result.snapshot);
    if (persist_result.status == StateStoreStatus::kOk) {
      result.repaired_primary = true;
    }
  }
  return result;
}

StateStoreStatus StateStore::RemoveAllState() const {
  StateStoreStatus aggregate = StateStoreStatus::kOk;
  for (const std::string* path :
       {&paths_.primary_path, &paths_.temp_path, &paths_.backup_path}) {
    if (path->empty()) {
      continue;
    }
    const StateStoreStatus remove_status = RemovePath(*path);
    if (remove_status != StateStoreStatus::kOk &&
        remove_status != StateStoreStatus::kNotFound) {
      aggregate = remove_status;
    }
  }
  return aggregate;
}

std::uint64_t StateStore::ComputeChecksum(const std::string& payload) {
  std::uint64_t hash = kFnvOffset;
  for (const unsigned char c : payload) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

StateStoreStatus StateStore::SerializeSnapshot(
    const StateSnapshot& snapshot,
    std::string* out_serialized) {
  if (out_serialized == nullptr) {
    return StateStoreStatus::kInvalidArgument;
  }

  const std::string escaped_payload = EscapePayload(snapshot.payload);
  const std::uint64_t checksum = ComputeChecksum(snapshot.payload);

  std::ostringstream output;
  output << "schema_version=" << snapshot.schema_version << "\n";
  output << "sequence=" << snapshot.sequence << "\n";
  output << "saved_at_ms=" << snapshot.saved_at_ms << "\n";
  output << "checksum=" << checksum << "\n";
  output << "payload=" << escaped_payload << "\n";

  *out_serialized = output.str();
  return StateStoreStatus::kOk;
}

StateStoreStatus StateStore::DeserializeSnapshot(
    const std::string& serialized,
    StateSnapshot* out_snapshot) {
  if (out_snapshot == nullptr) {
    return StateStoreStatus::kInvalidArgument;
  }

  std::istringstream input(serialized);
  StateSnapshot parsed;
  bool has_schema = false;
  bool has_sequence = false;
  bool has_saved_at = false;
  bool has_checksum = false;
  bool has_payload = false;
  std::uint64_t expected_checksum = 0U;

  std::string line;
  while (std::getline(input, line)) {
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0U) {
      continue;
    }
    const std::string key = line.substr(0U, equals);
    const std::string value = line.substr(equals + 1U);
    try {
      if (key == "schema_version") {
        parsed.schema_version = static_cast<std::uint32_t>(std::stoul(value));
        has_schema = true;
      } else if (key == "sequence") {
        parsed.sequence = static_cast<std::uint64_t>(std::stoull(value));
        has_sequence = true;
      } else if (key == "saved_at_ms") {
        parsed.saved_at_ms = static_cast<std::uint64_t>(std::stoull(value));
        has_saved_at = true;
      } else if (key == "checksum") {
        expected_checksum = static_cast<std::uint64_t>(std::stoull(value));
        has_checksum = true;
      } else if (key == "payload") {
        if (!UnescapePayload(value, &parsed.payload)) {
          return StateStoreStatus::kCorruptData;
        }
        has_payload = true;
      }
    } catch (...) {
      return StateStoreStatus::kCorruptData;
    }
  }

  if (!has_schema || !has_sequence || !has_saved_at || !has_checksum ||
      !has_payload) {
    return StateStoreStatus::kCorruptData;
  }

  if (ComputeChecksum(parsed.payload) != expected_checksum) {
    return StateStoreStatus::kCorruptData;
  }

  *out_snapshot = std::move(parsed);
  return StateStoreStatus::kOk;
}

bool StateStore::PathExists(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

StateStoreStatus StateStore::ReadFileText(const std::string& path,
                                          std::string* out_content) {
  if (path.empty() || out_content == nullptr) {
    return StateStoreStatus::kInvalidArgument;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return PathExists(path) ? StateStoreStatus::kIoError : StateStoreStatus::kNotFound;
  }
  std::ostringstream content;
  content << input.rdbuf();
  if (!input.good() && !input.eof()) {
    return StateStoreStatus::kIoError;
  }

  *out_content = content.str();
  return StateStoreStatus::kOk;
}

StateStoreStatus StateStore::WriteFileText(const std::string& path,
                                           const std::string& content) {
  if (path.empty()) {
    return StateStoreStatus::kInvalidArgument;
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return StateStoreStatus::kIoError;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  output.flush();
  if (!output.good()) {
    return StateStoreStatus::kIoError;
  }
  return StateStoreStatus::kOk;
}

StateStoreStatus StateStore::CopyFileReplace(const std::string& source_path,
                                             const std::string& destination_path) {
  if (source_path.empty() || destination_path.empty()) {
    return StateStoreStatus::kInvalidArgument;
  }
  if (!PathExists(source_path)) {
    return StateStoreStatus::kNotFound;
  }

#ifdef _WIN32
  if (CopyFileA(source_path.c_str(), destination_path.c_str(), FALSE) != 0) {
    return StateStoreStatus::kOk;
  }
  return StateStoreStatus::kIoError;
#else
  std::error_code error;
  std::filesystem::copy_file(
      source_path,
      destination_path,
      std::filesystem::copy_options::overwrite_existing,
      error);
  if (error) {
    return StateStoreStatus::kIoError;
  }
  return StateStoreStatus::kOk;
#endif
}

StateStoreStatus StateStore::ReplacePathAtomically(
    const std::string& source_tmp_path,
    const std::string& destination_path) {
  if (source_tmp_path.empty() || destination_path.empty()) {
    return StateStoreStatus::kInvalidArgument;
  }

#ifdef _WIN32
  if (MoveFileExA(
          source_tmp_path.c_str(),
          destination_path.c_str(),
          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
    return StateStoreStatus::kOk;
  }
  return StateStoreStatus::kIoError;
#else
  std::error_code error;
  std::filesystem::rename(source_tmp_path, destination_path, error);
  if (error) {
    return StateStoreStatus::kIoError;
  }
  return StateStoreStatus::kOk;
#endif
}

void StateStore::BestEffortDelete(const std::string& path) {
  if (path.empty()) {
    return;
  }
  std::error_code error;
  (void)std::filesystem::remove(path, error);
}

}  // namespace ntium::runtime
