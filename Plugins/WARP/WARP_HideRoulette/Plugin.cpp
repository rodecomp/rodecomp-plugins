// Hooks UIWindowMgr::EnsureWindow, which creates every window: WID_OPENROULLETTEWND and WID_ROULLETTEWND return no
// window.
#include "RoPlugin.h"

#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

namespace {
  constexpr const char* kName = "WARP_HideRoulette";

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindowDetour(int id) {
      if (id == WID_OPENROULLETTEWND || id == WID_ROULLETTEWND) {
        return nullptr;
      }
      return (this->*g_ensureWindow)(id);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName);
}
