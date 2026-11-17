// Hooks UIWindowMgr::EnsureWindow, which creates every window, once for both switches:
// - CashShop: WID_CASHSHOP_WND returns no window and WID_NCASH_CALLWND is hidden after creation; UIWindowMgr::
//   ProcessShortcutKey (hooked only for CashShop) swallows the Cash Shop hotkey, which would otherwise request the shop
//   from the server.
// - Roulette: WID_OPENROULLETTEWND and WID_ROULLETTEWND return no window.
#include "RoPlugin.h"

#include "Contents/Hotkeys/InterfaceHotkey.h"
#include "UI/Window/UIWindow.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

namespace {
  constexpr const char* kName = "WARP_HideFeatures";

  bool g_cashShop;
  bool g_roulette;

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);
  bool (UIWindowMgr::*g_processShortcutKey)(int);

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindowDetour(int id) {
      if ((g_cashShop && id == WID_CASHSHOP_WND) || (g_roulette && (id == WID_OPENROULLETTEWND || id == WID_ROULLETTEWND))) {
        return nullptr;
      }
      UIWindow* window = (this->*g_ensureWindow)(id);
      if (g_cashShop && id == WID_NCASH_CALLWND && window != nullptr) {
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
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_cashShop = host->SettingBool(kName, "CashShop");
  g_roulette = host->SettingBool(kName, "Roulette");
  if (!g_cashShop && !g_roulette) {
    host->Log("%s: CashShop and Roulette are both off, nothing to do", kName);
    return true;
  }
  return ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName) &&
         (!g_cashShop ||
          ro::Hook(host, &UIWindowMgr::ProcessShortcutKey, &WindowMgrHook::ProcessShortcutKeyDetour, g_processShortcutKey, kName));
}
