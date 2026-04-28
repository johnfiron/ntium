#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace render {

// Shared status codes for render foundation manager operations.
enum class RenderStatus : uint8_t {
  kOk = 0,
  kNotFound,
  kInvalidArgument,
  kUnsupportedPlatform,
  kDeviceLost,
  kInternalError
};

// Stable adapter identity used as the key for per-adapter resources.
// On Windows this maps to LUID semantics. adapter_key is an optional
// diagnostics identifier for logs and tests.
struct AdapterIdentity {
  uint32_t luid_low_part = 0;
  int32_t luid_high_part = 0;
  std::string adapter_key;

  bool operator==(const AdapterIdentity& other) const noexcept {
    return luid_low_part == other.luid_low_part &&
           luid_high_part == other.luid_high_part &&
           adapter_key == other.adapter_key;
  }
};

struct AdapterIdentityHash {
  std::size_t operator()(const AdapterIdentity& value) const noexcept {
    const std::size_t h1 = std::hash<uint32_t>{}(value.luid_low_part);
    const std::size_t h2 =
        std::hash<int32_t>{}(value.luid_high_part) + 0x9e3779b9ULL +
        (h1 << 6U) + (h1 >> 2U);
    const std::size_t h3 = std::hash<std::string>{}(value.adapter_key);
    return h2 ^ (h3 + 0x9e3779b9ULL + (h2 << 6U) + (h2 >> 2U));
  }
};

// Stable monitor identity (adapter + output target id).
struct MonitorIdentity {
  AdapterIdentity adapter;
  uint32_t target_id = 0;
  std::string monitor_key;

  bool operator==(const MonitorIdentity& other) const noexcept {
    return adapter == other.adapter &&
           target_id == other.target_id &&
           monitor_key == other.monitor_key;
  }
};

struct MonitorIdentityHash {
  std::size_t operator()(const MonitorIdentity& value) const noexcept {
    const std::size_t h1 = AdapterIdentityHash{}(value.adapter);
    const std::size_t h2 = std::hash<uint32_t>{}(value.target_id);
    const std::size_t h3 = std::hash<std::string>{}(value.monitor_key);
    return h1 ^ (h2 + 0x9e3779b9ULL + (h1 << 6U) + (h1 >> 2U)) ^
           (h3 + 0x9e3779b9ULL + (h2 << 6U) + (h2 >> 2U));
  }
};

struct DirtyRectPx {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
};

enum class PresentReason : uint8_t {
  kInvalidation = 0,
  kTopologyChanged,
  kDeviceRecovery
};

#ifdef _WIN32
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIAdapter1;
struct IDXGISwapChain1;
#endif

struct NativeDeviceHandles {
#ifdef _WIN32
  ID3D11Device* d3d11_device = nullptr;
  ID3D11DeviceContext* immediate_context = nullptr;
  IDXGIAdapter1* dxgi_adapter = nullptr;
#else
  void* d3d11_device = nullptr;
  void* immediate_context = nullptr;
  void* dxgi_adapter = nullptr;
#endif
};

struct AdapterDeviceState {
  AdapterIdentity adapter;
  NativeDeviceHandles native;
  bool device_lost = false;
  uint64_t generation = 0;
};

struct SwapchainDescriptor {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t buffer_count = 2;
  bool allow_tearing = false;
};

struct NativeSwapchainHandles {
#ifdef _WIN32
  IDXGISwapChain1* swapchain = nullptr;
#else
  void* swapchain = nullptr;
#endif
};

struct SwapchainState {
  MonitorIdentity monitor;
  SwapchainDescriptor descriptor;
  NativeSwapchainHandles native;
  bool is_occluded = false;
  bool requires_recreate = false;
  uint64_t generation = 0;
};

// Event-driven present request. Render core should call Present only when
// an invalidation/runtime event is consumed. A free-running render loop is
// intentionally outside this contract.
struct PresentIntent {
  MonitorIdentity monitor;
  uint64_t event_sequence = 0;
  bool force_full_frame = false;
  bool bypass_overlay_throttle = false;
  PresentReason reason = PresentReason::kInvalidation;
  std::vector<DirtyRectPx> dirty_rects;
};

struct PresentResult {
  RenderStatus status = RenderStatus::kOk;
  bool presented = false;
  bool became_occluded = false;
  bool requires_recreate = false;
};

}  // namespace render
