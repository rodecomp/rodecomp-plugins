// Hooks CreateApplicationWindow (InitApp's only window creation): it always passes "Ragnarok 2026" to CreateWindowExA,
// so the detour lets it create the window and then sets Title from plugins\WARP_CustomWindowTitle.ini.
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_CustomWindowTitle";

  decltype(&CreateApplicationWindow) g_original;

  void ReadTitle(char* title, DWORD size) {
    char path[MAX_PATH];
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&ReadTitle), &module);
    DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    char* slash = length && length < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (!slash || slash - path + 1 + strlen(kName) + 4 >= MAX_PATH) {
      lstrcpynA(title, "RoDecomp", size);
      return;
    }
    strcpy(slash + 1, kName);
    strcat(slash + 1, ".ini");
    GetPrivateProfileStringA(kName, "Title", "RoDecomp", title, size, path);
  }

  HWND __stdcall CreateApplicationWindowHook(
    DWORD extendedStyle, const char* className, const char* ignoredTitle, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, void* parameter
  ) {
    HWND window = g_original(extendedStyle, className, ignoredTitle, style, x, y, width, height, parent, menu, instance, parameter);
    if (window) {
      char title[256];
      ReadTitle(title, sizeof(title));
      if (title[0]) {
        SetWindowTextA(window, title);
      }
    }
    return window;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CreateApplicationWindow, &CreateApplicationWindowHook, g_original, kName);
}
