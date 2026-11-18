// Hooks UIWindowMgr::EnsureWindow, which creates every window, to return no window for the configured IDs, as WARP's
// redirect of UIWindowMgr::MakeWindow's switch to its default case does.
#include "RoPlugin.h"

#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

#include <vector>

namespace {
  constexpr const char* kName = "HideWindows";

  std::vector<int> g_ids;

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindowDetour(int id) {
      for (const int hidden : g_ids) {
        if (hidden == id) {
          return nullptr;
        }
      }
      return (this->*g_ensureWindow)(id);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  if (host->SettingBool(kName, "HideWorldView")) {
    g_ids.push_back(WID_ROMAPWND);
  }
  const int count = host->SettingCount(kName, "WindowIds");
  for (int i = 0; i < count; ++i) {
    g_ids.push_back(static_cast<int>(host->SettingItemInt(kName, "WindowIds", i)));
  }
  if (g_ids.empty()) {
    host->Log("%s: no window to hide, nothing to do", kName);
    return true;
  }
  return ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName);
}
