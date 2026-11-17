// CTownInfoMgr::LoadScript reports success without running System\Towninfo.lub.
#include "RoPlugin.h"

#include "Contents/Map/TownInfoMgr.h"

namespace {
  constexpr const char* kName = "WARP_SkipTownInfo";

  bool (CTownInfoMgr::*g_original)();

  struct TownInfoHook : CTownInfoMgr {
    bool LoadScript() {
      return true;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CTownInfoMgr::LoadScript, &TownInfoHook::LoadScript, g_original, kName);
}
