// Hooks per setting:
// - MoveIcon: UIWindowMgr::EnsureWindow, for WID_NCASH_CALLWND when it creates the icon window (nCashCallWnd was
//   null), moves it to IconX, IconY after the original; a negative coordinate counts from the right/bottom edge of the
//   viewport, as WARP's does.
// - ZeroSecondaryPoints: UICashShopWnd::Paint runs with m_secondaryPoints set to 0, restored afterwards, so the
//   "%d C" line next to the refresh label shows 0 C.
#include "RoPlugin.h"

#include "Render/Renderer.h"
#include "UI/CashShop/UICashShopWnd.h"
#include "UI/Window/UIWindow.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

namespace {
  constexpr const char* kName = "CashShop";

  int g_iconX;
  int g_iconY;

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);
  void (UICashShopWnd::*g_paint)();

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindowDetour(int id) {
      const bool created = id == WID_NCASH_CALLWND && this->nCashCallWnd == nullptr;
      UIWindow* window = (this->*g_ensureWindow)(id);
      if (created && window != nullptr) {
        const int x = g_iconX < 0 ? g_renderer->viewport.width + g_iconX : g_iconX;
        const int y = g_iconY < 0 ? g_renderer->viewport.height + g_iconY : g_iconY;
        window->Move(x, y);
      }
      return window;
    }
  };

  struct CashShopHook : UICashShopWnd {
    void PaintDetour() {
      const int points = this->m_secondaryPoints;
      this->m_secondaryPoints = 0;
      (this->*g_paint)();
      this->m_secondaryPoints = points;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool moveIcon = host->SettingBool(kName, "MoveIcon");
  const bool zero = host->SettingBool(kName, "ZeroSecondaryPoints");
  g_iconX = static_cast<int>(host->SettingInt(kName, "IconX"));
  g_iconY = static_cast<int>(host->SettingInt(kName, "IconY"));
  if (!moveIcon && !zero) {
    host->Log("%s: MoveIcon and ZeroSecondaryPoints are off, nothing to do", kName);
    return true;
  }
  return (!moveIcon ||
          ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName)) &&
         (!zero || ro::HookVirtual(host, "?Paint@UICashShopWnd@@UAEXXZ", &CashShopHook::PaintDetour, g_paint, kName));
}
