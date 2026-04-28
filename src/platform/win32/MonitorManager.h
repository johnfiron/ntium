#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ntium::platform::win32 {

using MonitorId = std::uint32_t;

enum class MonitorOrientation : std::uint8_t {
  kLandscape = 0,
  kPortrait90,
  kLandscape180,
  kPortrait270,
};

struct MonitorStableIdentity {
  std::uint32_t adapter_luid_low = 0;
  std::int32_t adapter_luid_high = 0;
  std::uint32_t target_id = 0;
  std::string monitor_key;
};

struct MonitorGeometryPx {
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  MonitorOrientation orientation = MonitorOrientation::kLandscape;
};

struct MonitorScaleInfo {
  std::uint32_t dpi_x = 96;
  std::uint32_t dpi_y = 96;
  double scale_x = 1.0;
  double scale_y = 1.0;
};

struct MonitorTopologyEntry {
  MonitorId stable_monitor_id = 0;
  MonitorStableIdentity identity;
  MonitorGeometryPx geometry;
  MonitorScaleInfo scale;
  bool is_primary = false;
};

struct MonitorTopologySnapshot {
  std::uint64_t generation = 0;
  MonitorId primary_monitor_id = 0;
  std::vector<MonitorTopologyEntry> monitors;
};

enum class MonitorTopologyChangeReason : std::uint8_t {
  kInitial = 0,
  kDisplayChange,
  kDpiChange,
  kDeviceChange,
  kManualRefresh,
};

struct MonitorTopologyDelta {
  MonitorTopologyChangeReason reason = MonitorTopologyChangeReason::kManualRefresh;
  std::uint64_t generation = 0;
  std::vector<MonitorId> added_monitor_ids;
  std::vector<MonitorId> removed_monitor_ids;
  std::vector<MonitorId> updated_monitor_ids;
};

struct MonitorTopologyChangeNotification {
  MonitorTopologyDelta delta;
  MonitorTopologySnapshot snapshot;
};

class MonitorManager {
 public:
  using ChangeCallback = std::function<void(const MonitorTopologyChangeNotification&)>;

  MonitorManager();

  bool Initialize();
  void Shutdown();

  bool RefreshTopology(
      MonitorTopologyChangeReason reason = MonitorTopologyChangeReason::kManualRefresh);
  bool OnDisplayChange(std::uint32_t bits_per_pixel,
                       std::uint32_t width,
                       std::uint32_t height);
  bool OnDpiChange();
  bool OnDeviceChange();

  void SetChangeCallback(ChangeCallback callback);

  MonitorTopologySnapshot topology_snapshot() const { return snapshot_; }
  std::uint64_t topology_generation() const { return snapshot_.generation; }

 private:
  static MonitorId MakeStableIdForKey(const std::string& identity_key);
  MonitorId AssignStableId(const std::string& identity_key);

  MonitorTopologySnapshot BuildNextSnapshot();
  static MonitorTopologyDelta ComputeDelta(const MonitorTopologySnapshot& previous,
                                           const MonitorTopologySnapshot& current,
                                           MonitorTopologyChangeReason reason);
  bool ApplySnapshot(MonitorTopologySnapshot next, MonitorTopologyChangeReason reason);

  ChangeCallback callback_;
  MonitorTopologySnapshot snapshot_;
  bool initialized_ = false;
  std::unordered_map<std::string, MonitorId> stable_ids_by_identity_key_;
};

}  // namespace ntium::platform::win32
