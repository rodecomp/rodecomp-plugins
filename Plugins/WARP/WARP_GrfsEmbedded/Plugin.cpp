// WinMain's DATA.INI loop is inlined, so PakRegistry::Add ignores every archive until LoadClientInfo, which WinMain calls
// right after that loop; the LoadClientInfo detour then adds the configured list and lets later Add calls through.
#include "RoPlugin.h"

#include "Archive/PakRegistry.h"
#include "Client/Application/ClientInfo.h"

#include <cstdio>
#include <cstring>

namespace {
  constexpr const char* kName = "WARP_GrfsEmbedded";

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

  // [Grfs] 0, 1, ... from plugins\WARP_GrfsEmbedded.ini next to this DLL, up to the first missing key.
  void AddConfiguredArchives() {
    char ini[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&AddConfiguredArchives), &self);
    GetModuleFileNameA(self, ini, MAX_PATH);
    if (char* dot = std::strrchr(ini, '.')) {
      std::strcpy(dot, ".ini");
    }
    int index = 0;
    for (;; ++index) {
      char key[16];
      char file[MAX_PATH];
      std::snprintf(key, sizeof(key), "%d", index);
      if (!GetPrivateProfileStringA("Grfs", key, "", file, sizeof(file), ini)) {
        break;
      }
      (g_pakRegistry.*g_add)(file);
    }
    if (index == 0) {
      (g_pakRegistry.*g_add)("data.grf");
    }
  }

  void LoadClientInfoWithArchives(char* protection) {
    g_listAdded = true;
    AddConfiguredArchives();
    g_loadClientInfo(protection);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &PakRegistry::Add, &PakRegistryHook::Add, g_add, kName) &&
         ro::Hook(host, &LoadClientInfo, &LoadClientInfoWithArchives, g_loadClientInfo, kName);
}
