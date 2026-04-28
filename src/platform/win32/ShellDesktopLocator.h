#pragma once

#include <cstdint>

namespace ntium::platform::win32 {

struct DesktopAttachAnchor {
  bool found = false;
  std::uintptr_t def_view_window = 0;
  std::uintptr_t attach_parent_window = 0;
  bool workerw_observed = false;
  std::uintptr_t workerw_window = 0;
};

class ShellDesktopLocator {
 public:
  // DefView-relative strategy:
  // 1) Locate SHELLDLL_DefView in the Explorer shell window tree.
  // 2) Use DefView's actual parent as the attach anchor.
  // WorkerW can be observed for diagnostics, but is never required for control
  // flow so shells that do not expose WorkerW still work.
  DesktopAttachAnchor LocateDesktopAnchor() const;

  static const char* StrategySummary();
};

}  // namespace ntium::platform::win32
