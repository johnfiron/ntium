#include "SnapshotWatcher.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <cwctype>
#endif

namespace ntium::ingest {

namespace {

constexpr std::size_t kReadDirBufferSizeBytes = 64 * 1024;

#if defined(_WIN32)
constexpr std::uint32_t kDefaultWinNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
    FILE_NOTIFY_CHANGE_CREATION;

std::wstring ToWide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int len =
      MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(len - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), len);
  return wide;
}

bool EqualsInsensitive(const std::wstring& left,
                       const std::wstring& right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::towlower(left[i]) != std::towlower(right[i])) {
      return false;
    }
  }
  return true;
}
#endif

}  // namespace

#if defined(_WIN32)
struct SnapshotWatcher::WinImpl {
  HANDLE directory = INVALID_HANDLE_VALUE;
  HANDLE completion_port = nullptr;
  OVERLAPPED overlapped{};
  std::vector<std::uint8_t> notify_buffer;
  std::uint32_t notify_filter = kDefaultWinNotifyFilter;
  std::wstring filename_filter;
  bool read_armed = false;
};
#endif

SnapshotWatcher::SnapshotWatcher(SnapshotWatcherConfig config)
    : config_(std::move(config)) {}

SnapshotWatcher::~SnapshotWatcher() { (void)Stop(); }

SnapshotWatcherError SnapshotWatcher::Start() noexcept {
  if (running_.load(std::memory_order_acquire)) {
    return SnapshotWatcherError::kAlreadyRunning;
  }
  if (config_.watch_directory.empty()) {
    return SnapshotWatcherError::kWatchDirectoryEmpty;
  }
  if (!std::filesystem::exists(config_.watch_directory)) {
    return SnapshotWatcherError::kWatchDirectoryMissing;
  }

#if defined(_WIN32)
  auto impl = std::make_unique<WinImpl>();
  impl->notify_buffer.resize(kReadDirBufferSizeBytes);
  std::memset(&impl->overlapped, 0, sizeof(impl->overlapped));
  impl->filename_filter = ToWide(config_.snapshot_filename_filter);

  const std::wstring watch_path_wide = ToWide(config_.watch_directory);
  if (watch_path_wide.empty()) {
    return SnapshotWatcherError::kDirectoryOpenFailed;
  }

  impl->directory = CreateFileW(
      watch_path_wide.c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (impl->directory == INVALID_HANDLE_VALUE) {
    return SnapshotWatcherError::kDirectoryOpenFailed;
  }

  impl->completion_port =
      CreateIoCompletionPort(impl->directory, nullptr, 1, 0);
  if (impl->completion_port == nullptr) {
    CloseHandle(impl->directory);
    impl->directory = INVALID_HANDLE_VALUE;
    return SnapshotWatcherError::kCompletionPortCreateFailed;
  }

  DWORD bytes_returned = 0;
  const BOOL arm_ok = ReadDirectoryChangesW(
      impl->directory, impl->notify_buffer.data(),
      static_cast<DWORD>(impl->notify_buffer.size()),
      config_.watch_subdirectories ? TRUE : FALSE, impl->notify_filter,
      &bytes_returned, &impl->overlapped, nullptr);
  (void)bytes_returned;
  if (!arm_ok) {
    CloseHandle(impl->completion_port);
    CloseHandle(impl->directory);
    impl->completion_port = nullptr;
    impl->directory = INVALID_HANDLE_VALUE;
    return SnapshotWatcherError::kArmReadFailed;
  }
  impl->read_armed = true;
  win_impl_ = std::move(impl);

  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  worker_thread_ = std::thread([this]() { WorkerLoop(); });
#else
  // Non-Windows targets expose a deterministic stub surface.
  return SnapshotWatcherError::kUnsupportedPlatform;
#endif
  return SnapshotWatcherError::kNone;
}

SnapshotWatcherError SnapshotWatcher::Stop() noexcept {
  const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
  if (!was_running) {
    return SnapshotWatcherError::kNotRunning;
  }

  stop_requested_.store(true, std::memory_order_release);

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

#if defined(_WIN32)
  CleanupWinImpl();
#endif

  return SnapshotWatcherError::kNone;
}

bool SnapshotWatcher::IsRunning() const noexcept {
  return running_.load(std::memory_order_acquire);
}

std::optional<std::uint64_t> SnapshotWatcher::ClaimNextGeneration() noexcept {
  const std::uint64_t latest = latest_generation_.load(std::memory_order_acquire);
  std::uint64_t claimed = claimed_generation_.load(std::memory_order_acquire);
  while (latest > claimed) {
    if (claimed_generation_.compare_exchange_weak(
            claimed, latest, std::memory_order_acq_rel, std::memory_order_acquire)) {
      if (latest > claimed + 1) {
        coalesced_generations_.fetch_add(latest - claimed - 1,
                                         std::memory_order_relaxed);
      }
      return latest;
    }
  }
  return std::nullopt;
}

std::uint64_t SnapshotWatcher::LatestObservedGeneration() const noexcept {
  return latest_generation_.load(std::memory_order_acquire);
}

SnapshotWatcherStats SnapshotWatcher::GetStats() const noexcept {
  SnapshotWatcherStats stats{};
  stats.observed_notifications =
      observed_notifications_.load(std::memory_order_relaxed);
  stats.coalesced_generations =
      coalesced_generations_.load(std::memory_order_relaxed);
  stats.latest_generation = latest_generation_.load(std::memory_order_relaxed);
  stats.claimed_generation = claimed_generation_.load(std::memory_order_relaxed);
  return stats;
}

void SnapshotWatcher::PublishGeneration(std::uint64_t generation) noexcept {
  std::uint64_t current = latest_generation_.load(std::memory_order_acquire);
  while (generation > current &&
         !latest_generation_.compare_exchange_weak(
             current, generation, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }
}

#if defined(_WIN32)
void SnapshotWatcher::WorkerLoop() noexcept {
  while (!stop_requested_.load(std::memory_order_acquire)) {
    if (win_impl_ == nullptr || win_impl_->completion_port == nullptr) {
      break;
    }

    DWORD bytes_transferred = 0;
    ULONG_PTR completion_key = 0;
    OVERLAPPED* completed_overlapped = nullptr;
    const BOOL ok = GetQueuedCompletionStatus(
        win_impl_->completion_port, &bytes_transferred, &completion_key,
        &completed_overlapped, 250);
    (void)completion_key;

    if (!ok && completed_overlapped == nullptr) {
      const DWORD err = GetLastError();
      if (err == WAIT_TIMEOUT) {
        continue;
      }
      // IOCP failed in an unrecoverable way; terminate watcher loop.
      running_.store(false, std::memory_order_release);
      break;
    }

    if (completed_overlapped != &win_impl_->overlapped) {
      continue;
    }

    win_impl_->read_armed = false;
    std::uint64_t matched_notifications = 0;
    if (ok) {
      matched_notifications = CountMatchingNotifications(bytes_transferred);
    } else {
      const DWORD err = GetLastError();
      if (err == ERROR_OPERATION_ABORTED &&
          stop_requested_.load(std::memory_order_acquire)) {
        break;
      }
      // Directory notification overflow or similar errors force a fresh read.
      matched_notifications = 1;
    }

    if (matched_notifications > 0) {
      observed_notifications_.fetch_add(matched_notifications,
                                        std::memory_order_relaxed);
      const std::uint64_t generation =
          latest_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
      PublishGeneration(generation);
    }

    if (stop_requested_.load(std::memory_order_acquire)) {
      break;
    }
    if (!ArmReadDirectoryChanges()) {
      running_.store(false, std::memory_order_release);
      break;
    }
  }
}

bool SnapshotWatcher::ArmReadDirectoryChanges() noexcept {
  if (win_impl_ == nullptr || win_impl_->directory == INVALID_HANDLE_VALUE) {
    return false;
  }

  std::memset(&win_impl_->overlapped, 0, sizeof(win_impl_->overlapped));
  DWORD bytes_returned = 0;
  const BOOL arm_ok = ReadDirectoryChangesW(
      win_impl_->directory, win_impl_->notify_buffer.data(),
      static_cast<DWORD>(win_impl_->notify_buffer.size()),
      config_.watch_subdirectories ? TRUE : FALSE, win_impl_->notify_filter,
      &bytes_returned, &win_impl_->overlapped, nullptr);
  (void)bytes_returned;
  if (!arm_ok) {
    win_impl_->read_armed = false;
    return false;
  }
  win_impl_->read_armed = true;
  return true;
}

std::uint64_t SnapshotWatcher::CountMatchingNotifications(
    std::uint32_t bytes_transferred) const noexcept {
  if (win_impl_ == nullptr || bytes_transferred == 0) {
    // Empty completion implies change overflow/recovery required.
    return 1;
  }

  const std::uint8_t* buffer = win_impl_->notify_buffer.data();
  const std::size_t buffer_size = static_cast<std::size_t>(bytes_transferred);
  std::size_t offset = 0;
  std::uint64_t matched = 0;

  while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= buffer_size) {
    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
    const std::size_t name_len_chars = info->FileNameLength / sizeof(WCHAR);
    const std::wstring changed_relative(info->FileName, name_len_chars);

    bool include = win_impl_->filename_filter.empty();
    if (!include) {
      const std::filesystem::path changed_path(changed_relative);
      include = EqualsInsensitive(changed_path.filename().native(),
                                  win_impl_->filename_filter);
    }
    if (include) {
      ++matched;
    }

    if (info->NextEntryOffset == 0) {
      break;
    }
    offset += info->NextEntryOffset;
    if (offset >= buffer_size) {
      break;
    }
  }

  return matched;
}

void SnapshotWatcher::CleanupWinImpl() noexcept {
  if (win_impl_ == nullptr) {
    return;
  }

  if (win_impl_->directory != INVALID_HANDLE_VALUE) {
    CancelIoEx(win_impl_->directory, &win_impl_->overlapped);
    CloseHandle(win_impl_->directory);
    win_impl_->directory = INVALID_HANDLE_VALUE;
  }
  if (win_impl_->completion_port != nullptr) {
    CloseHandle(win_impl_->completion_port);
    win_impl_->completion_port = nullptr;
  }
  win_impl_.reset();
}
#endif

const char* ToString(SnapshotWatcherError error) noexcept {
  switch (error) {
    case SnapshotWatcherError::kNone:
      return "none";
    case SnapshotWatcherError::kWatchDirectoryEmpty:
      return "watch_directory_empty";
    case SnapshotWatcherError::kWatchDirectoryMissing:
      return "watch_directory_missing";
    case SnapshotWatcherError::kAlreadyRunning:
      return "already_running";
    case SnapshotWatcherError::kNotRunning:
      return "not_running";
    case SnapshotWatcherError::kUnsupportedPlatform:
      return "unsupported_platform";
    case SnapshotWatcherError::kDirectoryOpenFailed:
      return "directory_open_failed";
    case SnapshotWatcherError::kCompletionPortCreateFailed:
      return "completion_port_create_failed";
    case SnapshotWatcherError::kArmReadFailed:
      return "arm_read_failed";
  }
  return "unknown_error";
}

}  // namespace ntium::ingest
