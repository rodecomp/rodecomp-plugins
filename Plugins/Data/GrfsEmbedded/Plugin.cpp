// Gravity's WinMain loads one archive: g_pakRegistry.Add("data.grf"), its first PakRegistry::Add call. The detour
// replaces that one call with the chosen source and lets every later Add (e.g. event.grf) through:
// - DataIni (WARP GRFsFromIni): keys 9 down to 0 of section [Data] in .\DATA.INI, as WARP's loader read them; if none
//   is set it writes 2=data.grf and reads again. Later adds take priority, so key 0 wins.
// - List (WARP GrfsEmbedded): the Archives setting, in order; later entries take priority.
// - DataGrf: the game's own data.grf only (no hook).
#include "RoPlugin.h"

#include "Archive/PakRegistry.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {
  constexpr const char* kName = "GrfsEmbedded";
  constexpr const char* kDataIni = ".\\DATA.INI";

  const RoHostApi* g_host;
  bool (PakRegistry::*g_add)(const char*);
  bool g_fromIni;
  bool g_replaced;

  int AddFromIni(PakRegistry& registry) {
    int added = 0;
    for (int key = 9; key >= 0; --key) {
      char name[2] = {static_cast<char>('0' + key), 0};
      char file[MAX_PATH] = {};
      if (GetPrivateProfileStringA("Data", name, "", file, sizeof(file), kDataIni) > 0) {
        g_host->Log("%s: DATA.INI %d=%s", kName, key, file);
        (registry.*g_add)(file);
        ++added;
      }
    }
    return added;
  }

  struct PakRegistryHook : PakRegistry {
    bool Add(const char* filename) {
      if (g_replaced || filename == nullptr || _stricmp(filename, "data.grf") != 0) {
        return (this->*g_add)(filename);
      }
      g_replaced = true;
      if (g_fromIni) {
        if (AddFromIni(*this) == 0 && WritePrivateProfileStringA("Data", "2", "data.grf", kDataIni)) {
          AddFromIni(*this);
        }
        return true;
      }
      const int count = g_host->SettingCount(kName, "Archives");
      for (int index = 0; index < count; ++index) {
        (this->*g_add)(g_host->SettingItem(kName, "Archives", index));
      }
      return true;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_host = host;
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const char* source = host->SettingString(kName, "Source");
  if (std::strcmp(source, "DataGrf") == 0) {
    host->Log("%s: Source is DataGrf, the game loads data.grf only", kName);
    return true;
  }
  g_fromIni = std::strcmp(source, "List") != 0;
  if (!g_fromIni) {
    const int count = host->SettingCount(kName, "Archives");
    if (count == 0) {
      host->Log("%s: Source is List but Archives is empty, the game loads data.grf only", kName);
      return true;
    }
    for (int index = 0; index < count; ++index) {
      host->Log("%s: archive %d: %s", kName, index, host->SettingItem(kName, "Archives", index));
    }
  }
  return ro::Hook(host, &PakRegistry::Add, &PakRegistryHook::Add, g_add, kName);
}
