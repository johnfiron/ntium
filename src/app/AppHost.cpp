#include "src/app/AppHost.h"

#include <chrono>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace ntium::app {
namespace {

AppHostResult MakeAppHostResult(
    AppHostStatusCode code, std::string stage, std::string detail) {
  AppHostResult result;
  result.code = code;
  result.stage = std::move(stage);
  result.detail = std::move(detail);
  return result;
}

RuntimeGraphResult MakeInitFailure(std::string node, std::string detail) {
  return MakeRuntimeGraphResult(
      RuntimeGraphStatusCode::kInitializeFailed, std::move(node), std::move(detail));
}

}  // namespace

const char* ToString(AppHostStatusCode code) {
  switch (code) {
    case AppHostStatusCode::kOk:
      return "ok";
    case AppHostStatusCode::kInvalidState:
      return "invalid_state";
    case AppHostStatusCode::kBootstrapFailed:
      return "bootstrap_failed";
    case AppHostStatusCode::kShutdownFailed:
      return "shutdown_failed";
  }
  return "unknown";
}

AppHost::AppHost(AppHostConfig config) : config_(std::move(config)) {}

AppHostResult AppHost::Initialize() {
  if (initialized_) {
    return MakeAppHostResult(
        AppHostStatusCode::kInvalidState, "initialize",
        "AppHost is already initialized");
  }

  bootstrap_order_.clear();
  shutdown_order_.clear();
  notes_.clear();

  if (!runtime_graph_built_) {
    const AppHostResult build_result = BuildRuntimeGraph();
    if (!build_result.ok()) {
      return build_result;
    }
  }

  const RuntimeGraphResult graph_result = runtime_graph_.Initialize();
  const AppHostResult app_result = ConvertResult(graph_result, false);
  if (!app_result.ok()) {
    initialized_ = false;
    return app_result;
  }

  initialized_ = true;
  return MakeAppHostResult(AppHostStatusCode::kOk, "initialize", {});
}

AppHostResult AppHost::Shutdown() {
  if (!runtime_graph_built_) {
    return MakeAppHostResult(AppHostStatusCode::kOk, "shutdown", {});
  }

  const RuntimeGraphResult graph_result = runtime_graph_.Shutdown();
  initialized_ = false;
  return ConvertResult(graph_result, true);
}

#if defined(_WIN32)
bool AppHost::OnWindowMessage(
    std::uint32_t message,
    std::uintptr_t wparam,
    std::intptr_t lparam,
    std::intptr_t* out_result) {
  if (!initialized_) {
    return false;
  }

  if (message == WM_WTSSESSION_CHANGE && session_manager_ != nullptr) {
    (void)session_manager_->HandleSessionChange(
        static_cast<std::uint32_t>(wparam), lparam, NowMs());
  } else if (message == WM_POWERBROADCAST && session_manager_ != nullptr) {
    (void)session_manager_->HandlePowerBroadcast(
        static_cast<std::uint32_t>(wparam), NowMs());
  } else if (message == WM_ENDSESSION && session_manager_ != nullptr) {
    (void)session_manager_->HandleEndSession(wparam != 0U, NowMs());
  }

  if (desktop_host_ != nullptr &&
      desktop_host_->DispatchWindowMessage(message, wparam, lparam)) {
    if (out_result != nullptr) {
      *out_result = 0;
    }
    return true;
  }

  return false;
}
#endif

AppHostResult AppHost::BuildRuntimeGraph() {
  runtime_graph_ = RuntimeGraph();

  const RuntimeGraphNode startup_node{
      .name = "startup_manager",
      .initialize = [this]() { return InitializeStartupManager(); },
      .shutdown = [this]() { return ShutdownStartupManager(); },
  };
  RuntimeGraphResult result = runtime_graph_.AddNode(startup_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode session_node{
      .name = "session_manager",
      .initialize = [this]() { return InitializeSessionManager(); },
      .shutdown = [this]() { return ShutdownSessionManager(); },
  };
  result = runtime_graph_.AddNode(session_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode desktop_node{
      .name = "desktop_host",
      .initialize = [this]() { return InitializeDesktopHost(); },
      .shutdown = [this]() { return ShutdownDesktopHost(); },
  };
  result = runtime_graph_.AddNode(desktop_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode monitor_node{
      .name = "monitor_manager",
      .initialize = [this]() { return InitializeMonitorManager(); },
      .shutdown = [this]() { return ShutdownMonitorManager(); },
  };
  result = runtime_graph_.AddNode(monitor_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode render_node{
      .name = "render_runtime",
      .initialize = [this]() { return InitializeRenderRuntime(); },
      .shutdown = [this]() { return ShutdownRenderRuntime(); },
  };
  result = runtime_graph_.AddNode(render_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode input_node{
      .name = "input_runtime",
      .initialize = [this]() { return InitializeInputRuntime(); },
      .shutdown = [this]() { return ShutdownInputRuntime(); },
  };
  result = runtime_graph_.AddNode(input_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode ipc_node{
      .name = "ipc_runtime",
      .initialize = [this]() { return InitializeIpcRuntime(); },
      .shutdown = [this]() { return ShutdownIpcRuntime(); },
  };
  result = runtime_graph_.AddNode(ipc_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  const RuntimeGraphNode ingest_node{
      .name = "ingest_runtime",
      .initialize = [this]() { return InitializeIngestRuntime(); },
      .shutdown = [this]() { return ShutdownIngestRuntime(); },
  };
  result = runtime_graph_.AddNode(ingest_node);
  if (!result.ok()) {
    return ConvertResult(result, false);
  }

  runtime_graph_built_ = true;
  return MakeAppHostResult(AppHostStatusCode::kOk, "build_runtime_graph", {});
}

AppHostResult AppHost::ConvertResult(
    RuntimeGraphResult result, bool shutdown_path) const {
  if (result.ok()) {
    return MakeAppHostResult(
        AppHostStatusCode::kOk, shutdown_path ? "shutdown" : "initialize", {});
  }
  return MakeAppHostResult(
      shutdown_path ? AppHostStatusCode::kShutdownFailed
                    : AppHostStatusCode::kBootstrapFailed,
      result.node_name, std::move(result.detail));
}

std::uint64_t AppHost::NowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

RuntimeGraphResult AppHost::InitializeStartupManager() {
  startup_manager_ = std::make_unique<runtime::StartupManager>();
  const runtime::StartupDecision startup_decision =
      startup_manager_->EvaluateStartup(
          runtime::StartupContext{
              .launch_reason = runtime::StartupLaunchReason::kProcessStart,
              .now_ms = NowMs(),
          });
  if (startup_decision.disposition ==
      runtime::StartupDecisionDisposition::kDenyDuplicateInstance) {
    return MakeInitFailure(
        "startup_manager", "single-instance gate rejected duplicate host instance");
  }
  if (startup_decision.disposition ==
      runtime::StartupDecisionDisposition::kDenyPolicySuppressed) {
    return MakeInitFailure(
        "startup_manager", "startup policy suppressed initialization");
  }
  startup_manager_->MarkProcessRunning(NowMs());
  bootstrap_order_.push_back("startup_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownStartupManager() {
  if (startup_manager_ != nullptr) {
    startup_manager_->OnProcessExit(runtime::ExitContext{
        .reason = runtime::StartupExitReason::kGracefulShutdown,
        .now_ms = NowMs(),
    });
    startup_manager_->ReleaseSingleInstance();
    startup_manager_.reset();
  }
  shutdown_order_.push_back("startup_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeSessionManager() {
  runtime::SessionTransitionHooks hooks;
#if defined(_WIN32)
  hooks.rebind_desktop = [this](runtime::SessionTransitionTrigger /*trigger*/,
                                std::uint64_t /*now_ms*/) {
    if (desktop_host_ == nullptr) {
      return true;
    }
    return desktop_host_->EnsureAttached(
        platform::win32::HostRebindReason::kSessionChanged);
  };
#endif
  session_manager_ = std::make_unique<runtime::SessionManager>(std::move(hooks));
  (void)session_manager_->ProcessTrigger(
      runtime::SessionTransitionTrigger::kProcessStart, NowMs());
  bootstrap_order_.push_back("session_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownSessionManager() {
  if (session_manager_ != nullptr) {
    (void)session_manager_->ProcessTrigger(
        runtime::SessionTransitionTrigger::kShutdown, NowMs());
    session_manager_.reset();
  }
  shutdown_order_.push_back("session_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeDesktopHost() {
#if defined(_WIN32)
  desktop_host_ = std::make_unique<platform::win32::DesktopHost>();
  if (!desktop_host_->Initialize()) {
    return MakeInitFailure("desktop_host", "DesktopHost::Initialize failed");
  }
  bootstrap_order_.push_back("desktop_host");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#else
  notes_.push_back("desktop_host: skipped on non-Windows host");
  bootstrap_order_.push_back("desktop_host(stub)");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#endif
}

RuntimeGraphResult AppHost::ShutdownDesktopHost() {
#if defined(_WIN32)
  if (desktop_host_ != nullptr) {
    desktop_host_->Shutdown();
    desktop_host_.reset();
  }
#endif
  shutdown_order_.push_back("desktop_host");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeMonitorManager() {
#if defined(_WIN32)
  monitor_manager_ = std::make_unique<platform::win32::MonitorManager>();
  if (!monitor_manager_->Initialize()) {
    return MakeInitFailure(
        "monitor_manager", "MonitorManager::Initialize failed");
  }
  bootstrap_order_.push_back("monitor_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#else
  notes_.push_back("monitor_manager: skipped on non-Windows host");
  bootstrap_order_.push_back("monitor_manager(stub)");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
#endif
}

RuntimeGraphResult AppHost::ShutdownMonitorManager() {
#if defined(_WIN32)
  if (monitor_manager_ != nullptr) {
    monitor_manager_->Shutdown();
    monitor_manager_.reset();
  }
#endif
  shutdown_order_.push_back("monitor_manager");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeRenderRuntime() {
  render_device_manager_ = render::CreateRenderDeviceManager();
  swapchain_manager_ = render::CreateSwapchainManager();
  dirty_present_pipeline_ = render::CreateDirtyPresentPipeline();
  device_recovery_ = render::CreateDeviceRecoveryStateMachine();

  if (!render_device_manager_ || !swapchain_manager_ ||
      !dirty_present_pipeline_ || !device_recovery_) {
    return MakeInitFailure(
        "render_runtime", "failed to create render runtime managers");
  }

  notes_.push_back(
      "render_runtime: TODO A2 follow-up for adapter enumeration + swapchain binding");
  bootstrap_order_.push_back("render_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownRenderRuntime() {
  device_recovery_.reset();
  dirty_present_pipeline_.reset();
  swapchain_manager_.reset();
  render_device_manager_.reset();
  shutdown_order_.push_back("render_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeInputRuntime() {
  input_router_ = std::make_unique<input::InputRouter>();
  arbitration_engine_ = std::make_unique<input::ArbitrationEngine>();
  if (!input_router_ || !arbitration_engine_) {
    return MakeInitFailure(
        "input_runtime", "failed to create input runtime components");
  }

  notes_.push_back(
      "input_runtime: TODO integrate live camera command dispatch in later batch");
  bootstrap_order_.push_back("input_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownInputRuntime() {
  arbitration_engine_.reset();
  input_router_.reset();
  shutdown_order_.push_back("input_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeIpcRuntime() {
  ipc::PipeServerConfig pipe_config;
  pipe_server_ = ipc::PipeServer::Create(std::move(pipe_config));
  if (!pipe_server_) {
    return MakeInitFailure("ipc_runtime", "PipeServer::Create returned nullptr");
  }

  const ipc::PipeServerError start_error = pipe_server_->Start();
  if (start_error == ipc::PipeServerError::kNone) {
    ipc_started_ = true;
    bootstrap_order_.push_back("ipc_runtime");
    return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
  }

  ipc_started_ = false;
  if (start_error == ipc::PipeServerError::kPlatformUnsupported &&
      !config_.require_ipc_server) {
    notes_.push_back("ipc_runtime: running in platform-stub mode");
    bootstrap_order_.push_back("ipc_runtime(stub)");
    return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
  }

  return MakeInitFailure(
      "ipc_runtime",
      "TODO not implemented: PipeServer runtime loop/transport binding required");
}

RuntimeGraphResult AppHost::ShutdownIpcRuntime() {
  if (pipe_server_ != nullptr) {
    pipe_server_->Stop();
    pipe_server_.reset();
  }
  ipc_started_ = false;
  shutdown_order_.push_back("ipc_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::InitializeIngestRuntime() {
  overlay_event_store_ = std::make_unique<overlay::EventStore>();
  if (!overlay_event_store_) {
    return MakeInitFailure("ingest_runtime", "failed to create overlay event store");
  }

  ingest::SnapshotWatcherConfig watcher_config;
  watcher_config.watch_directory = config_.snapshot_watch_directory;
  watcher_config.snapshot_filename_filter = config_.snapshot_filename_filter;

  if (watcher_config.watch_directory.empty()) {
    if (config_.require_snapshot_watcher) {
      return MakeInitFailure(
          "ingest_runtime",
          "TODO not implemented: snapshot_watch_directory is required but missing");
    }
    notes_.push_back(
        "ingest_runtime: TODO configure snapshot_watch_directory when enabling live ingest");
  } else {
    snapshot_watcher_ = std::make_unique<ingest::SnapshotWatcher>(watcher_config);
    const ingest::SnapshotWatcherError watcher_error = snapshot_watcher_->Start();
    if (watcher_error == ingest::SnapshotWatcherError::kNone) {
      snapshot_watcher_started_ = true;
    } else if (watcher_error == ingest::SnapshotWatcherError::kUnsupportedPlatform &&
               !config_.require_snapshot_watcher) {
      notes_.push_back("ingest_runtime: watcher unsupported on current platform");
    } else if (config_.require_snapshot_watcher) {
      return MakeInitFailure(
          "ingest_runtime",
          "TODO not implemented: native snapshot watcher runtime integration failed");
    } else {
      notes_.push_back("ingest_runtime: snapshot watcher failed to start; continuing");
    }
  }

  bootstrap_order_.push_back("ingest_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

RuntimeGraphResult AppHost::ShutdownIngestRuntime() {
  if (snapshot_watcher_ != nullptr) {
    if (snapshot_watcher_started_) {
      (void)snapshot_watcher_->Stop();
      snapshot_watcher_started_ = false;
    }
    snapshot_watcher_.reset();
  }
  overlay_event_store_.reset();
  shutdown_order_.push_back("ingest_runtime");
  return MakeRuntimeGraphResult(RuntimeGraphStatusCode::kOk);
}

}  // namespace ntium::app
