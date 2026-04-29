#include "src/app/AppHost.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>

namespace {

constexpr wchar_t kHostWindowClassName[] = L"NtiumWallpaperHostWindowClass";
constexpr wchar_t kHostWindowTitle[] = L"Ntium Wallpaper Host";

ntium::app::AppHost* g_app_host = nullptr;

void EmitDebugString(const std::string& text) {
  if (text.empty()) {
    return;
  }
  OutputDebugStringA(text.c_str());
}

LRESULT CALLBACK HostWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
  if (g_app_host != nullptr) {
    std::intptr_t handled_result = 0;
    if (g_app_host->OnWindowMessage(
            static_cast<std::uint32_t>(message),
            static_cast<std::uintptr_t>(wparam),
            static_cast<std::intptr_t>(lparam),
            &handled_result)) {
      return static_cast<LRESULT>(handled_result);
    }
  }

  switch (message) {
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }

  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace

int APIENTRY wWinMain(
    HINSTANCE instance,
    HINSTANCE previous_instance,
    PWSTR command_line,
    int show_command) {
  (void)previous_instance;
  (void)command_line;
  (void)show_command;

  ntium::app::AppHost app_host;
  const ntium::app::AppHostResult init_result = app_host.Initialize();
  if (!init_result.ok()) {
    EmitDebugString("AppHost initialize failed at stage: " + init_result.stage +
                    " detail: " + init_result.detail + "\n");
    return static_cast<int>(init_result.code);
  }

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = HostWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kHostWindowClassName;

  const ATOM class_atom = RegisterClassExW(&window_class);
  if (class_atom == 0) {
    (void)app_host.Shutdown();
    EmitDebugString("RegisterClassExW failed\n");
    return 2;
  }

  HWND window_handle = CreateWindowExW(
      0,
      kHostWindowClassName,
      kHostWindowTitle,
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      320,
      240,
      nullptr,
      nullptr,
      instance,
      nullptr);
  if (window_handle == nullptr) {
    (void)app_host.Shutdown();
    UnregisterClassW(kHostWindowClassName, instance);
    EmitDebugString("CreateWindowExW failed\n");
    return 3;
  }

  g_app_host = &app_host;

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  g_app_host = nullptr;
  if (IsWindow(window_handle) != FALSE) {
    DestroyWindow(window_handle);
  }
  const ntium::app::AppHostResult shutdown_result = app_host.Shutdown();
  UnregisterClassW(kHostWindowClassName, instance);
  if (!shutdown_result.ok()) {
    EmitDebugString("AppHost shutdown failed at stage: " + shutdown_result.stage +
                    " detail: " + shutdown_result.detail + "\n");
    return static_cast<int>(shutdown_result.code);
  }

  return static_cast<int>(message.wParam);
}

#else

int main() { return 0; }

#endif
