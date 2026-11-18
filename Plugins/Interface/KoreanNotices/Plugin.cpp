// CRenderer::DrawOverlays shows the hourly banner on Korea when s_hourlyBannerState leaves 0; it is reset before each
// call, so the banner never fades in (the state machine only starts on the exact second the hour turns).
#include "RoPlugin.h"

#include "Render/Renderer.h"

extern char s_hourlyBannerState;
extern int s_hourlyBannerAlpha;

namespace {
  constexpr const char* kName = "KoreanNotices";

  void (CRenderer::*g_drawOverlays)();

  struct RendererHook : CRenderer {
    void DrawOverlaysDetour() {
      s_hourlyBannerState = 0;
      s_hourlyBannerAlpha = 0;
      (this->*g_drawOverlays)();
      s_hourlyBannerState = 0;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  void (CRenderer::*target)() = &CRenderer::DrawOverlays;
  return ro::Hook(host, target, &RendererHook::DrawOverlaysDetour, g_drawOverlays, kName);
}
