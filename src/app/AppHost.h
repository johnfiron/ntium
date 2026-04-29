#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "src/app/RuntimeGraph.h"
#include "src/ingest/SnapshotWatcher.h"
#include "src/input/ArbitrationEngine.h"
#include "src/input/InputRouter.h"
#include "src/ipc/PipeServer.h"
#include "src/overlay/EventStore.h"
#if defined(_WIN32)
#include "src/platform/win32/DesktopHost.h"
#include "src/platform/win32/MonitorManager.h"
#endif
#include "src/render/DeviceRecovery.h"
#include "src/render/DirtyPresentPipeline.h"
#include "src/render/RenderDeviceManager.h"
#include "src/render/SwapchainManager.h"
#include "src/runtime/SessionManager.h"
#include "src/runtime/StartupManager.h"

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

  AppHostConfig config_;
  RuntimeGraph runtime_graph_;
  bool runtime_graph_built_ = false;
  bool initialized_ = false;
  bool ipc_started_ = false;
  bool snapshot_watcher_started_ = false;

  std::vector<std::string> bootstrap_order_;
  std::vector<std::string> shutdown_order_;
  std::vector<std::string> notes_;

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
};

}  // namespace ntium::app
