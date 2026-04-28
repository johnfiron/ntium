#pragma once

#include <cstdint>
#include <functional>

namespace ntium::input {

enum class InputMode : std::uint8_t {
  kPassive = 0U,
  kActive = 1U,
};

enum class InputFocusState : std::uint8_t {
  kUnfocused = 0U,
  kFocused = 1U,
};

enum class InputCaptureState : std::uint8_t {
  kNone = 0U,
  kPointerCaptured = 1U,
};

enum class InputTransitionReason : std::uint8_t {
  kUnknown = 0U,
  kUserRequest = 1U,
  kFocusGained = 2U,
  kFocusLost = 3U,
  kCaptureAcquired = 4U,
  kCaptureReleased = 5U,
  kWindowShown = 6U,
  kWindowHidden = 7U,
  kSystemCancel = 8U,
};

enum class InputRouterEventKind : std::uint8_t {
  kModeChanged = 0U,
  kFocusChanged = 1U,
  kCaptureChanged = 2U,
  kInteractionChanged = 3U,
  kRoutingSuppressed = 4U,
};

struct InputRouterSnapshot {
  InputMode mode = InputMode::kPassive;
  InputFocusState focus_state = InputFocusState::kUnfocused;
  InputCaptureState capture_state = InputCaptureState::kNone;
  bool interaction_active = false;
  std::uint64_t interaction_epoch = 0U;
};

struct InputRouterEvent {
  InputRouterEventKind kind = InputRouterEventKind::kModeChanged;
  InputTransitionReason reason = InputTransitionReason::kUnknown;
  InputRouterSnapshot previous{};
  InputRouterSnapshot current{};
  bool should_flush_transient_input = false;
};

using InputRouterEventCallback = std::function<void(const InputRouterEvent&)>;

struct InputRouterConfig {
  // Test hook used by non-Windows hosts. On Win32, active mode should usually
  // still require WM_SETFOCUS after foreground activation.
  bool allow_active_without_focus = false;
  bool auto_passivate_on_focus_loss = true;
  bool auto_release_capture_on_focus_loss = true;
  InputRouterEventCallback event_callback;
};

class InputRouter {
 public:
  explicit InputRouter(InputRouterConfig config = {});

  const InputRouterSnapshot& snapshot() const { return snapshot_; }

  bool SetMode(InputMode mode, InputTransitionReason reason);
  bool OnFocusChanged(bool focused, InputTransitionReason reason);

  // Win32 hook: call after successful SetCapture(hwnd).
  bool AcquirePointerCapture(InputTransitionReason reason);

  // Win32 hook: call after ReleaseCapture() or WM_CAPTURECHANGED.
  bool ReleasePointerCapture(InputTransitionReason reason);

  bool BeginInteraction(InputTransitionReason reason);
  bool EndInteraction(InputTransitionReason reason);
  void CancelInteractionForSystem(
      InputTransitionReason reason = InputTransitionReason::kSystemCancel);

  bool CanRoutePointerInput() const;
  bool CanRouteKeyboardInput() const;
  bool ShouldHoldUserLease() const;

 private:
  bool CanEnterActiveMode() const;
  void EmitEvent(InputRouterEventKind kind,
                 InputTransitionReason reason,
                 const InputRouterSnapshot& previous,
                 bool should_flush_transient_input = false);

  InputRouterConfig config_;
  InputRouterSnapshot snapshot_{};
};

}  // namespace ntium::input
