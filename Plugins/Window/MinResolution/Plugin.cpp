// CSession::LoadOptionInfo reads WIDTH/HEIGHT and replaces them with 1024x768 when HEIGHT is below 664 (768 - 104),
// then copies them to the viewport. That check is inline, so after the original the size is recomputed from the same
// options with WARP's patched limits: the fallback becomes MinWidth x MinHeight and the HEIGHT threshold keeps the
// same 104-pixel margin below it (MinHeight - 104). With the defaults this is exactly the game's behaviour.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Render/DX7/RenderSystemDX7.h"

namespace {
  constexpr const char* kName = "MinResolution";
  constexpr int kHeightMargin = 768 - 664;

  int g_minWidth;
  int g_minHeight;

  void (CSession::*g_loadOptionInfo)();

  struct SessionHook : CSession {
    void LoadOptionInfoDetour() {
      (this->*g_loadOptionInfo)();
      int width = this->GetOptionInteger("WIDTH", g_minWidth);
      int height = this->GetOptionInteger("HEIGHT", g_minHeight);
      if (static_cast<unsigned>(height) < static_cast<unsigned>(g_minHeight - kHeightMargin)) {
        width = g_minWidth;
        height = g_minHeight;
      }
      g_configuredScreenWidth = width;
      g_configuredScreenHeight = height;
      g_viewportWidth = width;
      g_viewportHeight = height;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_minWidth = static_cast<int>(host->SettingInt(kName, "MinWidth"));
  g_minHeight = static_cast<int>(host->SettingInt(kName, "MinHeight"));
  host->Log("%s: minimum %dx%d", kName, g_minWidth, g_minHeight);
  return ro::Hook(host, &CSession::LoadOptionInfo, &SessionHook::LoadOptionInfoDetour, g_loadOptionInfo, kName);
}
