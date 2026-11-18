// Two camera-angle settings:
// - Pitch: CGameMode::InitializeCamera, the only place the map's starting camera pitch is chosen: after the original,
//   a map without view parameters (indoor or outdoor) starts at the Pitch setting. Only pitch fields are written.
// - Range (WARP LowCamAngle / MediumCamAngle / HighCamAngle): those patches replace the 42.0 pitch range of
//   CGameMode::ProcessRightButtonDrag's Shift+right-drag tilt on rotatable maps. The detour saves the camera's desired
//   pitch and distance, runs the original, then redoes that tilt with Range in place of 42 (same inputs, same
//   clamps: lower bound -45 - range, upper bound range - 45 with Ctrl inside the zoom-out cap, else range - 70).
//   Hooked only when Range is not the game's 42.
#include "RoPlugin.h"

#include "Client/Input/Mouse.h"
#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"
#include "Contents/Configuration/ServiceOptions.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "CamAngle";
  constexpr float kGameRange = 42.0f;

  float g_pitch;
  float g_range;

  void (CGameMode::*g_originalInitializeCamera)();
  void (CGameMode::*g_originalProcessRightButtonDrag)();

  struct GameModeHook : CGameMode {
    void InitializeCamera() {
      (this->*g_originalInitializeCamera)();
      if (this->hasViewParameters) {
        return;
      }
      g_cameraPitch = g_pitch;
      this->camera->camera.pitch = g_pitch;
      this->camera->camera.desiredPitch = g_pitch;
    }

    void ProcessRightButtonDrag() {
      const float desiredPitch = this->camera->camera.desiredPitch;
      const float desiredDistance = this->camera->camera.desiredDistance;
      (this->*g_originalProcessRightButtonDrag)();
      if (g_mouse.buttons[1] != 2 || !this->canRotateView || this->hasViewParameters ||
          HIBYTE(GetAsyncKeyState(VK_MENU)) || !HIBYTE(GetAsyncKeyState(VK_SHIFT))) {
        return;
      }
      float pitch = desiredPitch - static_cast<float>((g_mouse.y - this->oldMouseY) / 2);
      if (pitch < -45.0f - g_range) {
        pitch = -45.0f - g_range;
      }
      const float limit = HIBYTE(GetAsyncKeyState(VK_CONTROL)) && desiredDistance <= g_serviceOptionJitterScale
                            ? g_range - 45.0f
                            : g_range - 70.0f;
      if (pitch > limit) {
        pitch = limit;
      }
      g_cameraPitch = pitch;
      this->camera->camera.desiredPitch = pitch;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_pitch = static_cast<float>(host->SettingFloat(kName, "Pitch"));
  g_range = static_cast<float>(host->SettingFloat(kName, "Range"));
  host->Log("%s: pitch %.0f, tilt range %.1f", kName, g_pitch, g_range);
  return ro::Hook(host, &CGameMode::InitializeCamera, &GameModeHook::InitializeCamera, g_originalInitializeCamera, kName) &&
         (g_range == kGameRange || ro::Hook(host, &CGameMode::ProcessRightButtonDrag, &GameModeHook::ProcessRightButtonDrag,
                                            g_originalProcessRightButtonDrag, kName));
}
