// Hooks CSession::LoadExternalSettings, the only loader of g_session.m_makeableDoram (Lua MakeableRace.Doram): after
// the original, forces the flag to false, so UINewMakeCharWnd shows the "coming soon" Doram tile and refuses it.
#include "RoPlugin.h"

#include "Client/Session/Session.h"

namespace {
  constexpr const char* kName = "DisableDoram";

  void (CSession::*g_original)();

  struct SessionHook : CSession {
    void LoadExternalSettingsDetour() {
      (this->*g_original)();
      this->m_makeableDoram = false;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CSession::LoadExternalSettings, &SessionHook::LoadExternalSettingsDetour, g_original, kName);
}
