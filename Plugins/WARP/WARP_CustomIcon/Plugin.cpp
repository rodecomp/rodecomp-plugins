// CreateApplicationWindow (the main window's only creation point, called once from InitApp) gets an icon loaded from
// the .ico file of the Icon setting (plugins\WARP_CustomIcon.toml, default plugins\WARP_CustomIcon.ico).
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"

namespace {
  constexpr const char* kName = "WARP_CustomIcon";

  const RoHostApi* g_host;
  decltype(&CreateApplicationWindow) g_original;
  const char* g_icon;  // absolute path, host-owned, valid for the life of the process

  HWND __stdcall CreateWindowWithIcon(DWORD extendedStyle, const char* className, const char* title, DWORD style, int x,
                                      int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance,
                                      void* parameter) {
    HWND window = g_original(extendedStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
    if (!window) {
      return window;
    }
    const int bigSize = GetSystemMetrics(SM_CXICON);
    const int smallSize = GetSystemMetrics(SM_CXSMICON);
    auto big = static_cast<HICON>(LoadImageA(nullptr, g_icon, IMAGE_ICON, bigSize, bigSize, LR_LOADFROMFILE));
    auto small = static_cast<HICON>(LoadImageA(nullptr, g_icon, IMAGE_ICON, smallSize, smallSize, LR_LOADFROMFILE));
    if (!big && !small) {
      g_host->Log("%s: cannot load icon %s", kName, g_icon);
      return window;
    }
    if (big) {
      SetClassLongPtrA(window, GCLP_HICON, reinterpret_cast<LONG_PTR>(big));
      SendMessageA(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    }
    if (small) {
      SetClassLongPtrA(window, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small));
      SendMessageA(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
    }
    return window;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_host = host;
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_icon = host->SettingString(kName, "Icon");
  host->Log("%s: icon %s", kName, g_icon);
  return ro::Hook(host, &CreateApplicationWindow, &CreateWindowWithIcon, g_original, kName);
}
