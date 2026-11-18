// Audio tweaks, each switch hooking only what it needs (no hook is shared between switches):
// - NoBgm: CSession::GetBgmFileName (map BGM: mp3NameTable.txt lookup, default bgm\01.mp3) returns an empty name, and
//   CSoundMgr::PlayStream replaces the literal bgm\01.mp3 (CLoginMode::Initialize) with an empty name, as the original
//   patch blanked those strings. Server-driven BGM (CGameMode::PlayBgm types 0/1, ReceiveNpcBgm) still plays.
// - Force44kHz: CSoundMgr::Initialize, the only reader of g_soundConfiguration.digitalRateType, runs with the type set
//   to 0 (44100 Hz) and the configured value is restored afterwards.
// - AutoMute: SetApplicationActive (called by WindowProc on activation changes): after the original, pauses g_soundMgr
//   when the window becomes inactive and resumes it when it becomes active.
// - FirstFrameSounds: CRenderObject::PlayFrameSounds rewinds its last-sounded-frame cursor to 0 when the animation
//   changed and then plays frames 1..curMotion, skipping frame 0. WARP rewinds it to -1 instead; the detour plays the
//   frame-0 sound itself (same lookup and PlayWave call as the original) in exactly that case (animation changed and
//   curMotion >= 0) before running the original, which plays the rest as before.
#include "RoPlugin.h"

#include "Client/Application/ApplicationActivation.h"
#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMgr.h"
#include "Client/Session/Session.h"
#include "Contents/Actors/Pc.h"
#include "Contents/Actors/RenderObject.h"
#include "Contents/Sound/SoundConfiguration.h"
#include "Contents/Sound/SoundMgr.h"
#include "Contents/World/World.h"
#include "Resource/Sprites/ActionResource.h"

#include <cstring>
#include <string>

namespace {
  constexpr const char* kName = "SoundSettings";

  const char* (CSession::*g_getBgmFileName)(const char*);
  void (CSoundMgr::*g_playStream)(const char*, int, bool);
  bool (CSoundMgr::*g_initialize)();
  decltype(&SetApplicationActive) g_setApplicationActive;
  void (CRenderObject::*g_playFrameSounds)();

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

    bool InitializeDetour() {
      const int configured = g_soundConfiguration.digitalRateType;
      g_soundConfiguration.digitalRateType = 0;
      const bool result = (this->*g_initialize)();
      g_soundConfiguration.digitalRateType = configured;
      return result;
    }
  };

  void SetApplicationActiveDetour(int value) {
    g_setApplicationActive(value);
    if (g_soundMgr != nullptr) {
      g_soundMgr->Pause(value != 0 ? 0 : 1);
    }
  }

  struct RenderObjectHook : CRenderObject {
    void PlayFrameSoundsDetour() {
      CActRes* action = this->render.action;
      if (this->render.lastSoundAnimation != static_cast<std::uint32_t>(this->render.animation) && this->render.curMotion >= 0 &&
          action != nullptr) {
        const ActionFrame& frame = action->Frame(this->render.previousAnimation, 0);
        if (frame.sound != -1) {
          const std::string sound = action->Sound(frame.sound);
          CGameMode* gameMode = g_modeMgr.GetGameMode();
          if (sound.find(".wav") != std::string::npos && gameMode != nullptr && gameMode->scene != nullptr &&
              gameMode->scene->player != nullptr && g_soundMgr != nullptr) {
            const vector3d& player = static_cast<const CRenderObject*>(gameMode->scene->player)->render.position;
            g_soundMgr->PlayWave(
              sound.c_str(), this->render.position.x - player.x, 0.0f, this->render.position.z - player.z, 250, 40, 1.0f, 0
            );
          }
        }
      }
      (this->*g_playFrameSounds)();
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool noBgm = host->SettingBool(kName, "NoBgm");
  const bool force44kHz = host->SettingBool(kName, "Force44kHz");
  const bool autoMute = host->SettingBool(kName, "AutoMute");
  const bool firstFrameSounds = host->SettingBool(kName, "FirstFrameSounds");
  if (!noBgm && !force44kHz && !autoMute && !firstFrameSounds) {
    host->Log("%s: NoBgm, Force44kHz, AutoMute and FirstFrameSounds are all off, nothing to do", kName);
    return true;
  }
  return (!noBgm ||
          (ro::Hook(host, &CSession::GetBgmFileName, &SessionHook::GetBgmFileNameDetour, g_getBgmFileName, kName) &&
           ro::Hook(host, &CSoundMgr::PlayStream, &SoundMgrHook::PlayStreamDetour, g_playStream, kName))) &&
         (!force44kHz || ro::Hook(host, &CSoundMgr::Initialize, &SoundMgrHook::InitializeDetour, g_initialize, kName)) &&
         (!autoMute || ro::Hook(host, &SetApplicationActive, &SetApplicationActiveDetour, g_setApplicationActive, kName)) &&
         (!firstFrameSounds ||
          ro::Hook(host, &CRenderObject::PlayFrameSounds, &RenderObjectHook::PlayFrameSoundsDetour, g_playFrameSounds, kName));
}
