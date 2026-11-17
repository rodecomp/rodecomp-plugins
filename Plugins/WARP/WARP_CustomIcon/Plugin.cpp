// CreateApplicationWindow (the main window's only creation point, called once from InitApp) gets an icon loaded from
// a .ico file: IconPath in plugins\WARP_CustomIcon.ini, else plugins\WARP_CustomIcon.ico next to this DLL.
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"

#include <cstring>

namespace {
  constexpr const char* kName = "WARP_CustomIcon";

  const RoHostApi* g_host;
  decltype(&CreateApplicationWindow) g_original;

  // plugins\WARP_CustomIcon.<extension>, next to this DLL.
  void SiblingPath(char (&path)[MAX_PATH], const char* extension) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&SiblingPath), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* dot = std::strrchr(path, '.');
    if (dot && static_cast<size_t>(dot - path) + std::strlen(extension) < MAX_PATH) {
      std::strcpy(dot, extension);
    }
  }

  HWND __stdcall CreateWindowWithIcon(DWORD extendedStyle, const char* className, const char* title, DWORD style, int x,
                                      int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance,
                                      void* parameter) {
    HWND window = g_original(extendedStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
    if (!window) {
      return window;
    }
    char ini[MAX_PATH];
    char icon[MAX_PATH];
    SiblingPath(ini, ".ini");
    SiblingPath(icon, ".ico");
    GetPrivateProfileStringA(kName, "IconPath", icon, icon, MAX_PATH, ini);

    const int bigSize = GetSystemMetrics(SM_CXICON);
    const int smallSize = GetSystemMetrics(SM_CXSMICON);
    auto big = static_cast<HICON>(LoadImageA(nullptr, icon, IMAGE_ICON, bigSize, bigSize, LR_LOADFROMFILE));
    auto small = static_cast<HICON>(LoadImageA(nullptr, icon, IMAGE_ICON, smallSize, smallSize, LR_LOADFROMFILE));
    if (!big && !small) {
      g_host->Log("%s: cannot load icon %s", kName, icon);
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
  return ro::Hook(host, &CreateApplicationWindow, &CreateWindowWithIcon, g_original, kName);
}
