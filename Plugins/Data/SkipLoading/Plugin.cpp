// Each switch installs its own hook, only when on; no hook is shared:
// - SignBoard: InitializeSignBoardManager (LoadList + Initialize at launch) does nothing, so the SignBoard list is never
//   loaded and the later per-map CSignBoardMgr::Initialize finds no entries. It is the single launch call site of
//   LoadList.
// - TownInfo: CTownInfoMgr::LoadScript reports success without running System\Towninfo.lub.
// - ViewPointTable: CSession::LoadMapViewParameters leaves the table empty and reports failure, as when the file is
//   missing, so no map locks the camera.
// - MapInfo: CMapInfoMgr::LoadScript sets the "nowhere" name the original sets first and reports success without
//   running system\mapInfo_true.lub, so no map has a sign and ShowMapName draws nothing.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Map/MapInfoMgr.h"
#include "Contents/Map/TownInfoMgr.h"
#include "Contents/SignBoard/SignBoard.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/MSGSTRINGID.h"

namespace {
  constexpr const char* kName = "SkipLoading";

  void(__fastcall* g_initializeSignBoardManager)(CSignBoardMgr*);
  bool (CTownInfoMgr::*g_loadScript)();
  bool (CSession::*g_loadMapViewParameters)();
  bool (CMapInfoMgr::*g_loadMapInfo)();

  void __fastcall InitializeSignBoardManagerDetour(CSignBoardMgr*) {
  }

  struct TownInfoHook : CTownInfoMgr {
    bool LoadScriptDetour() {
      return true;
    }
  };

  struct SessionHook : CSession {
    bool LoadMapViewParametersDetour() {
      this->m_mapViewParameters.clear();
      return false;
    }
  };

  struct MapInfoHook : CMapInfoMgr {
    bool LoadScriptDetour() {
      this->m_emptyName = LookupMessage(MSI_NOWHERE);
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
  const bool viewPoint = host->SettingBool(kName, "ViewPointTable");
  const bool mapInfo = host->SettingBool(kName, "MapInfo");
  if (!signBoard && !townInfo && !viewPoint && !mapInfo) {
    host->Log("%s: every switch is off, nothing to do", kName);
    return true;
  }
  return (!signBoard ||
          ro::Hook(host, &InitializeSignBoardManager, &InitializeSignBoardManagerDetour, g_initializeSignBoardManager, kName)) &&
         (!townInfo || ro::Hook(host, &CTownInfoMgr::LoadScript, &TownInfoHook::LoadScriptDetour, g_loadScript, kName)) &&
         (!viewPoint || ro::Hook(host, &CSession::LoadMapViewParameters, &SessionHook::LoadMapViewParametersDetour,
                                 g_loadMapViewParameters, kName)) &&
         (!mapInfo || ro::Hook(host, &CMapInfoMgr::LoadScript, &MapInfoHook::LoadScriptDetour, g_loadMapInfo, kName));
}
