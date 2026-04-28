#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "src/render/RenderTypes.h"

namespace render {

// Device manager contract:
// - Owns one logical device state per AdapterIdentity.
// - Mutations are expected to be driven by explicit runtime events
//   (topology change, device lost, recovery request).
// - Callers should not poll this API in a free-running loop.
class IRenderDeviceManager {
 public:
  using DeviceRecoveredCallback =
      std::function<void(const AdapterIdentity& adapter, uint64_t generation)>;

  virtual ~IRenderDeviceManager() = default;

  // Ensures adapter state exists and native device handles are ready.
  // Windows implementations should bind this to D3D11/DXGI creation logic.
  virtual RenderStatus EnsureDeviceForAdapter(
      const AdapterIdentity& adapter) = 0;

  virtual RenderStatus GetDeviceState(
      const AdapterIdentity& adapter,
      AdapterDeviceState* out_state) const = 0;

  // Marks a specific adapter as device-lost. This is expected to be called
  // from explicit runtime/device events and not from speculative polling.
  virtual RenderStatus MarkDeviceLost(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) = 0;

  // Performs adapter-scoped recovery. A successful recovery increments
  // generation and invokes the recovery callback.
  virtual RenderStatus RecoverDevice(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) = 0;

  virtual RenderStatus RemoveAdapter(const AdapterIdentity& adapter) = 0;
  virtual std::vector<AdapterIdentity> ListAdapters() const = 0;

  virtual void SetDeviceRecoveredCallback(DeviceRecoveredCallback callback) = 0;
};

std::unique_ptr<IRenderDeviceManager> CreateRenderDeviceManager();

}  // namespace render
