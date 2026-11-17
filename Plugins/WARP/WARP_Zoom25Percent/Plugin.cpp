// Sets the outdoor camera's maximum distance to 128, where the game uses
// g_serviceOptionJitterScale. CGameMode::InitializeCamera: after the original, an outdoor map without view parameters
// re-clamps g_outdoorCameraDistance to [g_cameraMinDistance, 128] (only distance fields are written).
// CGameMode::ProcessMouseWheel / ProcessRightButtonDrag (wheel and Ctrl+right-drag zoom): the global is set to
// that distance around the original and restored after, so the Shift+Ctrl pitch-limit check in both also uses it.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"
#include "Contents/Configuration/ServiceOptions.h"

namespace {
  constexpr const char* kName = "WARP_Zoom25Percent";

  constexpr float kMaxDistance = 128.0f;

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
      if (distance > kMaxDistance) {
        distance = kMaxDistance;
      }
      g_cameraDistance = distance;
      this->camera->camera.distance = distance;
      this->camera->camera.desiredDistance = distance;
    }

    void ProcessMouseWheel(int requireControl) {
      const float saved = g_serviceOptionJitterScale;
      g_serviceOptionJitterScale = kMaxDistance;
      (this->*g_originalProcessMouseWheel)(requireControl);
      g_serviceOptionJitterScale = saved;
    }

    void ProcessRightButtonDrag() {
      const float saved = g_serviceOptionJitterScale;
      g_serviceOptionJitterScale = kMaxDistance;
      (this->*g_originalProcessRightButtonDrag)();
      g_serviceOptionJitterScale = saved;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CGameMode::InitializeCamera, &GameModeHook::InitializeCamera, g_originalInitializeCamera, kName) &&
         ro::Hook(host, &CGameMode::ProcessMouseWheel, &GameModeHook::ProcessMouseWheel, g_originalProcessMouseWheel, kName) &&
         ro::Hook(host, &CGameMode::ProcessRightButtonDrag, &GameModeHook::ProcessRightButtonDrag, g_originalProcessRightButtonDrag, kName);
}
