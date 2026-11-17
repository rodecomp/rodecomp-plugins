// CreateApplicationWindow (the main window's only creation point, called once from InitApp) gets icon 123 from this
// DLL's resources (icon.rc): the exe has no resource section, so InitApp's LoadIconA(123) finds nothing.
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"

namespace {
  constexpr const char* kName = "WARP_RestoreIcon";

  decltype(&CreateApplicationWindow) g_original;

  HICON LoadOwnIcon(int sizeMetric) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LoadOwnIcon), &self);
    const int size = GetSystemMetrics(sizeMetric);
    return static_cast<HICON>(LoadImageA(self, MAKEINTRESOURCEA(123), IMAGE_ICON, size, size, 0));
  }

  HWND __stdcall CreateWindowWithIcon(DWORD extendedStyle, const char* className, const char* title, DWORD style, int x,
                                      int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance,
                                      void* parameter) {
    HWND window = g_original(extendedStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
    if (window) {
      if (HICON big = LoadOwnIcon(SM_CXICON)) {
        SetClassLongPtrA(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(big));
        SendMessageA(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
      }
      if (HICON small = LoadOwnIcon(SM_CXSMICON)) {
        SetClassLongPtrA(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small));
        SendMessageA(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
      }
    }
    return window;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CreateApplicationWindow, &CreateWindowWithIcon, g_original, kName);
}
