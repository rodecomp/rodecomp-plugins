// Hooks UIWindowMgr::SendFlagEffect: hotkey HK_FLAG_n + 200 sends MM_CASH_EMOTION with the configured name.
#include "RoPlugin.h"

#include "Client/Mode/CMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Contents/Hotkeys/InterfaceHotkey.h"
#include "UI/Window/UIWindowMgr.h"

namespace {
  constexpr const char* kName = "FlagEmotes";
  constexpr int kFlags = HK_FLAG_10 - HK_FLAG_1 + 1;

  const char* g_flags[kFlags];

  void (UIWindowMgr::*g_sendFlag)(int);

  struct WindowMgrHook : UIWindowMgr {
    void SendFlagDetour(int hotkey) {
      const int index = hotkey - (HK_FLAG_1 + 200);
      if (index >= 0 && index < kFlags && g_flags[index] != nullptr && g_modeMgr.currentMode != nullptr) {
        g_modeMgr.currentMode->SendMsg(MM_CASH_EMOTION, reinterpret_cast<int>(g_flags[index]), 0, 0, 0);
        return;
      }
      (this->*g_sendFlag)(hotkey);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const int count = host->SettingCount(kName, "Flags");
  bool any = false;
  for (int i = 0; i < count && i < kFlags; ++i) {
    const char* name = host->SettingItem(kName, "Flags", i);
    if (name[0] != '\0') {
      g_flags[i] = name;
      any = true;
    }
  }
  if (!any) {
    return true;
  }
  return ro::Hook(host, &UIWindowMgr::SendFlagEffect, &WindowMgrHook::SendFlagDetour, g_sendFlag, kName);
}
