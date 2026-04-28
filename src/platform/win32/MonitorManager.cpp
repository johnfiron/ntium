#include "MonitorManager.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shellscalingapi.h>
#endif

namespace ntium::platform::win32 {
namespace {

constexpr std::uint32_t kFallbackDpi = 96U;

#ifdef _WIN32
std::string MakeIdentityKey(const MonitorStableIdentity& identity) {
  return std::to_string(identity.adapter_luid_low) + ":" +
         std::to_string(identity.adapter_luid_high) + ":" +
         std::to_string(identity.target_id) + ":" + identity.monitor_key;
}

std::string SanitizeMonitorDeviceName(const char* device_name) {
  if (device_name == nullptr || device_name[0] == '\0') {
    return "monitor:unknown";
  }
  return std::string(device_name);
}

MonitorOrientation RotationToOrientation(std::uint32_t display_orientation) {
  switch (display_orientation) {
    case DMDO_90:
      return MonitorOrientation::kPortrait90;
    case DMDO_180:
      return MonitorOrientation::kLandscape180;
    case DMDO_270:
      return MonitorOrientation::kPortrait270;
    case DMDO_DEFAULT:
    default:
      return MonitorOrientation::kLandscape;
  }
}
#endif

std::uint32_t HashFnv1a32(const std::string& value) {
  std::uint32_t hash = 2166136261U;
  for (const unsigned char c : value) {
    hash ^= static_cast<std::uint32_t>(c);
    hash *= 16777619U;
  }
  return hash;
}

bool TopologyEntriesEquivalent(const MonitorTopologyEntry& lhs,
                              const MonitorTopologyEntry& rhs) {
  return lhs.identity.adapter_luid_low == rhs.identity.adapter_luid_low &&
         lhs.identity.adapter_luid_high == rhs.identity.adapter_luid_high &&
         lhs.identity.target_id == rhs.identity.target_id &&
         lhs.identity.monitor_key == rhs.identity.monitor_key &&
         lhs.geometry.left == rhs.geometry.left &&
         lhs.geometry.top == rhs.geometry.top &&
         lhs.geometry.right == rhs.geometry.right &&
         lhs.geometry.bottom == rhs.geometry.bottom &&
         lhs.geometry.width == rhs.geometry.width &&
         lhs.geometry.height == rhs.geometry.height &&
         lhs.geometry.orientation == rhs.geometry.orientation &&
         lhs.scale.dpi_x == rhs.scale.dpi_x &&
         lhs.scale.dpi_y == rhs.scale.dpi_y &&
         lhs.scale.scale_x == rhs.scale.scale_x &&
         lhs.scale.scale_y == rhs.scale.scale_y &&
         lhs.is_primary == rhs.is_primary;
}

#ifdef _WIN32
MonitorScaleInfo QueryMonitorScaleInfo(HMONITOR monitor_handle) {
  MonitorScaleInfo scale{};
  scale.dpi_x = kFallbackDpi;
  scale.dpi_y = kFallbackDpi;
  scale.scale_x = 1.0;
  scale.scale_y = 1.0;

  UINT dpi_x = 0U;
  UINT dpi_y = 0U;
  const HRESULT dpi_result =
      GetDpiForMonitor(monitor_handle, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);
  if (SUCCEEDED(dpi_result) && dpi_x > 0U && dpi_y > 0U) {
    scale.dpi_x = static_cast<std::uint32_t>(dpi_x);
    scale.dpi_y = static_cast<std::uint32_t>(dpi_y);
  }

  scale.scale_x = static_cast<double>(scale.dpi_x) / static_cast<double>(kFallbackDpi);
  scale.scale_y = static_cast<double>(scale.dpi_y) / static_cast<double>(kFallbackDpi);
  return scale;
}

MonitorGeometryPx QueryMonitorGeometry(const MONITORINFOEXA& info_ex) {
  MonitorGeometryPx geometry{};
  geometry.left = info_ex.rcMonitor.left;
  geometry.top = info_ex.rcMonitor.top;
  geometry.right = info_ex.rcMonitor.right;
  geometry.bottom = info_ex.rcMonitor.bottom;
  geometry.width = static_cast<std::uint32_t>(
      std::max<LONG>(0, info_ex.rcMonitor.right - info_ex.rcMonitor.left));
  geometry.height = static_cast<std::uint32_t>(
      std::max<LONG>(0, info_ex.rcMonitor.bottom - info_ex.rcMonitor.top));
  geometry.orientation = MonitorOrientation::kLandscape;

  DEVMODEA dev_mode{};
  dev_mode.dmSize = static_cast<WORD>(sizeof(dev_mode));
  if (EnumDisplaySettingsExA(info_ex.szDevice, ENUM_CURRENT_SETTINGS, &dev_mode, 0) != 0) {
    geometry.orientation = RotationToOrientation(dev_mode.dmDisplayOrientation);
  }
  return geometry;
}

MonitorStableIdentity BuildStableIdentity(const MONITORINFOEXA& info_ex) {
  MonitorStableIdentity identity{};
  identity.monitor_key = SanitizeMonitorDeviceName(info_ex.szDevice);

  DISPLAY_DEVICEA adapter_device{};
  adapter_device.cb = static_cast<DWORD>(sizeof(adapter_device));
  if (EnumDisplayDevicesA(info_ex.szDevice, 0, &adapter_device, 0) != 0) {
    const std::string adapter_name =
        adapter_device.DeviceName[0] == '\0' ? std::string("adapter:unknown")
                                             : std::string(adapter_device.DeviceName);
    const std::uint32_t adapter_hash = HashFnv1a32(adapter_name);
    identity.adapter_luid_low = adapter_hash;
    identity.adapter_luid_high = static_cast<std::int32_t>((adapter_hash >> 1U) ^ 0x5A5A5A5AU);
  } else {
    const std::uint32_t key_hash = HashFnv1a32(identity.monitor_key);
    identity.adapter_luid_low = key_hash;
    identity.adapter_luid_high = static_cast<std::int32_t>(key_hash ^ 0x13579BDFU);
  }

  const std::size_t slash_index = identity.monitor_key.find('\\');
  if (slash_index != std::string::npos) {
    const std::string suffix = identity.monitor_key.substr(slash_index + 1U);
    identity.target_id = HashFnv1a32(suffix);
  } else {
    identity.target_id = HashFnv1a32(identity.monitor_key);
  }
  return identity;
}

struct EnumeratedMonitorRecord {
  MonitorStableIdentity identity;
  MonitorGeometryPx geometry;
  MonitorScaleInfo scale;
  bool is_primary = false;
};

struct EnumerationContext {
  std::vector<EnumeratedMonitorRecord> monitors;
};

BOOL CALLBACK EnumerateMonitorCallback(HMONITOR monitor_handle,
                                       HDC device_context,
                                       LPRECT clip_rect,
                                       LPARAM user_data) {
  (void)device_context;
  (void)clip_rect;
  auto* context = reinterpret_cast<EnumerationContext*>(user_data);
  if (context == nullptr) {
    return TRUE;
  }

  MONITORINFOEXA info_ex{};
  info_ex.cbSize = static_cast<DWORD>(sizeof(info_ex));
  if (GetMonitorInfoA(monitor_handle, &info_ex) == 0) {
    return TRUE;
  }

  EnumeratedMonitorRecord record{};
  record.identity = BuildStableIdentity(info_ex);
  record.geometry = QueryMonitorGeometry(info_ex);
  record.scale = QueryMonitorScaleInfo(monitor_handle);
  record.is_primary = (info_ex.dwFlags & MONITORINFOF_PRIMARY) != 0;
  context->monitors.push_back(std::move(record));
  return TRUE;
}
#endif

}  // namespace

MonitorManager::MonitorManager() = default;

bool MonitorManager::Initialize() {
  if (initialized_) {
    return true;
  }
  initialized_ = true;
  return RefreshTopology(MonitorTopologyChangeReason::kInitial);
}

void MonitorManager::Shutdown() {
  callback_ = nullptr;
  initialized_ = false;
  snapshot_ = {};
  stable_ids_by_identity_key_.clear();
}

bool MonitorManager::RefreshTopology(MonitorTopologyChangeReason reason) {
  if (!initialized_) {
    return false;
  }
  MonitorTopologySnapshot next = BuildNextSnapshot();
  return ApplySnapshot(std::move(next), reason);
}

bool MonitorManager::OnDisplayChange(std::uint32_t bits_per_pixel,
                                     std::uint32_t width,
                                     std::uint32_t height) {
  (void)bits_per_pixel;
  (void)width;
  (void)height;
  return RefreshTopology(MonitorTopologyChangeReason::kDisplayChange);
}

bool MonitorManager::OnDpiChange() {
  return RefreshTopology(MonitorTopologyChangeReason::kDpiChange);
}

bool MonitorManager::OnDeviceChange() {
  return RefreshTopology(MonitorTopologyChangeReason::kDeviceChange);
}

void MonitorManager::SetChangeCallback(ChangeCallback callback) {
  callback_ = std::move(callback);
}

MonitorId MonitorManager::MakeStableIdForKey(const std::string& identity_key) {
  std::uint32_t stable_id = HashFnv1a32(identity_key);
  if (stable_id == 0U) {
    stable_id = 1U;
  }
  return stable_id;
}

MonitorId MonitorManager::AssignStableId(const std::string& identity_key) {
  const auto it = stable_ids_by_identity_key_.find(identity_key);
  if (it != stable_ids_by_identity_key_.end()) {
    return it->second;
  }

  MonitorId candidate = MakeStableIdForKey(identity_key);
  while (candidate == 0U) {
    ++candidate;
  }

  for (;;) {
    bool collision = false;
    for (const auto& [existing_key, existing_id] : stable_ids_by_identity_key_) {
      if (existing_id == candidate && existing_key != identity_key) {
        collision = true;
        ++candidate;
        if (candidate == 0U) {
          candidate = 1U;
        }
        break;
      }
    }
    if (!collision) {
      break;
    }
  }

  stable_ids_by_identity_key_[identity_key] = candidate;
  return candidate;
}

MonitorTopologySnapshot MonitorManager::BuildNextSnapshot() {
  MonitorTopologySnapshot next{};

#ifdef _WIN32
  EnumerationContext context{};
  (void)EnumDisplayMonitors(nullptr, nullptr, EnumerateMonitorCallback,
                            reinterpret_cast<LPARAM>(&context));

  std::sort(context.monitors.begin(), context.monitors.end(),
            [](const EnumeratedMonitorRecord& lhs, const EnumeratedMonitorRecord& rhs) {
              const std::string lhs_key = MakeIdentityKey(lhs.identity);
              const std::string rhs_key = MakeIdentityKey(rhs.identity);
              return lhs_key < rhs_key;
            });

  next.monitors.reserve(context.monitors.size());
  for (const EnumeratedMonitorRecord& record : context.monitors) {
    const std::string identity_key = MakeIdentityKey(record.identity);
    MonitorTopologyEntry entry{};
    entry.identity = record.identity;
    entry.stable_monitor_id = AssignStableId(identity_key);
    entry.geometry = record.geometry;
    entry.scale = record.scale;
    entry.is_primary = record.is_primary;
    if (entry.is_primary) {
      next.primary_monitor_id = entry.stable_monitor_id;
    }
    next.monitors.push_back(std::move(entry));
  }
#else
  // Non-Windows builds keep topology deterministic but empty.
  next = {};
#endif

  if (next.primary_monitor_id == 0U && !next.monitors.empty()) {
    next.primary_monitor_id = next.monitors.front().stable_monitor_id;
    next.monitors.front().is_primary = true;
  }

  return next;
}

MonitorTopologyDelta MonitorManager::ComputeDelta(const MonitorTopologySnapshot& previous,
                                                  const MonitorTopologySnapshot& current,
                                                  MonitorTopologyChangeReason reason) {
  MonitorTopologyDelta delta{};
  delta.reason = reason;
  delta.generation = current.generation;

  std::unordered_map<MonitorId, MonitorTopologyEntry> previous_by_id;
  previous_by_id.reserve(previous.monitors.size());
  for (const MonitorTopologyEntry& entry : previous.monitors) {
    previous_by_id.emplace(entry.stable_monitor_id, entry);
  }

  std::unordered_map<MonitorId, MonitorTopologyEntry> current_by_id;
  current_by_id.reserve(current.monitors.size());
  for (const MonitorTopologyEntry& entry : current.monitors) {
    current_by_id.emplace(entry.stable_monitor_id, entry);
  }

  for (const auto& [monitor_id, current_entry] : current_by_id) {
    const auto prev_it = previous_by_id.find(monitor_id);
    if (prev_it == previous_by_id.end()) {
      delta.added_monitor_ids.push_back(monitor_id);
      continue;
    }
    if (!TopologyEntriesEquivalent(prev_it->second, current_entry) ||
        previous.primary_monitor_id != current.primary_monitor_id) {
      delta.updated_monitor_ids.push_back(monitor_id);
    }
  }

  for (const auto& [monitor_id, _] : previous_by_id) {
    (void)_;
    if (current_by_id.find(monitor_id) == current_by_id.end()) {
      delta.removed_monitor_ids.push_back(monitor_id);
    }
  }

  std::sort(delta.added_monitor_ids.begin(), delta.added_monitor_ids.end());
  std::sort(delta.removed_monitor_ids.begin(), delta.removed_monitor_ids.end());
  std::sort(delta.updated_monitor_ids.begin(), delta.updated_monitor_ids.end());
  return delta;
}

bool MonitorManager::ApplySnapshot(MonitorTopologySnapshot next,
                                   MonitorTopologyChangeReason reason) {
  const MonitorTopologySnapshot previous = snapshot_;

  bool has_changes = false;
  if (previous.monitors.size() != next.monitors.size() ||
      previous.primary_monitor_id != next.primary_monitor_id) {
    has_changes = true;
  } else {
    for (std::size_t index = 0; index < next.monitors.size(); ++index) {
      if (!TopologyEntriesEquivalent(previous.monitors[index], next.monitors[index])) {
        has_changes = true;
        break;
      }
    }
  }

  if (!has_changes && reason != MonitorTopologyChangeReason::kInitial) {
    return false;
  }

  next.generation = previous.generation + 1U;
  snapshot_ = std::move(next);

  if (callback_) {
    MonitorTopologyChangeNotification notification{};
    notification.snapshot = snapshot_;
    notification.delta = ComputeDelta(previous, snapshot_, reason);
    callback_(notification);
  }
  return true;
}

}  // namespace ntium::platform::win32
