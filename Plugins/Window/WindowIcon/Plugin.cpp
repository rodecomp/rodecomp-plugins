// InitApp creates the main window with a direct CreateWindowExA call (no exe function wraps it), so user32's
// CreateWindowExA is hooked for both settings and only the call with the game's window class (g_windowName, the
// pointer InitApp registers) is changed; after the original creates the window, the icon is set on its class and
// title bar/taskbar entry:
// - Icon empty: icon 123 from this DLL's resources (icon.rc), since the exe has no resource section, so InitApp's
//   LoadIconA(123) finds nothing (WARP RestoreIcon).
// - Icon a path: the icon loaded from that .ico file (WARP CustomIcon); if it cannot be loaded, the window keeps the
//   game's own icon and the failure is logged.
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"
#include "Contents/Configuration/ServiceConfiguration.h"

namespace {
  constexpr const char* kName = "WindowIcon";

  const RoHostApi* g_host;
  decltype(&::CreateWindowExA) g_original;
  const char* g_icon;  // absolute path or empty, host-owned, valid for the life of the process

  HICON LoadWindowIcon(int sizeMetric) {
    const int size = GetSystemMetrics(sizeMetric);
    if (*g_icon) {
      return static_cast<HICON>(LoadImageA(nullptr, g_icon, IMAGE_ICON, size, size, LR_LOADFROMFILE));
    }
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&LoadWindowIcon), &self);
    return static_cast<HICON>(LoadImageA(self, MAKEINTRESOURCEA(123), IMAGE_ICON, size, size, 0));
  }

  HWND WINAPI CreateWindowWithIcon(DWORD extendedStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y,
                                   int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
    HWND window = g_original(extendedStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
    if (!window || className != g_windowName) {
      return window;
    }
    HICON big = LoadWindowIcon(SM_CXICON);
    HICON small = LoadWindowIcon(SM_CXSMICON);
    if (!big && !small) {
      g_host->Log("%s: cannot load icon %s", kName, *g_icon ? g_icon : "123 (built-in)");
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
  host->Log("%s: icon %s", kName, *g_icon ? g_icon : "built-in");
  return ro::HookSystem(host, "user32.dll", "CreateWindowExA", &CreateWindowWithIcon, g_original, kName);
}
