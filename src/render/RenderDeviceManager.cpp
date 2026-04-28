#include "src/render/RenderDeviceManager.h"

#include <unordered_map>
#include <utility>

namespace render {

namespace {

class RenderDeviceManager final : public IRenderDeviceManager {
 public:
  RenderStatus EnsureDeviceForAdapter(const AdapterIdentity& adapter) override {
    auto [it, inserted] =
        states_.try_emplace(adapter, AdapterDeviceState{adapter, {}, false, 1});
    if (!inserted && it->second.device_lost) {
      return RenderStatus::kDeviceLost;
    }

#ifndef _WIN32
    (void)adapter;
    return RenderStatus::kUnsupportedPlatform;
#else
    // Ticket R1-201 scaffold: D3D11/DXGI binding is intentionally deferred.
    return RenderStatus::kOk;
#endif
  }

  RenderStatus GetDeviceState(
      const AdapterIdentity& adapter,
      AdapterDeviceState* out_state) const override {
    if (out_state == nullptr) {
      return RenderStatus::kInvalidArgument;
    }

    const auto it = states_.find(adapter);
    if (it == states_.end()) {
      return RenderStatus::kNotFound;
    }
    *out_state = it->second;
    return RenderStatus::kOk;
  }

  RenderStatus MarkDeviceLost(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) override {
    (void)event_sequence;
    const auto it = states_.find(adapter);
    if (it == states_.end()) {
      return RenderStatus::kNotFound;
    }
    it->second.device_lost = true;
    return RenderStatus::kOk;
  }

  RenderStatus RecoverDevice(
      const AdapterIdentity& adapter,
      uint64_t event_sequence) override {
    (void)event_sequence;
    const auto it = states_.find(adapter);
    if (it == states_.end()) {
      return RenderStatus::kNotFound;
    }

#ifndef _WIN32
    return RenderStatus::kUnsupportedPlatform;
#else
    it->second.device_lost = false;
    ++it->second.generation;
    if (device_recovered_callback_) {
      device_recovered_callback_(adapter, it->second.generation);
    }
    return RenderStatus::kOk;
#endif
  }

  RenderStatus RemoveAdapter(const AdapterIdentity& adapter) override {
    const auto erased = states_.erase(adapter);
    return erased == 0 ? RenderStatus::kNotFound : RenderStatus::kOk;
  }

  std::vector<AdapterIdentity> ListAdapters() const override {
    std::vector<AdapterIdentity> adapters;
    adapters.reserve(states_.size());
    for (const auto& [adapter, _] : states_) {
      (void)_;
      adapters.push_back(adapter);
    }
    return adapters;
  }

  void SetDeviceRecoveredCallback(DeviceRecoveredCallback callback) override {
    device_recovered_callback_ = std::move(callback);
  }

 private:
  std::unordered_map<AdapterIdentity, AdapterDeviceState, AdapterIdentityHash>
      states_;
  DeviceRecoveredCallback device_recovered_callback_;
};

}  // namespace

std::unique_ptr<IRenderDeviceManager> CreateRenderDeviceManager() {
  return std::make_unique<RenderDeviceManager>();
}

}  // namespace render
