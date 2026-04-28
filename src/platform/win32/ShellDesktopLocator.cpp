#include "ShellDesktopLocator.h"

#ifdef _WIN32
#include <windows.h>

#include <cstring>
#endif

namespace ntium::platform::win32 {
namespace {

#ifdef _WIN32
struct DesktopWindowScanState {
  HWND def_view = nullptr;
  HWND workerw = nullptr;
};

BOOL CALLBACK ScanTopLevelShellWindows(HWND hwnd, LPARAM user_data) {
  auto* state = reinterpret_cast<DesktopWindowScanState*>(user_data);
  if (state == nullptr) {
    return TRUE;
  }

  char class_name[64] = {};
  const int class_name_length = GetClassNameA(hwnd, class_name, static_cast<int>(sizeof(class_name)));
  if (class_name_length > 0 && std::strcmp(class_name, "WorkerW") == 0 &&
      state->workerw == nullptr) {
    state->workerw = hwnd;
  }

  HWND def_view = FindWindowExA(hwnd, nullptr, "SHELLDLL_DefView", nullptr);
  if (def_view != nullptr) {
    state->def_view = def_view;
    return FALSE;
  }

  return TRUE;
}
#endif

}  // namespace

DesktopAttachAnchor ShellDesktopLocator::LocateDesktopAnchor() const {
  DesktopAttachAnchor anchor = {};

#ifdef _WIN32
  DesktopWindowScanState state = {};

  // Step 1: Probe under Progman first (common baseline shell hierarchy).
  const HWND progman = FindWindowA("Progman", nullptr);
  if (progman != nullptr) {
    state.def_view = FindWindowExA(progman, nullptr, "SHELLDLL_DefView", nullptr);
  }

  // Step 2: Fall back to top-level shell window scan.
  if (state.def_view == nullptr) {
    EnumWindows(ScanTopLevelShellWindows, reinterpret_cast<LPARAM>(&state));
  } else {
    // Even if DefView is already found, still capture WorkerW for diagnostics.
    EnumWindows(ScanTopLevelShellWindows, reinterpret_cast<LPARAM>(&state));
  }

  anchor.workerw_observed = state.workerw != nullptr;
  anchor.workerw_window = reinterpret_cast<std::uintptr_t>(state.workerw);

  if (state.def_view != nullptr) {
    const HWND def_view_parent = GetParent(state.def_view);
    anchor.def_view_window = reinterpret_cast<std::uintptr_t>(state.def_view);
    anchor.attach_parent_window = reinterpret_cast<std::uintptr_t>(def_view_parent);
    // DefView + parent is the required attach anchor. WorkerW is explicitly not
    // part of control flow and is surfaced only for telemetry/diagnostics.
    anchor.found = def_view_parent != nullptr;
  }
#else
  // Non-Windows builds intentionally return an empty anchor.
  anchor = {};
#endif

  return anchor;
}

const char* ShellDesktopLocator::StrategySummary() {
  return "Locate SHELLDLL_DefView, attach relative to its actual parent, and "
         "treat WorkerW as optional diagnostics only.";
}

}  // namespace ntium::platform::win32
