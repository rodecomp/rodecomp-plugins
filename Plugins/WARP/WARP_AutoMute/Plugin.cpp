// Hooks SetApplicationActive (called by WindowProc on activation changes): after the original, pauses g_soundMgr when
// the window becomes inactive and resumes it when it becomes active.
#include "RoPlugin.h"

#include "Client/Application/ApplicationActivation.h"
#include "Contents/Sound/SoundMgr.h"

namespace {
  constexpr const char* kName = "WARP_AutoMute";

  decltype(&SetApplicationActive) g_original;

  void SetApplicationActiveHook(int value) {
    g_original(value);
    if (g_soundMgr != nullptr) {
      g_soundMgr->Pause(value != 0 ? 0 : 1);
    }
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &SetApplicationActive, &SetApplicationActiveHook, g_original, kName);
}
