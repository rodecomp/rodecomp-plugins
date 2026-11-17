// Hooks CreateApplicationWindow (InitApp's only window creation) to add WS_EX_TOOLWINDOW to the extended style.
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"

namespace {
  constexpr const char* kName = "WARP_UseMiniTitle";

  decltype(&CreateApplicationWindow) g_original;

  HWND __stdcall CreateApplicationWindowHook(
    DWORD extendedStyle, const char* className, const char* ignoredTitle, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, void* parameter
  ) {
    return g_original(extendedStyle | WS_EX_TOOLWINDOW, className, ignoredTitle, style, x, y, width, height, parent, menu, instance, parameter);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CreateApplicationWindow, &CreateApplicationWindowHook, g_original, kName);
}
