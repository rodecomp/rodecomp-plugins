// Sets the outdoor camera's maximum distance to MaxDistance from plugins\WARP_ZoomMax.ini (default 1224), where the game uses
// g_serviceOptionJitterScale. CGameMode::InitializeCamera: after the original, an outdoor map without view parameters
// re-clamps g_outdoorCameraDistance to [g_cameraMinDistance, MaxDistance] (only distance fields are written).
// CGameMode::ProcessMouseWheel / ProcessRightButtonDrag (wheel and Ctrl+right-drag zoom): the global is set to
// MaxDistance around the original and restored after, so the Shift+Ctrl pitch-limit check in both also uses it.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"
#include "Contents/Configuration/ServiceOptions.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>

namespace {
  constexpr const char* kName = "WARP_ZoomMax";

  float ReadOption(const char* key, float fallback) {
    char path[MAX_PATH];
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&ReadOption), &module);
    DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    char* slash = length && length < MAX_PATH ? strrchr(path, '\\') : nullptr;
    if (!slash || slash - path + 1 + strlen(kName) + 4 >= MAX_PATH) {
      return fallback;
    }
    strcpy(slash + 1, kName);
    strcat(slash + 1, ".ini");
    char text[64];
    GetPrivateProfileStringA(kName, key, "", text, sizeof(text), path);
    return text[0] ? static_cast<float>(atof(text)) : fallback;
  }

  float MaxDistance() {
    static const float distance = ReadOption("MaxDistance", 1224.0f);
    return distance;
  }

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
      if (distance > MaxDistance()) {
        distance = MaxDistance();
      }
      g_cameraDistance = distance;
      this->camera->camera.distance = distance;
      this->camera->camera.desiredDistance = distance;
    }

    void ProcessMouseWheel(int requireControl) {
      const float saved = g_serviceOptionJitterScale;
      g_serviceOptionJitterScale = MaxDistance();
      (this->*g_originalProcessMouseWheel)(requireControl);
      g_serviceOptionJitterScale = saved;
    }

    void ProcessRightButtonDrag() {
      const float saved = g_serviceOptionJitterScale;
      g_serviceOptionJitterScale = MaxDistance();
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
