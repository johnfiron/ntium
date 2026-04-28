#include "ShellEventRouter.h"

#ifdef _WIN32
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace ntium::platform::win32 {

ShellEventRouter::ShellEventRouter() = default;
ShellEventRouter::~ShellEventRouter() = default;

bool ShellEventRouter::Initialize(EventCallback callback) {
  callback_ = std::move(callback);

  if (initialized_) {
    return true;
  }

#ifdef _WIN32
  taskbar_created_message_ = RegisterWindowMessageA("TaskbarCreated");
#else
  // Non-Windows builds cannot register shell messages. A stable placeholder
  // value keeps tests deterministic while indicating unsupported integration.
  taskbar_created_message_ = 0;
#endif

  initialized_ = true;
  return true;
}

void ShellEventRouter::Shutdown() {
  callback_ = nullptr;
  initialized_ = false;
  taskbar_created_message_ = 0;
}

bool ShellEventRouter::DispatchWindowMessage(std::uint32_t message,
                                             std::uintptr_t wparam,
                                             std::intptr_t lparam) {
  if (!initialized_) {
    return false;
  }

#ifdef _WIN32
  if (taskbar_created_message_ != 0 && message == taskbar_created_message_) {
    return OnTaskbarCreated();
  }
  if (message == WM_DISPLAYCHANGE) {
    const auto bits_per_pixel = static_cast<std::uint32_t>(wparam);
    const auto width = static_cast<std::uint32_t>(LOWORD(static_cast<DWORD>(lparam)));
    const auto height = static_cast<std::uint32_t>(HIWORD(static_cast<DWORD>(lparam)));
    return OnDisplayChange(bits_per_pixel, width, height);
  }
  if (message == WM_WTSSESSION_CHANGE) {
    const auto session_event = static_cast<std::uint32_t>(wparam);
    return OnSessionChange(session_event, lparam);
  }
  if (message == WM_POWERBROADCAST) {
    const auto power_event_code = static_cast<std::uint32_t>(wparam);
    return OnPowerBroadcast(power_event_code, lparam);
  }
#else
  (void)message;
  (void)wparam;
  (void)lparam;
#endif

  return false;
}

bool ShellEventRouter::OnTaskbarCreated() {
  ShellLifecycleEvent event;
  event.kind = ShellLifecycleEventKind::kTaskbarCreated;
  event.message = taskbar_created_message_;
  return Emit(event);
}

bool ShellEventRouter::OnDisplayChange(std::uint32_t bits_per_pixel,
                                       std::uint32_t width,
                                       std::uint32_t height) {
  ShellLifecycleEvent event;
  event.kind = ShellLifecycleEventKind::kDisplayChanged;
#ifdef _WIN32
  event.message = WM_DISPLAYCHANGE;
#endif
  event.detail0 = bits_per_pixel;
  event.detail1 = width;
  event.detail2 = height;
  return Emit(event);
}

bool ShellEventRouter::OnSessionChange(std::uint32_t session_event,
                                       std::intptr_t session_id) {
  ShellLifecycleEvent event;
  event.kind = ShellLifecycleEventKind::kSessionChanged;
#ifdef _WIN32
  event.message = WM_WTSSESSION_CHANGE;
#endif
  event.wparam = session_event;
  event.lparam = session_id;
  event.detail0 = session_event;
  return Emit(event);
}

bool ShellEventRouter::OnPowerBroadcast(std::uint32_t power_event_code,
                                        std::intptr_t power_data) {
  ShellLifecycleEvent event;
  event.kind = ShellLifecycleEventKind::kPowerBroadcast;
#ifdef _WIN32
  event.message = WM_POWERBROADCAST;
#endif
  event.wparam = power_event_code;
  event.lparam = power_data;
  event.detail0 = power_event_code;
  return Emit(event);
}

bool ShellEventRouter::Emit(const ShellLifecycleEvent& event) {
  if (!initialized_ || !callback_) {
    return false;
  }
  callback_(event);
  return true;
}

}  // namespace ntium::platform::win32
