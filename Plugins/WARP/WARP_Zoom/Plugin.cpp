// Sets the outdoor camera's maximum distance, where the game uses g_serviceOptionJitterScale, from the Zoom setting:
// 0% is the game's default (400), 100% the farthest of the WARP presets (1224), linear in between.
// CGameMode::InitializeCamera: after the original, an outdoor map without view parameters re-clamps
// g_outdoorCameraDistance to [g_cameraMinDistance, that distance] (only distance fields are written).
// CGameMode::ProcessMouseWheel / ProcessRightButtonDrag (wheel and Ctrl+right-drag zoom): the global is set to
// that distance around the original and restored after, so the Shift+Ctrl pitch-limit check in both also uses it.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"
#include "Contents/Configuration/ServiceOptions.h"

namespace {
  constexpr const char* kName = "WARP_Zoom";
  constexpr float kDefaultDistance = 400.0f;
  constexpr float kFarthestDistance = 1224.0f;

  float g_maxDistance = kFarthestDistance;

  void (CGameMode::*g_originalInitializeCamera)();
  void (CGameMode::*g_originalProcessMouseWheel)(int);
  void (CGameMode::*g_originalProcessRightButtonDrag)();

  struct GameModeHook : CGameMode {
    void InitializeCamera() {
      (this->*g_originalInitializeCamera)();
      if (!this->canRotateView || this->hasViewParameters) {
        return;
      }
      float distance = g_outdoorCameraDistance;
      if (g_cameraMinDistance > distance) {
        distance = g_cameraMinDistance;
      }
      if (distance > g_maxDistance) {
        distance = g_maxDistance;
      }
      g_cameraDistance = distance;
      this->camera->camera.distance = distance;
      this->camera->camera.desiredDistance = distance;
    }

    void ProcessMouseWheel(int requireControl) {
      const float saved = g_serviceOptionJitterScale;
      g_serviceOptionJitterScale = g_maxDistance;
      (this->*g_originalProcessMouseWheel)(requireControl);
      g_serviceOptionJitterScale = saved;
    }

    void ProcessRightButtonDrag() {
      const float saved = g_serviceOptionJitterScale;
      g_serviceOptionJitterScale = g_maxDistance;
      (this->*g_originalProcessRightButtonDrag)();
      g_serviceOptionJitterScale = saved;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const double percent = host->SettingFloat(kName, "Zoom");
  g_maxDistance = kDefaultDistance + static_cast<float>(percent / 100.0) * (kFarthestDistance - kDefaultDistance);
  host->Log("%s: %.0f%%, maximum distance %.0f", kName, percent, g_maxDistance);
  return ro::Hook(host, &CGameMode::InitializeCamera, &GameModeHook::InitializeCamera, g_originalInitializeCamera, kName) &&
         ro::Hook(host, &CGameMode::ProcessMouseWheel, &GameModeHook::ProcessMouseWheel, g_originalProcessMouseWheel, kName) &&
         ro::Hook(host, &CGameMode::ProcessRightButtonDrag, &GameModeHook::ProcessRightButtonDrag, g_originalProcessRightButtonDrag, kName);
}
