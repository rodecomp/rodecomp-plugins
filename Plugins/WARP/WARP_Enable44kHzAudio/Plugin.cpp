// Hooks CSoundMgr::Initialize, the only reader of g_soundConfiguration.digitalRateType: runs it with the type set to 0
// (44100 Hz) and restores the configured value afterwards.
#include "RoPlugin.h"

#include "Contents/Sound/SoundConfiguration.h"
#include "Contents/Sound/SoundMgr.h"

namespace {
  constexpr const char* kName = "WARP_Enable44kHzAudio";

  bool (CSoundMgr::*g_original)();

  struct SoundMgrHook : CSoundMgr {
    bool InitializeDetour() {
      const int configured = g_soundConfiguration.digitalRateType;
      g_soundConfiguration.digitalRateType = 0;
      const bool result = (this->*g_original)();
      g_soundConfiguration.digitalRateType = configured;
      return result;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CSoundMgr::Initialize, &SoundMgrHook::InitializeDetour, g_original, kName);
}
