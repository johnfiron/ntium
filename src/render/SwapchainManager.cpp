#include "src/render/SwapchainManager.h"

#include <unordered_map>
#include <utility>

namespace render {

namespace {

class SwapchainManager final : public ISwapchainManager {
 public:
  RenderStatus EnsureSwapchainForMonitor(
      const MonitorIdentity& monitor,
      const SwapchainDescriptor& descriptor,
      uint64_t event_sequence) override {
    (void)event_sequence;
    if (descriptor.width == 0 || descriptor.height == 0) {
      return RenderStatus::kInvalidArgument;
    }

    auto [it, inserted] = states_.try_emplace(
        monitor,
        SwapchainState{monitor, descriptor, {}, false, 1});
    if (!inserted) {
      it->second.descriptor = descriptor;
      ++it->second.generation;
    }

#ifndef _WIN32
    return RenderStatus::kUnsupportedPlatform;
#else
    // Ticket R1-202 scaffold: DXGI swapchain creation is deferred.
    return RenderStatus::kOk;
#endif
  }

  RenderStatus ResizeSwapchain(
      const MonitorIdentity& monitor,
      const SwapchainDescriptor& descriptor,
      uint64_t event_sequence) override {
    (void)event_sequence;
    if (descriptor.width == 0 || descriptor.height == 0) {
      return RenderStatus::kInvalidArgument;
    }

    const auto it = states_.find(monitor);
    if (it == states_.end()) {
      return RenderStatus::kNotFound;
    }
    it->second.descriptor = descriptor;
    ++it->second.generation;

#ifndef _WIN32
    return RenderStatus::kUnsupportedPlatform;
#else
    return RenderStatus::kOk;
#endif
  }

  RenderStatus GetSwapchainState(
      const MonitorIdentity& monitor,
      SwapchainState* out_state) const override {
    if (out_state == nullptr) {
      return RenderStatus::kInvalidArgument;
    }
    const auto it = states_.find(monitor);
    if (it == states_.end()) {
      return RenderStatus::kNotFound;
    }
    *out_state = it->second;
    return RenderStatus::kOk;
  }

  std::vector<MonitorIdentity> ListMonitors() const override {
    std::vector<MonitorIdentity> monitors;
    monitors.reserve(states_.size());
    for (const auto& [monitor, _] : states_) {
      (void)_;
      monitors.push_back(monitor);
    }
    return monitors;
  }

  RenderStatus Present(
      const PresentIntent& intent,
      PresentResult* out_result) override {
    if (out_result == nullptr) {
      return RenderStatus::kInvalidArgument;
    }
    PresentResult result{};
    const auto it = states_.find(intent.monitor);
    if (it == states_.end()) {
      result.status = RenderStatus::kNotFound;
      *out_result = result;
      return result.status;
    }

    if (it->second.is_occluded && intent.reason == PresentReason::kInvalidation) {
      // Event-driven contract: when occluded, hold invalidation-driven presents
      // until an explicit recovery/visibility event updates state.
      result.status = RenderStatus::kOk;
      result.presented = false;
      result.became_occluded = true;
      *out_result = result;
      return result.status;
    }

#ifndef _WIN32
    result.status = RenderStatus::kUnsupportedPlatform;
    result.presented = false;
    *out_result = result;
    return result.status;
#else
    // Ticket R1-202 scaffold: Present implementation is intentionally deferred.
    result.status = RenderStatus::kOk;
    result.presented = true;
    *out_result = result;
    return result.status;
#endif
  }

  RenderStatus MarkOccluded(
      const MonitorIdentity& monitor,
      bool is_occluded,
      uint64_t event_sequence) override {
    (void)event_sequence;
    const auto it = states_.find(monitor);
    if (it == states_.end()) {
      return RenderStatus::kNotFound;
    }
    it->second.is_occluded = is_occluded;
    return RenderStatus::kOk;
  }

  RenderStatus RemoveMonitor(const MonitorIdentity& monitor) override {
    const auto erased = states_.erase(monitor);
    return erased == 0 ? RenderStatus::kNotFound : RenderStatus::kOk;
  }

  void SetSwapchainRecreatedCallback(
      SwapchainRecreatedCallback callback) override {
    swapchain_recreated_callback_ = std::move(callback);
  }

 private:
  std::unordered_map<MonitorIdentity, SwapchainState, MonitorIdentityHash> states_;
  SwapchainRecreatedCallback swapchain_recreated_callback_;
};

}  // namespace

std::unique_ptr<ISwapchainManager> CreateSwapchainManager() {
  return std::make_unique<SwapchainManager>();
}

}  // namespace render
