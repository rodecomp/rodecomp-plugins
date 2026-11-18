// Hooks InitApp's only window creation once for all four settings. InitApp calls CreateWindowExA directly (no exe
// function wraps it), so user32's CreateWindowExA is hooked and only the call with the game's window class
// (g_windowName, the pointer InitApp registers) is changed:
// - MiniTitle: adds WS_EX_TOOLWINDOW to the extended style.
// - Borderless: when the window InitApp asks for (outer size) is at least the screen size, it becomes a WS_POPUP |
//   WS_VISIBLE window at 0,0 exactly the size of the screen, as WARP's BorderlessFSW code before CreateWindowExA did.
//   SysMenu then leaves it alone.
// - SysMenu: gives the windowed client WS_OVERLAPPEDWINDOW (InitApp's style is inline, so the outer size InitApp computed
//   with AdjustWindowRect is grown by the new frame's extra border). WindowProc (hooked only for SysMenu) handles
//   WM_GETMINMAXINFO (keep the default max track size) and the SC_MINIMIZE/SC_MOUSEMENU/SC_RESTORE/SC_MONITORPOWER system
//   commands (handed to DefWindowProcA instead of swallowed), both under the original's IsWindowed() condition.
// - Title: InitApp passes g_windowName as the title, so after creation a non-empty Title replaces it.
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Render/DeviceContext/DCSurface.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "WindowFrame";

  constexpr DWORD kSysMenuStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

  const char* g_title;  // host-owned, valid for the life of the process; "" keeps the game's title
  bool g_miniTitle;
  bool g_sysMenu;
  bool g_borderless;

  decltype(&::CreateWindowExA) g_originalCreate;
  decltype(&WindowProc) g_originalWindowProc;
  // The exe's GetRenderSystemHolder: the header's inline copy would have its own empty static in this DLL.
  RenderSystemHolder* (*g_getRenderSystemHolder)();

  bool IsWindowed() {
    RenderSystemHolder* holder = g_getRenderSystemHolder();
    return holder && holder->IsWindowed();
  }

  HWND WINAPI CreateWindowExHook(
    DWORD extendedStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, LPVOID parameter
  ) {
    if (className != g_windowName) {
      return g_originalCreate(extendedStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
    }
    if (g_miniTitle) {
      extendedStyle |= WS_EX_TOOLWINDOW;
    }
    bool borderless = false;
    if (g_borderless) {
      const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
      const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
      if (width >= screenWidth && height >= screenHeight) {
        style = WS_POPUP | WS_VISIBLE;
        x = 0;
        y = 0;
        width = screenWidth;
        height = screenHeight;
        borderless = true;
      }
    }
    if (g_sysMenu && !borderless && style != WS_POPUP) {
      RECT before{0, 0, 0, 0};
      RECT after{0, 0, 0, 0};
      AdjustWindowRect(&before, style, FALSE);
      AdjustWindowRect(&after, kSysMenuStyle, FALSE);
      width += (after.right - after.left) - (before.right - before.left);
      height += (after.bottom - after.top) - (before.bottom - before.top);
      style = kSysMenuStyle;
    }
    HWND window = g_originalCreate(extendedStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
    if (window && g_title[0]) {
      SetWindowTextA(window, g_title);
    }
    return window;
  }

  LRESULT CALLBACK WindowProcHook(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_GETMINMAXINFO) {
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
      const POINT maxTrackSize = info->ptMaxTrackSize;
      LRESULT result = g_originalWindowProc(window, message, wParam, lParam);
      if (hWnd && IsWindowed()) {
        info->ptMaxTrackSize = maxTrackSize;
      }
      return result;
    }
    if (message == WM_SYSCOMMAND) {
      switch (wParam & 0xFFF0) {
      case SC_MINIMIZE:
      case SC_MOUSEMENU:
      case SC_RESTORE:
      case SC_MONITORPOWER:
        if (IsWindowed()) {
          return DefWindowProcA(window, message, wParam, lParam);
        }
        break;
      }
    }
    return g_originalWindowProc(window, message, wParam, lParam);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_title = host->SettingString(kName, "Title");
  g_miniTitle = host->SettingBool(kName, "MiniTitle");
  g_sysMenu = host->SettingBool(kName, "SysMenu");
  g_borderless = host->SettingBool(kName, "Borderless");
  if (!g_title[0] && !g_miniTitle && !g_sysMenu && !g_borderless) {
    host->Log("%s: Title is empty and MiniTitle, SysMenu and Borderless are off, nothing to do", kName);
    return true;
  }
  if (g_title[0]) {
    host->Log("%s: title \"%s\"", kName, g_title);
  }
  if (g_sysMenu) {
    g_getRenderSystemHolder = reinterpret_cast<RenderSystemHolder* (*)()>(host->Resolve("?GetRenderSystemHolder@@YAPAURenderSystemHolder@@XZ"));
    if (!g_getRenderSystemHolder) {
      host->Log("%s: no exe function named GetRenderSystemHolder", kName);
      return false;
    }
  }
  return ro::HookSystem(host, "user32.dll", "CreateWindowExA", &CreateWindowExHook, g_originalCreate, kName) &&
         (!g_sysMenu || ro::Hook(host, &WindowProc, &WindowProcHook, g_originalWindowProc, kName));
}
