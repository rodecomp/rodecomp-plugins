// Each switch installs its own hook, only when on; no hook is shared:
// - SignBoard: InitializeSignBoardManager (LoadList + Initialize at launch) does nothing, so the SignBoard list is never
//   loaded and the later per-map CSignBoardMgr::Initialize finds no entries. It is the single launch call site of
//   LoadList.
// - TownInfo: CTownInfoMgr::LoadScript reports success without running System\Towninfo.lub.
#include "RoPlugin.h"

#include "Contents/Map/TownInfoMgr.h"
#include "Contents/SignBoard/SignBoard.h"

namespace {
  constexpr const char* kName = "WARP_SkipLoading";

  void(__fastcall* g_initializeSignBoardManager)(CSignBoardMgr*);
  bool (CTownInfoMgr::*g_loadScript)();

  void __fastcall InitializeSignBoardManagerDetour(CSignBoardMgr*) {
  }

  struct TownInfoHook : CTownInfoMgr {
    bool LoadScriptDetour() {
      return true;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool signBoard = host->SettingBool(kName, "SignBoard");
  const bool townInfo = host->SettingBool(kName, "TownInfo");
  if (!signBoard && !townInfo) {
    host->Log("%s: SignBoard and TownInfo are both off, nothing to do", kName);
    return true;
  }
  return (!signBoard ||
          ro::Hook(host, &InitializeSignBoardManager, &InitializeSignBoardManagerDetour, g_initializeSignBoardManager, kName)) &&
         (!townInfo || ro::Hook(host, &CTownInfoMgr::LoadScript, &TownInfoHook::LoadScriptDetour, g_loadScript, kName));
}
