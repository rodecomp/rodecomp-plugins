// CSession::IsBerserkActive is only asked by CGameMode::SendMsg's chat channels, which drop the message while it
// returns 1. It always returns 0 here.
#include "RoPlugin.h"

#include "Client/Session/Session.h"

namespace {
  constexpr const char* kName = "BerserkChat";

  int (CSession::*g_original)() const;

  struct SessionHook : CSession {
    int IsBerserkActive() const {
      return 0;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CSession::IsBerserkActive, &SessionHook::IsBerserkActive, g_original, kName);
}
