// InitializeSignBoardManager (LoadList + Initialize at launch) does nothing, so the SignBoard list is never loaded and
// the later per-map CSignBoardMgr::Initialize finds no entries. It is the single launch call site of LoadList.
#include "RoPlugin.h"

#include "Contents/SignBoard/SignBoard.h"

namespace {
  constexpr const char* kName = "WARP_SkipSignBoard";

  void(__fastcall* g_original)(CSignBoardMgr*);

  void __fastcall InitializeSignBoardManagerDetour(CSignBoardMgr*) {
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &InitializeSignBoardManager, &InitializeSignBoardManagerDetour, g_original, kName);
}
