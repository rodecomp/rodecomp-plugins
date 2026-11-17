// Hooks CGameMode::InitializeCamera, the only place the map's starting camera pitch is chosen: after the original,
// a map without view parameters (indoor or outdoor) starts at the Pitch setting, which is where the WARP presets' fixed
// pitch and their clamp to pitch +- range always end. Only pitch fields are written.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"

namespace {
  constexpr const char* kName = "WARP_CamAngle";

  float g_pitch;

  void (CGameMode::*g_original)();

  struct GameModeHook : CGameMode {
    void InitializeCamera() {
      (this->*g_original)();
      if (this->hasViewParameters) {
        return;
      }
      g_cameraPitch = g_pitch;
      this->camera->camera.pitch = g_pitch;
      this->camera->camera.desiredPitch = g_pitch;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_pitch = static_cast<float>(host->SettingFloat(kName, "Pitch"));
  host->Log("%s: pitch %.0f", kName, g_pitch);
  return ro::Hook(host, &CGameMode::InitializeCamera, &GameModeHook::InitializeCamera, g_original, kName);
}
