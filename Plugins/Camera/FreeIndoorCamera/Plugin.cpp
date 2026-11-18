// WARP blanks the "indoorRswTable.txt" file name so the indoor map list loads empty. Its only reader is
// CSession::IsIndoorMapName (called once, from CGameMode's camera setup), so that function returns false instead:
// every map gets the outdoor camera (rotation allowed, outdoor pitch and distance limits).
#include "RoPlugin.h"

#include "Client/Session/Session.h"

namespace {
  constexpr const char* kName = "FreeIndoorCamera";

  bool (CSession::*g_isIndoorMapName)(const char*) const;

  struct SessionHook : CSession {
    bool IsIndoorMapNameDetour(const char*) const {
      return false;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CSession::IsIndoorMapName, &SessionHook::IsIndoorMapNameDetour, g_isIndoorMapName, kName);
}
