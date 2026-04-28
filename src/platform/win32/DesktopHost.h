#pragma once

#include <cstdint>

#include "ShellDesktopLocator.h"
#include "ShellEventRouter.h"

namespace ntium::platform::win32 {

enum class HostRebindReason : std::uint8_t {
  kInitialAttach = 0,
  kTaskbarCreated,
  kDisplayChanged,
  kSessionChanged,
  kPowerResumed,
};

struct DesktopHostAttachSnapshot {
  bool attached = false;
  std::uintptr_t def_view_window = 0;
  std::uintptr_t attach_parent_window = 0;
  // WorkerW is diagnostic-only; attach logic uses DefView + its parent.
  bool workerw_observed = false;
  std::uintptr_t workerw_window = 0;
  HostRebindReason last_reason = HostRebindReason::kInitialAttach;
};

class DesktopHost {
 public:
  DesktopHost();

  bool Initialize();
  void Shutdown();

  bool EnsureAttached(HostRebindReason reason);
  bool DispatchWindowMessage(std::uint32_t message, std::uintptr_t wparam, std::intptr_t lparam);

  // Explicit lifecycle entry points used by routing and tests.
  bool OnTaskbarCreated();
  bool OnDisplayChange(std::uint32_t bits_per_pixel,
                       std::uint32_t width,
                       std::uint32_t height);
  bool OnSessionChange(std::uint32_t session_event, std::intptr_t session_id);
  bool OnPowerBroadcast(std::uint32_t power_event_code);

  DesktopHostAttachSnapshot attach_snapshot() const { return snapshot_; }
  static const char* AttachStrategySummary();

 private:
  void HandleLifecycleEvent(const ShellLifecycleEvent& event);

  ShellDesktopLocator locator_;
  ShellEventRouter router_;
  DesktopHostAttachSnapshot snapshot_;
  bool initialized_ = false;
};

}  // namespace ntium::platform::win32
