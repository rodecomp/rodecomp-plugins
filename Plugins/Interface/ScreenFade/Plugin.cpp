// Reimplements CMode::DrawFadeIn and CMode::FadeOutMode (ModeFade.cpp, ModeFadeOut.cpp) with the configured duration;
// the alpha still runs over 0..255 across it.
#include "RoPlugin.h"

#include "Client/Mode/CMode.h"
#include "Client/Mode/ModeFade.h"
#include "Contents/Sound/SoundMgr.h"
#include "Render/Primitives/AlphaRectangle.h"
#include "Render/Renderer.h"
#include "Resource/Bitmap/BitmapResource.h"
#include "Resource/Bitmap/TextureNames.h"
#include "Resource/ResourceMgr.h"
#include "UI/Window/UIWindowMgr.h"

#include <cstdint>
#include <timeapi.h>

namespace {
  constexpr const char* kName = "ScreenFade";

  std::uint32_t g_delay;

  void (CMode::*g_drawFadeIn)();
  std::uint32_t (CMode::*g_fadeOut)(int);

  std::uint32_t Alpha(std::uint32_t elapsed) {
    return elapsed * 255 / g_delay;
  }

  struct ModeHook : CMode {
    void DrawFadeInDetour() {
      if (this->fadeInRequested != 0) {
        g_modeFadeTick = timeGetTime();
        g_soundMgr->FadeIn();
        this->fadeInRequested = 0;
      }
      const std::uint32_t elapsed = timeGetTime() - g_modeFadeTick;
      if (elapsed < g_delay) {
        DrawAlphaRectangle(0, 0, g_renderer->viewport.width, g_renderer->viewport.height, (255 - Alpha(elapsed)) << 24);
      }
    }

    std::uint32_t FadeOutDetour(int showLogo) {
      g_soundMgr->FadeOut();
      g_modeFadeTick = timeGetTime();
      if (g_delay == 0) {
        return 0;
      }
      g_renderer->CaptureBackBuffer();
      std::uint32_t elapsed = timeGetTime() - g_modeFadeTick;
      while (elapsed < g_delay) {
        DrawAlphaRectangle(0, 0, g_renderer->viewport.width, g_renderer->viewport.height, Alpha(elapsed) << 24);
        if (showLogo != 0) {
          CBitmapRes* logo = static_cast<CBitmapRes*>(ResourceMgr::Instance().Load(Resolve("rag_logo.bmp")));
          g_windowMgr.DrawBitmapResource(g_renderer->viewport.width - 192, g_renderer->viewport.height - 122, logo);
        }
        if (g_renderer->Draw()) {
          g_renderer->Present(1);
        }
        g_renderer->RestoreBackBuffer();
        elapsed = timeGetTime() - g_modeFadeTick;
      }
      return elapsed;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_delay = static_cast<std::uint32_t>(host->SettingInt(kName, "DelayMs"));
  if (g_delay == 255) {
    return true;
  }
  return ro::Hook(host, &CMode::DrawFadeIn, &ModeHook::DrawFadeInDetour, g_drawFadeIn, kName) &&
         ro::Hook(host, &CMode::FadeOutMode, &ModeHook::FadeOutDetour, g_fadeOut, kName);
}
