#pragma once

#include <cstdint>
#include <functional>

namespace ntium::platform::win32 {

enum class ShellLifecycleEventKind : std::uint8_t {
  kTaskbarCreated = 0,
  kDisplayChanged,
  kSessionChanged,
  kPowerBroadcast,
};

struct ShellLifecycleEvent {
  ShellLifecycleEventKind kind = ShellLifecycleEventKind::kTaskbarCreated;
  std::uint32_t message = 0;
  std::uintptr_t wparam = 0;
  std::intptr_t lparam = 0;
  std::uint32_t detail0 = 0;
  std::uint32_t detail1 = 0;
  std::uint32_t detail2 = 0;
};

class ShellEventRouter {
 public:
  using EventCallback = std::function<void(const ShellLifecycleEvent&)>;

  ShellEventRouter();
  ~ShellEventRouter();

  bool Initialize(EventCallback callback);
  void Shutdown();

  std::uint32_t taskbar_created_message() const { return taskbar_created_message_; }
  bool DispatchWindowMessage(std::uint32_t message, std::uintptr_t wparam, std::intptr_t lparam);

  // Explicit entry points so lifecycle logic is testable without HWND/WndProc.
  bool OnTaskbarCreated();
  bool OnDisplayChange(std::uint32_t bits_per_pixel, std::uint32_t width, std::uint32_t height);
  bool OnSessionChange(std::uint32_t session_event, std::intptr_t session_id);
  bool OnPowerBroadcast(std::uint32_t power_event_code, std::intptr_t power_data);

 private:
  bool Emit(const ShellLifecycleEvent& event);

  EventCallback callback_;
  std::uint32_t taskbar_created_message_ = 0;
  bool initialized_ = false;
};

}  // namespace ntium::platform::win32
