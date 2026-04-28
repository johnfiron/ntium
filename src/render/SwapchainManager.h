#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "src/render/RenderTypes.h"

namespace render {

// Swapchain manager contract:
// - Owns one swapchain state per MonitorIdentity.
// - Lifecycle operations are event-sequenced and expected to be triggered by
//   explicit invalidation/runtime events (topology changes, resize, recovery).
// - Callers should not free-run Present in a polling loop.
class ISwapchainManager {
 public:
  using SwapchainRecreatedCallback =
      std::function<void(const MonitorIdentity& monitor, uint64_t generation)>;

  virtual ~ISwapchainManager() = default;

  virtual RenderStatus EnsureSwapchainForMonitor(
      const MonitorIdentity& monitor,
      const SwapchainDescriptor& descriptor,
      uint64_t event_sequence) = 0;

  virtual RenderStatus ResizeSwapchain(
      const MonitorIdentity& monitor,
      const SwapchainDescriptor& descriptor,
      uint64_t event_sequence) = 0;

  virtual RenderStatus GetSwapchainState(
      const MonitorIdentity& monitor,
      SwapchainState* out_state) const = 0;

  // Presents are strictly request-driven from event consumption. Implementors
  // should no-op when there is no effective dirty work.
  virtual RenderStatus Present(
      const PresentIntent& intent,
      PresentResult* out_result) = 0;

  virtual RenderStatus MarkOccluded(
      const MonitorIdentity& monitor,
      bool is_occluded,
      uint64_t event_sequence) = 0;

  virtual RenderStatus RemoveMonitor(const MonitorIdentity& monitor) = 0;
  virtual std::vector<MonitorIdentity> ListMonitors() const = 0;

  virtual void SetSwapchainRecreatedCallback(
      SwapchainRecreatedCallback callback) = 0;
};

std::unique_ptr<ISwapchainManager> CreateSwapchainManager();

}  // namespace render
