#pragma once

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "src/app/RuntimeGraph.h"
#include "src/ingest/SnapshotParser.h"
#include "src/ingest/SnapshotWatcher.h"
#include "src/input/ArbitrationEngine.h"
#include "src/input/InputRouter.h"
#include "src/ipc/PipeServer.h"
#include "src/overlay/DirtyRegionGenerator.h"
#include "src/overlay/EventStore.h"
#include "src/overlay/OverlayThrottle.h"
#if defined(_WIN32)
#include "src/platform/win32/DesktopHost.h"
#include "src/platform/win32/MonitorManager.h"
#endif
#include "src/render/DeviceRecovery.h"
#include "src/render/DirtyPresentPipeline.h"
#include "src/render/RenderDeviceManager.h"
#include "src/render/SwapchainManager.h"
#include "src/runtime/StateStore.h"
#include "src/runtime/SessionManager.h"
#include "src/runtime/StartupManager.h"
#include "src/scene/CameraController.h"

namespace ntium::app {

enum class AppHostStatusCode : std::uint8_t {
  kOk = 0U,
  kInvalidState = 1U,
  kBootstrapFailed = 2U,
  kShutdownFailed = 3U,
};

struct AppHostResult {
  AppHostStatusCode code = AppHostStatusCode::kOk;
  std::string stage;
  std::string detail;

  bool ok() const { return code == AppHostStatusCode::kOk; }
};

struct AppHostConfig {
  bool require_ipc_server = false;
  bool require_snapshot_watcher = false;
  std::string snapshot_watch_directory;
  std::string snapshot_filename_filter = "event_snapshot.bin";
  std::string state_store_primary_path = "data/runtime/state_store.txt";
  std::uint32_t overlay_throttle_hz = 10U;
  std::int32_t overlay_padding_px = 2;
};

const char* ToString(AppHostStatusCode code);

class AppHost {
 public:
  explicit AppHost(AppHostConfig config = {});

  AppHostResult Initialize();
  AppHostResult Shutdown();
  bool initialized() const { return initialized_; }

  const std::vector<std::string>& bootstrap_order() const { return bootstrap_order_; }
  const std::vector<std::string>& shutdown_order() const { return shutdown_order_; }
  const std::vector<std::string>& notes() const { return notes_; }

#if defined(_WIN32)
  bool OnWindowMessage(
      std::uint32_t message,
      std::uintptr_t wparam,
      std::intptr_t lparam,
      std::intptr_t* out_result);
#endif

  struct RuntimeTelemetry {
    std::uint64_t event_sequence = 0U;
    std::uint64_t present_attempts = 0U;
    std::uint64_t present_success = 0U;
    std::uint64_t overlay_throttled = 0U;
    std::uint64_t ipc_ack_count = 0U;
    std::uint64_t ipc_error_count = 0U;
  };
  RuntimeTelemetry telemetry() const { return telemetry_; }

 private:
  AppHostResult BuildRuntimeGraph();
  AppHostResult ConvertResult(RuntimeGraphResult result, bool shutdown_path) const;
  static std::uint64_t NowMs();

  RuntimeGraphResult InitializeStartupManager();
  RuntimeGraphResult ShutdownStartupManager();
  RuntimeGraphResult InitializeSessionManager();
  RuntimeGraphResult ShutdownSessionManager();
  RuntimeGraphResult InitializeDesktopHost();
  RuntimeGraphResult ShutdownDesktopHost();
  RuntimeGraphResult InitializeMonitorManager();
  RuntimeGraphResult ShutdownMonitorManager();
  RuntimeGraphResult InitializeRenderRuntime();
  RuntimeGraphResult ShutdownRenderRuntime();
  RuntimeGraphResult InitializeInputRuntime();
  RuntimeGraphResult ShutdownInputRuntime();
  RuntimeGraphResult InitializeIpcRuntime();
  RuntimeGraphResult ShutdownIpcRuntime();
  RuntimeGraphResult InitializeIngestRuntime();
  RuntimeGraphResult ShutdownIngestRuntime();

  RuntimeGraphResult RefreshRenderTopology(std::uint64_t event_sequence);
  RuntimeGraphResult DispatchDirtyPresentEvent(
      const std::vector<overlay::DirtyRegionRect>& dirty_regions,
      render::PresentReason reason,
      bool overlay_only_work,
      bool bypass_overlay_throttle,
      std::uint64_t event_sequence);
  RuntimeGraphResult PresentPendingIntents(
      bool overlay_only_work,
      bool bypass_overlay_throttle,
      std::uint64_t now_ms,
      std::uint64_t event_sequence);
  void PollRuntimeFeeds(std::uint64_t now_ms);
  void PollSnapshotWatcher(std::uint64_t now_ms);
  void ExpireOverlayEvents(std::uint64_t now_ms);
  void DrainDeferredIpcCommands(std::uint64_t now_ms);
  std::uint64_t NextEventSequence();
  bool ApplySnapshotFromBytes(
      const std::vector<std::uint8_t>& bytes,
      std::uint64_t now_ms,
      std::vector<overlay::DirtyRegionDelta>* out_deltas);
  bool ApplyIpcFrame(
      std::uint64_t connection_id,
      const ipc::PipeFrame& frame,
      std::uint64_t now_ms);
  void HandlePipeEvent(const ipc::PipeServerEvent& event);
  bool ApplyCameraRotateCommand(float delta_yaw_deg, float delta_pitch_deg);
  bool ApplyCameraZoomCommand(float delta);
  std::uint32_t BuildAckResultFlags(
      const input::ArbitrationDecision& decision) const;
  static input::ArbitrationChannel ChannelForCommand(
      ipc::PipeCommandId command_id);
  static std::string OverlayEventIdFromU64(std::uint64_t event_id);
  std::string SerializeRuntimeState(std::uint64_t now_ms) const;
  bool RestoreRuntimeState(const std::string& payload);

  AppHostConfig config_;
  RuntimeGraph runtime_graph_;
  bool runtime_graph_built_ = false;
  bool initialized_ = false;
  bool ipc_started_ = false;
  bool snapshot_watcher_started_ = false;

  std::vector<std::string> bootstrap_order_;
  std::vector<std::string> shutdown_order_;
  std::vector<std::string> notes_;
  RuntimeTelemetry telemetry_{};

  std::unique_ptr<runtime::StartupManager> startup_manager_;
  std::unique_ptr<runtime::SessionManager> session_manager_;
#if defined(_WIN32)
  std::unique_ptr<platform::win32::DesktopHost> desktop_host_;
  std::unique_ptr<platform::win32::MonitorManager> monitor_manager_;
#endif
  std::unique_ptr<render::IRenderDeviceManager> render_device_manager_;
  std::unique_ptr<render::ISwapchainManager> swapchain_manager_;
  std::unique_ptr<render::IDirtyPresentPipeline> dirty_present_pipeline_;
  std::unique_ptr<render::IDeviceRecoveryStateMachine> device_recovery_;
  std::unique_ptr<input::InputRouter> input_router_;
  std::unique_ptr<input::ArbitrationEngine> arbitration_engine_;
  std::unique_ptr<ipc::PipeServer> pipe_server_;
  std::unique_ptr<ingest::SnapshotWatcher> snapshot_watcher_;
  std::unique_ptr<overlay::EventStore> overlay_event_store_;
  std::unique_ptr<runtime::StateStore> state_store_;
  overlay::OverlayThrottle overlay_throttle_;
  scene::CameraController camera_controller_;
  std::vector<render::MonitorLayout> active_monitors_;
  std::unordered_map<std::string, std::uint8_t> overlay_record_types_;
  std::uint64_t runtime_event_sequence_ = 0U;
  std::uint64_t last_snapshot_sequence_ = 0U;
  std::uint64_t last_snapshot_generation_ = 0U;
  std::uint64_t active_pipe_connection_id_ = 0U;
  std::uint32_t control_state_revision_ = 0U;
  std::uint64_t overlay_only_deadline_ms_ = 0U;
  std::optional<std::pair<std::int32_t, std::int32_t>> last_pointer_position_px_;
};

}  // namespace ntium::app
