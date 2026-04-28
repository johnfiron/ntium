#include "DesktopHost.h"

#ifdef _WIN32
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace ntium::platform::win32 {
namespace {

bool ShouldRebindForSessionEvent(std::uint32_t session_event) {
#ifdef _WIN32
  return session_event == WTS_SESSION_LOGON || session_event == WTS_SESSION_UNLOCK ||
         session_event == WTS_CONSOLE_CONNECT || session_event == WTS_REMOTE_CONNECT;
#else
  constexpr std::uint32_t kWtsSessionLogon = 5;
  constexpr std::uint32_t kWtsSessionUnlock = 8;
  constexpr std::uint32_t kWtsConsoleConnect = 1;
  constexpr std::uint32_t kWtsRemoteConnect = 3;
  return session_event == kWtsSessionLogon || session_event == kWtsSessionUnlock ||
         session_event == kWtsConsoleConnect || session_event == kWtsRemoteConnect;
#endif
}

bool IsPowerResumeEvent(std::uint32_t power_event_code) {
#ifdef _WIN32
  return power_event_code == PBT_APMRESUMEAUTOMATIC ||
         power_event_code == PBT_APMRESUMESUSPEND;
#else
  constexpr std::uint32_t kPbtApmResumeSuspend = 0x0007;
  constexpr std::uint32_t kPbtApmResumeAutomatic = 0x0012;
  return power_event_code == kPbtApmResumeSuspend ||
         power_event_code == kPbtApmResumeAutomatic;
#endif
}

}  // namespace

DesktopHost::DesktopHost() = default;

bool DesktopHost::Initialize() {
  if (initialized_) {
    return true;
  }

  if (!router_.Initialize(
          [this](const ShellLifecycleEvent& event) { HandleLifecycleEvent(event); })) {
    return false;
  }

  initialized_ = true;
  return EnsureAttached(HostRebindReason::kInitialAttach);
}

void DesktopHost::Shutdown() {
  if (!initialized_) {
    return;
  }

  router_.Shutdown();
  initialized_ = false;
  snapshot_.attached = false;
}

bool DesktopHost::EnsureAttached(HostRebindReason reason) {
  const DesktopAttachAnchor anchor = locator_.LocateDesktopAnchor();
  snapshot_.def_view_window = anchor.def_view_window;
  snapshot_.attach_parent_window = anchor.attach_parent_window;
  snapshot_.workerw_observed = anchor.workerw_observed;
  snapshot_.workerw_window = anchor.workerw_window;
  snapshot_.attached =
      anchor.found && anchor.def_view_window != 0 && anchor.attach_parent_window != 0;
  snapshot_.last_reason = reason;
  return snapshot_.attached;
}

bool DesktopHost::DispatchWindowMessage(std::uint32_t message,
                                        std::uintptr_t wparam,
                                        std::intptr_t lparam) {
  if (!initialized_) {
    return false;
  }
  return router_.DispatchWindowMessage(message, wparam, lparam);
}

bool DesktopHost::OnTaskbarCreated() {
  return EnsureAttached(HostRebindReason::kTaskbarCreated);
}

bool DesktopHost::OnDisplayChange(std::uint32_t bits_per_pixel,
                                  std::uint32_t width,
                                  std::uint32_t height) {
  (void)bits_per_pixel;
  (void)width;
  (void)height;
  return EnsureAttached(HostRebindReason::kDisplayChanged);
}

bool DesktopHost::OnSessionChange(std::uint32_t session_event, std::intptr_t session_id) {
  (void)session_id;
  if (!ShouldRebindForSessionEvent(session_event)) {
    return false;
  }
  return EnsureAttached(HostRebindReason::kSessionChanged);
}

bool DesktopHost::OnPowerBroadcast(std::uint32_t power_event_code) {
  if (!IsPowerResumeEvent(power_event_code)) {
    return false;
  }
  return EnsureAttached(HostRebindReason::kPowerResumed);
}

const char* DesktopHost::AttachStrategySummary() {
  return ShellDesktopLocator::StrategySummary();
}

void DesktopHost::HandleLifecycleEvent(const ShellLifecycleEvent& event) {
  switch (event.kind) {
    case ShellLifecycleEventKind::kTaskbarCreated:
      (void)OnTaskbarCreated();
      break;
    case ShellLifecycleEventKind::kDisplayChanged:
      (void)OnDisplayChange(event.detail0, event.detail1, event.detail2);
      break;
    case ShellLifecycleEventKind::kSessionChanged:
      (void)OnSessionChange(event.detail0, event.lparam);
      break;
    case ShellLifecycleEventKind::kPowerBroadcast:
      (void)OnPowerBroadcast(event.detail0);
      break;
  }
}

}  // namespace ntium::platform::win32
