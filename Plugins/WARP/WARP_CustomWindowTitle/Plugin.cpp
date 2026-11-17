// Hooks CreateApplicationWindow (InitApp's only window creation): it always passes "Ragnarok 2026" to CreateWindowExA,
// so the detour lets it create the window and then sets the Title setting (plugins\WARP_CustomWindowTitle.toml).
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_CustomWindowTitle";

  decltype(&CreateApplicationWindow) g_original;
  const char* g_title;  // host-owned, valid for the life of the process

  HWND __stdcall CreateApplicationWindowHook(
    DWORD extendedStyle, const char* className, const char* ignoredTitle, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, void* parameter
  ) {
    HWND window = g_original(extendedStyle, className, ignoredTitle, style, x, y, width, height, parent, menu, instance, parameter);
    if (window && g_title[0]) {
      SetWindowTextA(window, g_title);
    }
    return window;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_title = host->SettingString(kName, "Title");
  host->Log("%s: title \"%s\"", kName, g_title);
  return ro::Hook(host, &CreateApplicationWindow, &CreateApplicationWindowHook, g_original, kName);
}
