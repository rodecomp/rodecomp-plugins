// WinMain's DATA.INI loop is inlined, so PakRegistry::Add ignores every archive until LoadClientInfo, which WinMain calls
// right after that loop; the LoadClientInfo detour then adds the Archives setting (plugins\WARP_GrfsEmbedded.toml) and
// lets later Add calls through. An empty list installs no hook: DATA.INI is used as without the plugin.
#include "RoPlugin.h"

#include "Archive/PakRegistry.h"
#include "Client/Application/ClientInfo.h"

namespace {
  constexpr const char* kName = "WARP_GrfsEmbedded";

  const RoHostApi* g_host;
  bool (PakRegistry::*g_add)(const char*);
  decltype(&LoadClientInfo) g_loadClientInfo;
  bool g_listAdded;

  struct PakRegistryHook : PakRegistry {
    bool Add(const char* filename) {
      if (!g_listAdded) {
        return true;
      }
      return (this->*g_add)(filename);
    }
  };

  void LoadClientInfoWithArchives(char* protection) {
    g_listAdded = true;
    const int count = g_host->SettingCount(kName, "Archives");
    for (int index = 0; index < count; ++index) {
      (g_pakRegistry.*g_add)(g_host->SettingItem(kName, "Archives", index));
    }
    g_loadClientInfo(protection);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_host = host;
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const int count = host->SettingCount(kName, "Archives");
  if (count == 0) {
    host->Log("%s: Archives is empty, DATA.INI is used", kName);
    return true;
  }
  for (int index = 0; index < count; ++index) {
    host->Log("%s: archive %d: %s", kName, index, host->SettingItem(kName, "Archives", index));
  }
  return ro::Hook(host, &PakRegistry::Add, &PakRegistryHook::Add, g_add, kName) &&
         ro::Hook(host, &LoadClientInfo, &LoadClientInfoWithArchives, g_loadClientInfo, kName);
}
