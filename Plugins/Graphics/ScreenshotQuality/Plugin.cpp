// JPEG screenshots go through ijl15.dll's ijlWrite (CRendererDX7::SaveJpeg and RendererDX12's screenshot), which uses
// the quality ijlInit set. The ijl15!ijlWrite detour sets IJL15Properties::quality for whole-image file writes
// (operation 8, the only one the game uses), where WARP patched the screenshot code's properties.
#include "RoPlugin.h"

#include "Resource/Bitmap/IJL15Runtime.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "ScreenshotQuality";
  constexpr int kWriteWholeImageToFile = 8;

  int g_quality;
  int(__stdcall* g_ijlWrite)(IJL15Properties*, int);

  int __stdcall IjlWriteDetour(IJL15Properties* properties, int operation) {
    if (operation == kWriteWholeImageToFile && properties != nullptr) {
      properties->quality = g_quality;
    }
    return g_ijlWrite(properties, operation);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_quality = static_cast<int>(host->SettingInt(kName, "Quality"));
  if (g_quality < 0) {
    host->Log("%s: Quality is -1, the client's quality is used", kName);
    return true;
  }
  return ro::HookSystem(host, "ijl15.dll", "ijlWrite", &IjlWriteDetour, g_ijlWrite, kName);
}
