// Blocks CView::Start and CView::SetAmplitudes, whose only callers are CGameMode::SendMsg's MM_QUAKE and
// MM_SETQUAKEINFO cases, which the mod emptied.
#include "RoPlugin.h"

#include "Contents/Camera/Camera.h"

namespace {
  constexpr const char* kName = "WARP_NoEarthquake";

  void (CView::*g_originalStart)(int, unsigned, float, float, float);
  void (CView::*g_originalSetAmplitudes)(float, float, float);

  struct ViewHook : CView {
    void Start(int, unsigned, float, float, float) {}
    void SetAmplitudes(float, float, float) {}
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CView::Start, &ViewHook::Start, g_originalStart, kName) &&
         ro::Hook(host, &CView::SetAmplitudes, &ViewHook::SetAmplitudes, g_originalSetAmplitudes, kName);
}
