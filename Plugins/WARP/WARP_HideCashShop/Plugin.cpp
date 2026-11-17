// Hooks UIWindowMgr::EnsureWindow, which creates every window: WID_CASHSHOP_WND returns no window, WID_NCASH_CALLWND is
// hidden after creation. Hooks UIWindowMgr::ProcessShortcutKey to swallow the Cash Shop hotkey, which would otherwise
// request the shop from the server.
#include "RoPlugin.h"

#include "Contents/Hotkeys/InterfaceHotkey.h"
#include "UI/Window/UIWindow.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

namespace {
  constexpr const char* kName = "WARP_HideCashShop";

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);
  bool (UIWindowMgr::*g_processShortcutKey)(int);

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindowDetour(int id) {
      if (id == WID_CASHSHOP_WND) {
        return nullptr;
      }
      UIWindow* window = (this->*g_ensureWindow)(id);
      if (id == WID_NCASH_CALLWND && window != nullptr) {
        window->Show(0);
      }
      return window;
    }

    bool ProcessShortcutKeyDetour(int action) {
      if (action == 4 && this->GetHotkeyBehavior() == HK_CASHSHOPWND + 100) {
        return true;
      }
      return (this->*g_processShortcutKey)(action);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName) &&
         ro::Hook(host, &UIWindowMgr::ProcessShortcutKey, &WindowMgrHook::ProcessShortcutKeyDetour, g_processShortcutKey, kName);
}
