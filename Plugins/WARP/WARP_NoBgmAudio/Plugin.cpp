// Hooks CSession::GetBgmFileName (map BGM: mp3NameTable.txt lookup, default bgm\01.mp3) to return an empty name, and
// CSoundMgr::PlayStream to replace the literal bgm\01.mp3 (CLoginMode::Initialize) with an empty name, as the original
// patch blanked those strings. Server-driven BGM (CGameMode::PlayBgm types 0/1, ReceiveNpcBgm) still plays.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Sound/SoundMgr.h"

#include <cstring>

namespace {
  constexpr const char* kName = "WARP_NoBgmAudio";

  const char* (CSession::*g_getBgmFileName)(const char*);
  void (CSoundMgr::*g_playStream)(const char*, int, bool);

  struct SessionHook : CSession {
    const char* GetBgmFileNameDetour(const char*) {
      return "";
    }
  };

  struct SoundMgrHook : CSoundMgr {
    void PlayStreamDetour(const char* name, int loopCount, bool fade) {
      if (name != nullptr && std::strcmp(name, "bgm\\01.mp3") == 0) {
        name = "";
      }
      (this->*g_playStream)(name, loopCount, fade);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CSession::GetBgmFileName, &SessionHook::GetBgmFileNameDetour, g_getBgmFileName, kName) &&
         ro::Hook(host, &CSoundMgr::PlayStream, &SoundMgrHook::PlayStreamDetour, g_playStream, kName);
}
