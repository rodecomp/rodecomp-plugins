// - Hourly banner (always): CRenderer::DrawOverlays shows the hourly banner on Korea when s_hourlyBannerState leaves 0;
//   it is reset before each call, so the banner never fades in (the state machine only starts on the exact second the
//   hour turns).
// - HelpMessage off: on the first map load after login, CGameMode::InitializeState on Korea sends
//   MM_PROCESS_TALK_TYPE(TT_EX_HELP) (the help lines in chat) and MM_BILLING_INFO(0) (CZ_REQ_REMAINTIME, whose reply
//   prints the remaining play time). The detour marks InitializeState; the virtual CGameMode::SendMsg detour drops
//   those two events while it runs, as WARP NoHelpMsg's jmp skips the block.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Render/Renderer.h"
#include "UI/GameSettings/TalkType.h"

extern char s_hourlyBannerState;
extern int s_hourlyBannerAlpha;

namespace {
  constexpr const char* kName = "KoreanNotices";

  bool g_inInitializeState;

  void (CRenderer::*g_drawOverlays)();
  void (CGameMode::*g_initializeState)();
  int (CGameMode::*g_sendMsg)(int, int, int, int, int);

  struct RendererHook : CRenderer {
    void DrawOverlaysDetour() {
      s_hourlyBannerState = 0;
      s_hourlyBannerAlpha = 0;
      (this->*g_drawOverlays)();
      s_hourlyBannerState = 0;
    }
  };

  struct GameModeHook : CGameMode {
    void InitializeStateDetour() {
      g_inInitializeState = true;
      (this->*g_initializeState)();
      g_inInitializeState = false;
    }

    int SendMsgDetour(int event, int argument1, int argument2, int argument3, int argument4) {
      if (g_inInitializeState && ((event == MM_PROCESS_TALK_TYPE && argument1 == TT_EX_HELP) ||
                                  (event == MM_BILLING_INFO && argument1 == 0))) {
        return 0;
      }
      return (this->*g_sendMsg)(event, argument1, argument2, argument3, argument4);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool help = host->SettingBool(kName, "HelpMessage");
  void (CRenderer::*target)() = &CRenderer::DrawOverlays;
  return ro::Hook(host, target, &RendererHook::DrawOverlaysDetour, g_drawOverlays, kName) &&
         (help || (ro::Hook(host, &CGameMode::InitializeState, &GameModeHook::InitializeStateDetour, g_initializeState, kName) &&
                   ro::HookVirtual(host, "?SendMsg@CGameMode@@UAEHHHHHH@Z", &GameModeHook::SendMsgDetour, g_sendMsg, kName)));
}
