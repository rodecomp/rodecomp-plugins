// Resizes the HP bars drawn over actors; a hook is installed only when a size differs from the game's.
// - Player (WARP ResizePlayerBar): CGameMode::SendMsg creates and refreshes the own HP/SP gauge (CGameMode::itemWindow)
//   with SetSize(60, 9), or (60, 13) with the extra trait bar, in its MM_TOGGLE_PLAYERGAGE / MM_REFRESH_PLAYERGAGE
//   cases. After the original the gauge is resized to Width x (Height, plus 4 with the trait bar). The game places
//   it at screenX - 30 there and every frame in CGameMode::UpdateBalloons, so after both the gauge is shifted to be
//   centred on its new width (WARP changes that 30 to half the width).
// - Monsters (WARP ResizeNormalBar / ResizeMiniBossBar / ResizeBossBar): CGameActor::SendMsg's AM_UPDATE_MONSTER_GAGE
//   creates the gauge with SetSize(60, 5). After the original the gauge gets the size of the actor's hpGaugeFlags
//   (0 normal, 1 mini-boss, 2 boss), checked on every update since the flag can arrive later, and is re-centred; the
//   game centres it on its width every frame after that. Monster bars fill the gauge; the player gauge draws its
//   HP/SP rows 3 pixels high at fixed offsets, so its height only adds border below them, as with WARP.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Session/Session.h"
#include "Contents/Actors/ActorEnums.h"
#include "Contents/Actors/GameActor.h"
#include "UI/Gauges/UIPcGage.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "HealthBars";
  constexpr int kGameHalfWidth = 30;

  struct Size {
    int width;
    int height;
  };

  Size g_player;
  Size g_monsters[3];  // by hpGaugeFlags: normal, mini-boss, boss

  int (CGameMode::*g_originalGameModeSendMsg)(int, int, int, int, int);
  void (CGameMode::*g_originalUpdateBalloons)();
  void (CGameActor::*g_originalActorSendMsg)(CGameObject*, std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                                             std::int64_t, std::int64_t, std::int64_t);

  void Resize(UIWindow* gauge, int width, int height) {
    if (gauge->geometry.width != width || gauge->geometry.height != height) {
      gauge->SetSize(width, height);
    }
  }

  // Moves the player gauge from the game's screenX - 30 to screenX - width / 2.
  void CentrePlayerGauge(UIWindow* gauge) {
    gauge->Move(gauge->geometry.x + kGameHalfWidth - gauge->geometry.width / 2, gauge->geometry.y);
  }

  struct GameModeHook : CGameMode {
    int SendMsgDetour(int event, int argument1, int argument2, int argument3, int argument4) {
      const int result = (this->*g_originalGameModeSendMsg)(event, argument1, argument2, argument3, argument4);
      if ((event == MM_TOGGLE_PLAYERGAGE || event == MM_REFRESH_PLAYERGAGE) && this->itemWindow) {
        UIPlayerGage* gauge = this->itemWindow;
        // The game's height is 9, or 13 with the trait bar (same test as the game's cases).
        const int extra = g_session.IsTreeGroupThree(g_session.GetDisplayJob()) ? 4 : 0;
        Resize(gauge, g_player.width, g_player.height + extra);
        CentrePlayerGauge(gauge);
      }
      return result;
    }

    void UpdateBalloons() {
      (this->*g_originalUpdateBalloons)();
      if (this->itemWindow) {
        CentrePlayerGauge(this->itemWindow);
      }
    }
  };

  struct GameActorHook : CGameActor {
    void SendMsgDetour(CGameObject* sender, std::int64_t message, std::int64_t b, std::int64_t c, std::int64_t d,
                       std::int64_t e, std::int64_t f, std::int64_t g) {
      (this->*g_originalActorSendMsg)(sender, message, b, c, d, e, f, g);
      if (message != AM_UPDATE_MONSTER_GAGE || !this->hpGaugeWindow) {
        return;
      }
      const unsigned kind = this->hpGaugeFlags < 3 ? this->hpGaugeFlags : 0;
      UIMonsterGage* gauge = this->hpGaugeWindow;
      const int oldWidth = gauge->geometry.width;
      Resize(gauge, g_monsters[kind].width, g_monsters[kind].height);
      if (gauge->geometry.width != oldWidth) {
        gauge->Move(gauge->geometry.x + oldWidth / 2 - gauge->geometry.width / 2, gauge->geometry.y);
      }
    }
  };

  Size ReadSize(const RoHostApi* host, const char* width, const char* height) {
    return {static_cast<int>(host->SettingInt(kName, width)), static_cast<int>(host->SettingInt(kName, height))};
  }

  bool IsGameSize(const Size& size, int height) {
    return size.width == 60 && size.height == height;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_player = ReadSize(host, "PlayerWidth", "PlayerHeight");
  g_monsters[0] = ReadSize(host, "NormalWidth", "NormalHeight");
  g_monsters[1] = ReadSize(host, "MiniBossWidth", "MiniBossHeight");
  g_monsters[2] = ReadSize(host, "BossWidth", "BossHeight");
  const bool player = !IsGameSize(g_player, 9);
  const bool monsters = !IsGameSize(g_monsters[0], 5) || !IsGameSize(g_monsters[1], 5) || !IsGameSize(g_monsters[2], 5);
  host->Log("%s: player %dx%d, normal %dx%d, mini-boss %dx%d, boss %dx%d", kName, g_player.width, g_player.height,
            g_monsters[0].width, g_monsters[0].height, g_monsters[1].width, g_monsters[1].height, g_monsters[2].width,
            g_monsters[2].height);
  return (!player ||
          (ro::HookVirtual(host, "?SendMsg@CGameMode@@UAEHHHHHH@Z", &GameModeHook::SendMsgDetour, g_originalGameModeSendMsg, kName) &&
           ro::Hook(host, &CGameMode::UpdateBalloons, &GameModeHook::UpdateBalloons, g_originalUpdateBalloons, kName))) &&
         (!monsters || ro::HookVirtual(host, "?SendMsg@CGameActor@@UAEXPAVCGameObject@@_J111111@Z",
                                       &GameActorHook::SendMsgDetour, g_originalActorSendMsg, kName));
}
