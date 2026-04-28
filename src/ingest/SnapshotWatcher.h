#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace ntium::ingest {

enum class SnapshotWatcherError : std::uint16_t {
  kNone = 0,
  kWatchDirectoryEmpty = 100,
  kWatchDirectoryMissing = 101,
  kAlreadyRunning = 102,
  kNotRunning = 103,
  kUnsupportedPlatform = 104,
  kDirectoryOpenFailed = 200,
  kCompletionPortCreateFailed = 201,
  kArmReadFailed = 202,
};

struct SnapshotWatcherConfig {
  // Absolute or relative directory containing snapshot file updates.
  std::string watch_directory;

  // Optional exact filename filter for notifications (for example:
  // "event_snapshot.bin"). Empty means all file changes in directory.
  std::string snapshot_filename_filter;

  // Mirrors ReadDirectoryChangesW's subtree behavior on Windows.
  bool watch_subdirectories = false;
};

struct SnapshotWatcherStats {
  std::uint64_t observed_notifications = 0;
  std::uint64_t coalesced_generations = 0;
  std::uint64_t latest_generation = 0;
  std::uint64_t claimed_generation = 0;
};

class SnapshotWatcher {
 public:
  explicit SnapshotWatcher(SnapshotWatcherConfig config);
  ~SnapshotWatcher();

  SnapshotWatcher(const SnapshotWatcher&) = delete;
  SnapshotWatcher& operator=(const SnapshotWatcher&) = delete;

  SnapshotWatcherError Start() noexcept;
  SnapshotWatcherError Stop() noexcept;

  bool IsRunning() const noexcept;

  // Returns newest generation and coalesces all older unclaimed generations.
  std::optional<std::uint64_t> ClaimNextGeneration() noexcept;

  std::uint64_t LatestObservedGeneration() const noexcept;
  SnapshotWatcherStats GetStats() const noexcept;

  static constexpr bool SupportsNativeWatcher() noexcept {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
  }

 private:
  SnapshotWatcherConfig config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint64_t> latest_generation_{0};
  std::atomic<std::uint64_t> claimed_generation_{0};
  std::atomic<std::uint64_t> observed_notifications_{0};
  std::atomic<std::uint64_t> coalesced_generations_{0};
  std::thread worker_thread_;

  void PublishGeneration(std::uint64_t generation) noexcept;

#if defined(_WIN32)
  struct WinImpl;
  std::unique_ptr<WinImpl> win_impl_;

  void WorkerLoop() noexcept;
  bool ArmReadDirectoryChanges() noexcept;
  std::uint64_t CountMatchingNotifications(std::uint32_t bytes_transferred) const noexcept;
  void CleanupWinImpl() noexcept;
#endif
};

const char* ToString(SnapshotWatcherError error) noexcept;

}  // namespace ntium::ingest
