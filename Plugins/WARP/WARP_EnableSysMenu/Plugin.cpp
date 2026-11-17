// Hooks CreateApplicationWindow to give the windowed client WS_OVERLAPPEDWINDOW (InitApp's style is inline, so the
// outer size InitApp computed with AdjustWindowRect is grown by the new frame's extra border), and WindowProc for
// WM_GETMINMAXINFO (keep the default max track size) and the SC_MINIMIZE/SC_MOUSEMENU/SC_RESTORE/SC_MONITORPOWER system
// commands (handed to DefWindowProcA instead of swallowed), both under the original's IsWindowed() condition.
#include "RoPlugin.h"

#include "Client/Application/ApplicationWindow.h"
#include "Render/DeviceContext/DCSurface.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_EnableSysMenu";

  constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

  decltype(&CreateApplicationWindow) g_originalCreate;
  decltype(&WindowProc) g_originalWindowProc;
  // The exe's GetRenderSystemHolder: the header's inline copy would have its own empty static in this DLL.
  RenderSystemHolder* (*g_getRenderSystemHolder)();

  bool IsWindowed() {
    RenderSystemHolder* holder = g_getRenderSystemHolder();
    return holder && holder->IsWindowed();
  }

  HWND __stdcall CreateApplicationWindowHook(
    DWORD extendedStyle, const char* className, const char* ignoredTitle, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, void* parameter
  ) {
    if (style != WS_POPUP) {
      RECT before{0, 0, 0, 0};
      RECT after{0, 0, 0, 0};
      AdjustWindowRect(&before, style, FALSE);
      AdjustWindowRect(&after, kStyle, FALSE);
      width += (after.right - after.left) - (before.right - before.left);
      height += (after.bottom - after.top) - (before.bottom - before.top);
      style = kStyle;
    }
    return g_originalCreate(extendedStyle, className, ignoredTitle, style, x, y, width, height, parent, menu, instance, parameter);
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
  g_getRenderSystemHolder = reinterpret_cast<RenderSystemHolder* (*)()>(host->Resolve("?GetRenderSystemHolder@@YAPAURenderSystemHolder@@XZ"));
  if (!g_getRenderSystemHolder) {
    host->Log("%s: no exe function named GetRenderSystemHolder", kName);
    return false;
  }
  return ro::Hook(host, &CreateApplicationWindow, &CreateApplicationWindowHook, g_originalCreate, kName) &&
         ro::Hook(host, &WindowProc, &WindowProcHook, g_originalWindowProc, kName);
}
