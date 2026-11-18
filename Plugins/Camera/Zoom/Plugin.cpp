// Sets the outdoor camera's maximum distance while the game's zoom-out option (/zoom, TT_EXPAND_SIGHT_ON_OFF) is on,
// the cap the game keeps in g_serviceOptionJitterScale (730 with the option on, 400 with it off). Like the WARP
// IncrZoom patches, which repoint only the option-on cap, the option-off cap is left alone.
// CGameMode::InitializeCamera: after the original, an outdoor map without view parameters re-clamps
// g_outdoorCameraDistance to [minimum, Distance] (only distance fields are written).
// CGameMode::ProcessMouseWheel / ProcessRightButtonDrag (wheel and Ctrl+right-drag zoom): the cap is set to Distance
// around the original and restored after, so the Shift+Ctrl pitch-limit check in both also uses it. A Distance below
// the game's minimum (230, WARP ZoomIn25Percent's 128) lowers g_cameraMinDistance the same way.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"
#include "Contents/Configuration/ServiceOptions.h"
#include "UI/GameSettings/TalkType.h"

namespace {
  constexpr const char* kName = "Zoom";

  float g_maxDistance;

  void (CGameMode::*g_originalInitializeCamera)();
  void (CGameMode::*g_originalProcessMouseWheel)(int);
  void (CGameMode::*g_originalProcessRightButtonDrag)();

  // Sets the caps for one call of the original while the zoom-out option is on.
  class ScopedCaps {
  public:
    ScopedCaps() : active_(ServiceOptions::Get(TT_EXPAND_SIGHT_ON_OFF)), cap_(g_serviceOptionJitterScale), min_(g_cameraMinDistance) {
      if (active_) {
        g_serviceOptionJitterScale = g_maxDistance;
        if (g_cameraMinDistance > g_maxDistance) {
          g_cameraMinDistance = g_maxDistance;
        }
      }
    }
    ~ScopedCaps() {
      if (active_) {
        g_serviceOptionJitterScale = cap_;
        g_cameraMinDistance = min_;
      }
    }
    ScopedCaps(const ScopedCaps&) = delete;
    ScopedCaps& operator=(const ScopedCaps&) = delete;

  private:
    bool active_;
    float cap_;
    float min_;
  };

  struct GameModeHook : CGameMode {
    void InitializeCamera() {
      (this->*g_originalInitializeCamera)();
      if (!this->canRotateView || this->hasViewParameters || !ServiceOptions::Get(TT_EXPAND_SIGHT_ON_OFF)) {
        return;
      }
      float distance = g_outdoorCameraDistance;
      const float minimum = g_cameraMinDistance < g_maxDistance ? g_cameraMinDistance : g_maxDistance;
      if (minimum > distance) {
        distance = minimum;
      }
      if (distance > g_maxDistance) {
        distance = g_maxDistance;
      }
      g_cameraDistance = distance;
      this->camera->camera.distance = distance;
      this->camera->camera.desiredDistance = distance;
    }

    void ProcessMouseWheel(int requireControl) {
      ScopedCaps caps;
      (this->*g_originalProcessMouseWheel)(requireControl);
    }

    void ProcessRightButtonDrag() {
      ScopedCaps caps;
      (this->*g_originalProcessRightButtonDrag)();
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_maxDistance = static_cast<float>(host->SettingFloat(kName, "Distance"));
  host->Log("%s: maximum distance %.1f with the zoom-out option on", kName, g_maxDistance);
  return ro::Hook(host, &CGameMode::InitializeCamera, &GameModeHook::InitializeCamera, g_originalInitializeCamera, kName) &&
         ro::Hook(host, &CGameMode::ProcessMouseWheel, &GameModeHook::ProcessMouseWheel, g_originalProcessMouseWheel, kName) &&
         ro::Hook(host, &CGameMode::ProcessRightButtonDrag, &GameModeHook::ProcessRightButtonDrag, g_originalProcessRightButtonDrag, kName);
}
