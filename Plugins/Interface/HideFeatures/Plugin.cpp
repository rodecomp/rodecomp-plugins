// Hooks UIWindowMgr::EnsureWindow, which creates every window, once for CashShop and Roulette:
// - CashShop: WID_CASHSHOP_WND returns no window and WID_NCASH_CALLWND is hidden after creation; UIWindowMgr::
//   ProcessShortcutKey (hooked only for CashShop) swallows the Cash Shop hotkey, which would otherwise request the shop
//   from the server.
// - Roulette: WID_OPENROULLETTEWND and WID_ROULLETTEWND return no window.
// - GravityImages (rodecomp patch 0009): TextureCache::Load (hooked only for GravityImages) loads "유저인터페이스\" in
//   place of the login overlay's Gravity logo, age-rating and ad textures (T_GameGrade.tga, T_R<n>.tga, T_중력성인.tga,
//   T_테입<n>.tga), the path the patch leaves by zeroing their `T`. No such file exists, so Load caches a 1x1
//   transparent texture under that name. Returning null instead would crash: DrawLoginOverlay passes Load's result
//   straight to CMode::DrawTexture, which dereferences it. With the patch applied the paths never match.
#include "RoPlugin.h"

#include "Contents/Hotkeys/InterfaceHotkey.h"
#include "Render/Textures/TextureCache.h"
#include "UI/Window/UIWindow.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

#include <cstring>

namespace {
  constexpr const char* kName = "HideFeatures";
  // cp949 like the game's literals (/execution-charset:.949); kBlank is the prefix without the "T_".
  constexpr char kPrefix[] = "유저인터페이스\\T_";
  constexpr char kBlank[] = "유저인터페이스\\";

  bool g_cashShop;
  bool g_roulette;

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);
  bool (UIWindowMgr::*g_processShortcutKey)(int);
  CTexture* (TextureCache::*g_load)(const char*, int, bool, bool, bool);

  // True if `text` is one or more digits followed by `tail`.
  bool NumberThen(const char* text, const char* tail) {
    if (*text < '0' || *text > '9') {
      return false;
    }
    while (*text >= '0' && *text <= '9') {
      ++text;
    }
    return std::strcmp(text, tail) == 0;
  }

  bool IsGravityImage(const char* name) {
    if (std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0) {
      return false;
    }
    const char* rest = name + sizeof(kPrefix) - 1;
    return std::strcmp(rest, "GameGrade.tga") == 0 || std::strcmp(rest, "중력성인.tga") == 0 ||
           (rest[0] == 'R' && NumberThen(rest + 1, ".tga")) ||
           (std::strncmp(rest, "테입", sizeof("테입") - 1) == 0 && NumberThen(rest + sizeof("테입") - 1, ".tga"));
  }

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

  struct TextureCacheHook : TextureCache {
    CTexture* LoadDetour(const char* name, int option, bool fullSize, bool dynamic, bool renderTarget) {
      return (this->*g_load)(IsGravityImage(name) ? kBlank : name, option, fullSize, dynamic, renderTarget);
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
  const bool gravityImages = host->SettingBool(kName, "GravityImages");
  if (!g_cashShop && !g_roulette && !gravityImages) {
    host->Log("%s: CashShop, Roulette and GravityImages are all off, nothing to do", kName);
    return true;
  }
  return (!(g_cashShop || g_roulette) ||
          ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName)) &&
         (!g_cashShop ||
          ro::Hook(host, &UIWindowMgr::ProcessShortcutKey, &WindowMgrHook::ProcessShortcutKeyDetour, g_processShortcutKey, kName)) &&
         (!gravityImages || ro::Hook(host, &TextureCache::Load, &TextureCacheHook::LoadDetour, g_load, kName));
}
