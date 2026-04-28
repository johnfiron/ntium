#include "src/input/InputRouter.h"

#include <utility>

namespace ntium::input {

InputRouter::InputRouter(InputRouterConfig config) : config_(std::move(config)) {}

bool InputRouter::SetMode(InputMode mode, InputTransitionReason reason) {
  if (mode == snapshot_.mode) {
    return false;
  }

  if (mode == InputMode::kActive && !CanEnterActiveMode()) {
    EmitEvent(InputRouterEventKind::kRoutingSuppressed, reason, snapshot_, false);
    return false;
  }

  const InputRouterSnapshot previous = snapshot_;
  snapshot_.mode = mode;
  if (mode == InputMode::kPassive) {
    snapshot_.capture_state = InputCaptureState::kNone;
    snapshot_.interaction_active = false;
  }

  EmitEvent(InputRouterEventKind::kModeChanged, reason, previous, false);
  if (previous.capture_state != snapshot_.capture_state) {
    EmitEvent(InputRouterEventKind::kCaptureChanged, reason, previous,
              previous.interaction_active);
  }
  if (previous.interaction_active != snapshot_.interaction_active) {
    EmitEvent(InputRouterEventKind::kInteractionChanged, reason, previous,
              previous.interaction_active);
  }
  return true;
}

bool InputRouter::OnFocusChanged(bool focused, InputTransitionReason reason) {
  const InputFocusState target =
      focused ? InputFocusState::kFocused : InputFocusState::kUnfocused;
  if (target == snapshot_.focus_state) {
    return false;
  }

  const InputRouterSnapshot focus_previous = snapshot_;
  snapshot_.focus_state = target;
  EmitEvent(InputRouterEventKind::kFocusChanged, reason, focus_previous, false);

  if (focused) {
    return true;
  }

  if (config_.auto_passivate_on_focus_loss) {
    SetMode(InputMode::kPassive, reason);
    return true;
  }

  if (config_.auto_release_capture_on_focus_loss &&
      snapshot_.capture_state != InputCaptureState::kNone) {
    const InputRouterSnapshot capture_previous = snapshot_;
    snapshot_.capture_state = InputCaptureState::kNone;
    EmitEvent(InputRouterEventKind::kCaptureChanged, reason, capture_previous,
              capture_previous.interaction_active);
  }

  if (snapshot_.interaction_active) {
    const InputRouterSnapshot interaction_previous = snapshot_;
    snapshot_.interaction_active = false;
    EmitEvent(InputRouterEventKind::kInteractionChanged, reason,
              interaction_previous, true);
  }

  return true;
}

bool InputRouter::AcquirePointerCapture(InputTransitionReason reason) {
  if (snapshot_.capture_state == InputCaptureState::kPointerCaptured) {
    return false;
  }
  if (snapshot_.mode != InputMode::kActive || !CanEnterActiveMode()) {
    EmitEvent(InputRouterEventKind::kRoutingSuppressed, reason, snapshot_, false);
    return false;
  }

  const InputRouterSnapshot previous = snapshot_;
  snapshot_.capture_state = InputCaptureState::kPointerCaptured;
  EmitEvent(InputRouterEventKind::kCaptureChanged, reason, previous, false);
  return true;
}

bool InputRouter::ReleasePointerCapture(InputTransitionReason reason) {
  if (snapshot_.capture_state == InputCaptureState::kNone) {
    return false;
  }

  const InputRouterSnapshot previous = snapshot_;
  snapshot_.capture_state = InputCaptureState::kNone;
  if (snapshot_.interaction_active) {
    snapshot_.interaction_active = false;
  }

  EmitEvent(InputRouterEventKind::kCaptureChanged, reason, previous,
            previous.interaction_active);
  if (previous.interaction_active != snapshot_.interaction_active) {
    EmitEvent(InputRouterEventKind::kInteractionChanged, reason, previous, true);
  }
  return true;
}

bool InputRouter::BeginInteraction(InputTransitionReason reason) {
  if (snapshot_.interaction_active) {
    return false;
  }
  if (snapshot_.mode != InputMode::kActive || !CanEnterActiveMode()) {
    EmitEvent(InputRouterEventKind::kRoutingSuppressed, reason, snapshot_, false);
    return false;
  }

  const InputRouterSnapshot previous = snapshot_;
  snapshot_.interaction_active = true;
  snapshot_.interaction_epoch += 1U;
  EmitEvent(InputRouterEventKind::kInteractionChanged, reason, previous, false);
  return true;
}

bool InputRouter::EndInteraction(InputTransitionReason reason) {
  if (!snapshot_.interaction_active) {
    return false;
  }

  const InputRouterSnapshot previous = snapshot_;
  snapshot_.interaction_active = false;
  EmitEvent(InputRouterEventKind::kInteractionChanged, reason, previous, false);
  return true;
}

void InputRouter::CancelInteractionForSystem(InputTransitionReason reason) {
  bool emitted = false;
  InputRouterSnapshot previous = snapshot_;
  if (snapshot_.capture_state != InputCaptureState::kNone) {
    snapshot_.capture_state = InputCaptureState::kNone;
    EmitEvent(InputRouterEventKind::kCaptureChanged, reason, previous, true);
    previous = snapshot_;
    emitted = true;
  }

  if (snapshot_.interaction_active) {
    snapshot_.interaction_active = false;
    EmitEvent(InputRouterEventKind::kInteractionChanged, reason, previous, true);
    emitted = true;
  }

  if (!emitted && config_.event_callback) {
    EmitEvent(InputRouterEventKind::kRoutingSuppressed, reason, snapshot_, true);
  }
}

bool InputRouter::CanRoutePointerInput() const {
  if (snapshot_.mode != InputMode::kActive) {
    return false;
  }
  if (snapshot_.capture_state == InputCaptureState::kPointerCaptured) {
    return true;
  }
  return CanEnterActiveMode();
}

bool InputRouter::CanRouteKeyboardInput() const {
  if (snapshot_.mode != InputMode::kActive) {
    return false;
  }
  return CanEnterActiveMode();
}

bool InputRouter::ShouldHoldUserLease() const {
  if (!snapshot_.interaction_active || snapshot_.mode != InputMode::kActive) {
    return false;
  }
  return CanEnterActiveMode() ||
         snapshot_.capture_state == InputCaptureState::kPointerCaptured;
}

bool InputRouter::CanEnterActiveMode() const {
  return config_.allow_active_without_focus ||
         snapshot_.focus_state == InputFocusState::kFocused;
}

void InputRouter::EmitEvent(InputRouterEventKind kind,
                            InputTransitionReason reason,
                            const InputRouterSnapshot& previous,
                            bool should_flush_transient_input) {
  if (!config_.event_callback) {
    return;
  }
  InputRouterEvent event;
  event.kind = kind;
  event.reason = reason;
  event.previous = previous;
  event.current = snapshot_;
  event.should_flush_transient_input = should_flush_transient_input;
  config_.event_callback(event);
}

}  // namespace ntium::input
