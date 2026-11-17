// Hooks CGameMode::InitializeCamera, the only place the map's starting camera pitch is chosen: after the original,
// a map without view parameters (indoor or outdoor) starts at Pitch from plugins\WARP_HighCamAngle.ini (default -35), which is
// where the mod's fixed pitch and its clamp to pitch +- range always end. Only pitch fields are written.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Camera/Camera.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>

namespace {
  constexpr const char* kName = "WARP_HighCamAngle";

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

  void (CGameMode::*g_original)();

  struct GameModeHook : CGameMode {
    void InitializeCamera() {
      (this->*g_original)();
      if (this->hasViewParameters) {
        return;
      }
      static const float pitch = ReadOption("Pitch", -35.0f);
      g_cameraPitch = pitch;
      this->camera->camera.pitch = pitch;
      this->camera->camera.desiredPitch = pitch;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CGameMode::InitializeCamera, &GameModeHook::InitializeCamera, g_original, kName);
}
