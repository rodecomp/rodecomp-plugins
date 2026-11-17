// Hooks CGameMode::InitializeCamera, the only place the map's starting camera pitch is chosen: after the original,
// a map without view parameters (indoor or outdoor) starts at pitch -35, which is
// where the mod's fixed pitch and its clamp to pitch +- range always end. Only pitch fields are written.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"

namespace {
  constexpr const char* kName = "WARP_HighCamAngle";

  constexpr float kPitch = -35.0f;

  void (CGameMode::*g_original)();

  struct GameModeHook : CGameMode {
    void InitializeCamera() {
      (this->*g_original)();
      if (this->hasViewParameters) {
        return;
      }
      g_cameraPitch = kPitch;
      this->camera->camera.pitch = kPitch;
      this->camera->camera.desiredPitch = kPitch;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CGameMode::InitializeCamera, &GameModeHook::InitializeCamera, g_original, kName);
}
