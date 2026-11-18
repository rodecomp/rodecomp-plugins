// WARP Character group ported as hooks. Per setting:
// - NoGMSprite: CPc::LoadEquipAction / LoadEquipSprite run with render.accountId = 0, so IsMonsterJob (the <yellow>
//   list) does not pick the GM body / sword; the yellow name (CGameMode::ActorNameColor) is untouched.
// - RestoreGMWeaponTrail: the same for CPc::SetSprWeapon / SetSprWeapon2 / SetSprShield, which inline that test.
// - ShowGvGDamage: CGameActor::SendMsg(AM_MAKE_NUMBER) runs with the scene's siege / battleground flags cleared.
// - ResurrectionButton: CGameMode::HasResurrectionItem returns false (Hide) or runs with the PK / siege / event PvP
//   flags cleared (Always).
// - QuickSwitchDelay: CGameMode::SendMsg(MM_SWAP_EQUIPMENT[_BY_INDEX]) and UISwapBitmapButton::Paint see
//   m_swapEquipmentTick shifted so the game's 10 s test becomes the configured delay.
// - WalkDelay: CGameMode::ProcessGroundClick sees leftBtnClickTick shifted so its 600 ms test becomes the delay.
// - NoAutoFollow / AutoFollowDelay / AutoFollowStopDelay: CGameMode::ProcessActorPick keeps g_hoverActorGid;
//   CGameMode::Update sees g_followUpdateTick / g_followLostTick shifted against its 1000 / 10000 ms tests.
// - AllowPartyLeaderLeave: CGameMode::SendMsg(MM_REQ_LEAVE_GROUP) handles "members online, none on this map" with the
//   game's own disband prompt and leave request instead of the refusal.
// - AllowGuildInClan: MM_REQ_MAKE_GUILD / MM_REQ_JOIN_GUILD run with the clan flag (hasSp) of the actor cleared.
// - SkipFriendCheck / SkipGuildCheck: CSession::IsFriendNameDifferent / IsGuildMemberNameDifferent return 0.
// - ModelCulling: C3dActor::MarkOccluded enables occlusion on the model first, so Draw fades it.
// - OfficialPal: CSession::InitResourceNameTables runs as Korea, whose only service test there picks the palette names.
// - Intravision: CPc::Render, CNpc::Render, CPc::ApplyLayerEffects and CRenderObject::DrawSpriteEffects, which inline
//   the "status 184 (Intravision) active" test, run with status 184 set through the game's SetActiveSkillEffect184
//   (cleared after unless the server had set it), so hidden / cloaked actors are drawn as dark silhouettes.
// - ShowBGEmblem: CSession::GetSiegeMode also answers battleground maps while one of the emblem callers runs: the actor
//   entry handlers that call CGameMode::ShowGuildEmblem, CGameActor::RefreshGuildEmblemWindow / ClearSiegeNameWindow /
//   ReleaseAttachments (creation and removal of the emblem window), and UIEmblemWnd::Paint, where GetBattleFieldMode
//   also answers 0 so the emblem is drawn.
// A shifted tick is put back after the call unless the game stored a new one.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Client/Session/Session.h"
#include "Contents/Actors/ActorEnums.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Actors/Npc.h"
#include "Contents/Actors/Pc.h"
#include "Contents/Actors/RenderObject.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "Contents/Messenger/MessengerMember.h"
#include "Contents/Models/ModelActor.h"
#include "Contents/Tables/MessageTable.h"
#include "Contents/World/World.h"
#include "Network/Connection/Connection.h"
#include "Network/Packets/PacketId.h"
#include "Resource/MSGSTRINGID.h"
#include "UI/Guild/UIEmblemWnd.h"
#include "UI/Items/UISwapEquipmentWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

extern int g_hoverActorGid;
extern int g_followUpdateTick;
extern int g_followLostTick;
void SetActiveSkillEffect184(unsigned char enable);  // GameModePackets.cpp, not declared in a header

namespace {
  constexpr const char* kName = "CharacterTweaks";

  bool g_noGmSprite, g_gmWeaponTrail, g_gvgDamage, g_noAutoFollow, g_leaderLeave, g_guildInClan, g_officialPal;
  int g_emblemScope, g_emblemPaintScope;  // >0 while an emblem caller runs
  int g_resMode;  // 0 default, 1 hide, 2 always
  unsigned g_swapShift, g_walkShift, g_followShift, g_lostShift;

  // Runs `call` with `value` replaced, then restores it.
  template <class T, class F>
  auto With(T& value, T replacement, F call) {
    const T saved = value;
    value = replacement;
    struct Restore { T& v; T s; ~Restore() { v = s; } } restore{value, saved};
    return call();
  }

  // Runs `call` with `tick` moved back by `shift`; keeps a tick the call stored, else restores the old one.
  template <class T, class F>
  auto Shifted(T& tick, unsigned shift, F call) {
    const T saved = tick;
    const T shifted = static_cast<T>(saved - shift);
    tick = shifted;
    struct Restore { T& v; T s; T sh; ~Restore() { if (v == sh) v = s; } } restore{tick, saved, shifted};
    return call();
  }

  void (CPc::*g_loadEquipAction)(int, const char*);
  void (CPc::*g_loadEquipSprite)(int, const char*);
  void (CPc::*g_setSprWeapon)(int, int);
  void (CPc::*g_setSprWeapon2)(int, int);
  void (CPc::*g_setSprShield)(int);
  void (CGameActor::*g_actorSendMsg)(CGameObject*, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t);
  bool (CGameMode::*g_hasResurrectionItem)();
  int (CGameMode::*g_modeSendMsg)(int, int, int, int, int);
  int (CGameMode::*g_processGroundClick)(int);
  void (CGameMode::*g_processActorPick)(int&, ActorPickInfo*, CGameActor*);
  void (CGameMode::*g_update)();
  void (UISwapBitmapButton::*g_swapPaint)();
  int (CSession::*g_friendDifferent)(const char*);
  int (CSession::*g_guildDifferent)(const char*);
  void (CSession::*g_initResourceNameTables)();
  void (C3dActor::*g_markOccluded)(const float*);
  int (CSession::*g_getSiegeMode)() const;
  int (CSession::*g_getBattleFieldMode)() const;
  void (CPc::*g_pcRender)(int);
  void (CNpc::*g_npcRender)(int);
  void (CPc::*g_applyLayerEffects)(SpriteQuad*, const SpriteFrameCell*, int, const tagRECT*, float, float, float, float, float, float,
                                   const std::uint32_t*, const SpriteRleImage*, int*, int, int);
  void (CRenderObject::*g_drawSpriteEffects)(SpriteQuad*, const SpriteFrameCell*, const tagRECT*, float, float, float, const std::uint32_t*,
                                             const SpriteRleImage*, int*, std::uint32_t);

  constexpr int kIntravision = 184;

  // Runs `call` with Intravision (status 184) active; when it was not, the game's own SetActiveSkillEffect184 adds the
  // entry to g_activeSkillEffects and removes it after (the vector is not reachable from plugins: std data).
  template <class F>
  void WithIntravision(F call) {
    if (g_session.HasActiveSkillEffect(kIntravision)) {
      call();
      return;
    }
    SetActiveSkillEffect184(1);
    struct Remove { ~Remove() { SetActiveSkillEffect184(0); } } remove;
    call();
  }

  struct IntravisionPcHook : CPc {
    void Render(int argument) {
      WithIntravision([&] { (this->*g_pcRender)(argument); });
    }
    void ApplyLayerEffects(SpriteQuad* quad, const SpriteFrameCell* cell, int layer, const tagRECT* box, float shiftX, float distance,
                           float bottomRhw, float topRhw, float screenX, float screenY, const std::uint32_t* palette,
                           const SpriteRleImage* image, int* blend, int offsetX, int offsetY) {
      WithIntravision([&] {
        (this->*g_applyLayerEffects)(quad, cell, layer, box, shiftX, distance, bottomRhw, topRhw, screenX, screenY, palette, image, blend,
                                     offsetX, offsetY);
      });
    }
  };

  struct IntravisionNpcHook : CNpc {
    void Render(int argument) {
      WithIntravision([&] { (this->*g_npcRender)(argument); });
    }
  };

  struct IntravisionObjectHook : CRenderObject {
    void DrawSpriteEffects(SpriteQuad* quad, const SpriteFrameCell* cell, const tagRECT* box, float offset, float bottomRhw, float topRhw,
                           const std::uint32_t* palette, const SpriteRleImage* image, int* blend, std::uint32_t color) {
      WithIntravision([&] { (this->*g_drawSpriteEffects)(quad, cell, box, offset, bottomRhw, topRhw, palette, image, blend, color); });
    }
  };

  // Hooks `Target`, an emblem caller, to count itself in g_emblemScope while it runs.
  template <auto Target>
  struct EmblemCaller;

  template <class C, class R, class... A, R (C::*Target)(A...)>
  struct EmblemCaller<Target> : C {
    static inline R (C::*original)(A...);

    R Run(A... args) {
      ++g_emblemScope;
      struct Leave { ~Leave() { --g_emblemScope; } } leave;
      return (this->*original)(static_cast<A&&>(args)...);
    }
  };

  template <auto Target>
  bool HookEmblemCaller(const RoHostApi* host) {
    return ro::Hook(host, Target, &EmblemCaller<Target>::Run, EmblemCaller<Target>::original, kName);
  }

  struct EmblemSessionHook : CSession {
    int GetSiegeMode() const {
      const int siege = (this->*g_getSiegeMode)();
      return siege || (g_emblemScope > 0 && (this->*g_getBattleFieldMode)());
    }
    int GetBattleFieldMode() const {
      return g_emblemPaintScope > 0 ? 0 : (this->*g_getBattleFieldMode)();
    }
  };

  struct EmblemWndHook : UIEmblemWnd {
    void Paint();
  };
  void (UIEmblemWnd::*g_emblemPaint)();
  void EmblemWndHook::Paint() {
    ++g_emblemScope;
    ++g_emblemPaintScope;
    struct Leave { ~Leave() { --g_emblemScope; --g_emblemPaintScope; } } leave;
    (this->*g_emblemPaint)();
  }

  bool HookEmblemCallers(const RoHostApi* host) {
    return HookEmblemCaller<&CGameMode::ReceiveActorStandEntry>(host) && HookEmblemCaller<&CGameMode::ReceiveActorStandEntryFont>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorTypedEntry>(host) && HookEmblemCaller<&CGameMode::ReceiveActorMoveEntryLegacy>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorMoveEntryOld>(host) && HookEmblemCaller<&CGameMode::ReceiveActorMoveEntryOld2>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorMoveEntryNamed>(host) && HookEmblemCaller<&CGameMode::ReceiveActorMoveEntryOld0>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorStandEntryLegacy>(host) && HookEmblemCaller<&CGameMode::ReceiveActorStandEntryNamedOld>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorStandEntryOld>(host) && HookEmblemCaller<&CGameMode::ReceiveActorStandEntryOld2>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorStandEntryOld3>(host) && HookEmblemCaller<&CGameMode::ReceiveActorStandEntryOld0>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorStateEntryNamedOld>(host) && HookEmblemCaller<&CGameMode::ReceiveActorStateEntryOld>(host) &&
           HookEmblemCaller<&CGameMode::ReceiveActorStateEntryLegacy>(host) && HookEmblemCaller<&CGameMode::SpawnMovingActor>(host) &&
           HookEmblemCaller<&CGameMode::SpawnStandingActor>(host) && HookEmblemCaller<&CGameMode::SpawnStateActor>(host) &&
           HookEmblemCaller<&CGameActor::RefreshGuildEmblemWindow>(host) && HookEmblemCaller<&CGameActor::ClearSiegeNameWindow>(host) &&
           HookEmblemCaller<&CGameActor::ReleaseAttachments>(host) &&
           ro::HookVirtual(host, "?Paint@UIEmblemWnd@@UAEXXZ", &EmblemWndHook::Paint, g_emblemPaint, kName) &&
           ro::Hook(host, &CSession::GetSiegeMode, &EmblemSessionHook::GetSiegeMode, g_getSiegeMode, kName) &&
           ro::Hook(host, &CSession::GetBattleFieldMode, &EmblemSessionHook::GetBattleFieldMode, g_getBattleFieldMode, kName);
  }

  struct PcHook : CPc {
    void LoadEquipAction(int slot, const char* name) {
      With(this->render.accountId, 0u, [&] { (this->*g_loadEquipAction)(slot, name); });
    }
    void LoadEquipSprite(int slot, const char* name) {
      With(this->render.accountId, 0u, [&] { (this->*g_loadEquipSprite)(slot, name); });
    }
    void SetSprWeapon(int first, int second) {
      With(this->render.accountId, 0u, [&] { (this->*g_setSprWeapon)(first, second); });
    }
    void SetSprWeapon2(int first, int second) {
      With(this->render.accountId, 0u, [&] { (this->*g_setSprWeapon2)(first, second); });
    }
    void SetSprShield(int value) {
      With(this->render.accountId, 0u, [&] { (this->*g_setSprShield)(value); });
    }
  };

  struct ActorHook : CGameActor {
    void SendMsg(CGameObject* sender, std::int64_t message, std::int64_t b, std::int64_t c, std::int64_t d, std::int64_t e, std::int64_t f, std::int64_t g) {
      CGameMode* mode = g_modeMgr.GetGameMode();
      if (message != AM_MAKE_NUMBER || mode == nullptr || mode->scene == nullptr) {
        (this->*g_actorSendMsg)(sender, message, b, c, d, e, f, g);
        return;
      }
      CWorld* world = mode->scene;
      With(world->isSiegeMode, 0, [&] {
        With(world->isBattleFieldMode, 0, [&] { (this->*g_actorSendMsg)(sender, message, b, c, d, e, f, g); });
      });
    }
  };

  // MM_REQ_LEAVE_GROUP as the game does it, with "online members, none on this map" sent to the disband prompt.
  bool LeaderLeave(CGameMode* mode) {
    const int count = g_session.PartyCount();
    if (g_session.partyLeader == 0 || count <= 1) {
      return false;
    }
    bool online = false;
    for (int index = 0; index < count; ++index) {
      const MessengerMember member = g_session.PartyAt(index);
      if (member.accountId != g_session.m_aid && member.offline == 0) {
        if (member.map == mode->mapName) {
          return false;
        }
        online = true;
      }
    }
    if (!online) {
      return false;
    }
    if (g_session.memorialDungeonState == 1 || g_session.memorialDungeonState == 2) {
      g_windowMgr.SendNotice(UIM_PUSHINTOCHATHISTORY, reinterpret_cast<std::intptr_t>(LookupMessage(MSI_MUST_PASSWORD_EXCEED_CHAR)), 0xFF, 0, 0, 0);
    }
    if (g_windowMgr.Confirm(LookupMessage(MSI_SERVER_PROPERTY_FREE_NO_COUNT), 2, false, false, 0, "Message", 280, 120, 0) != 0xBB) {
      return true;
    }
    if (g_windowMgr.partyInvitationToWnd != nullptr) {
      g_windowMgr.CloseWindow(WID_PARTYINVITATIONTOWND);
    }
    const std::uint16_t packet = ID_CZ_REQ_LEAVE_GROUP;
    GetRagConnection().Send(GetRagConnection().GetPacketSize(packet), &packet);
    return true;
  }

  struct ModeHook : CGameMode {
    bool HasResurrectionItem() {
      if (g_resMode == 1) {
        return false;
      }
      if (g_resMode == 0 || this->scene == nullptr) {
        return (this->*g_hasResurrectionItem)();
      }
      CWorld* world = this->scene;
      return With(world->isPKZone, 0, [&] {
        return With(world->isSiegeMode, 0, [&] {
          return With(world->isEventPVPMode, 0, [&] { return (this->*g_hasResurrectionItem)(); });
        });
      });
    }

    int SendMsg(int event, int a1, int a2, int a3, int a4) {
      auto original = [&] { return (this->*g_modeSendMsg)(event, a1, a2, a3, a4); };
      switch (event) {
      case MM_SWAP_EQUIPMENT:
      case MM_SWAP_EQUIPMENT_BY_INDEX:
        return g_swapShift ? Shifted(g_session.m_swapEquipmentTick, g_swapShift, original) : original();
      case MM_REQ_LEAVE_GROUP:
        return g_leaderLeave && LeaderLeave(this) ? 0 : original();
      case MM_REQ_MAKE_GUILD:
        if (g_guildInClan && this->scene && this->scene->player) {
          return With(this->scene->player->hasSp, static_cast<unsigned char>(0), original);
        }
        break;
      case MM_REQ_JOIN_GUILD:
        if (g_guildInClan && this->scene) {
          if (CGameActor* actor = this->scene->GetGameActorByAID(static_cast<std::uint32_t>(a1))) {
            return With(actor->hasSp, static_cast<unsigned char>(0), original);
          }
        }
        break;
      }
      return original();
    }

    int ProcessGroundClick(int result) {
      return Shifted(this->leftBtnClickTick, g_walkShift, [&] { return (this->*g_processGroundClick)(result); });
    }

    void ProcessActorPick(int& handled, ActorPickInfo* pick, CGameActor* actor) {
      const int saved = g_hoverActorGid;
      (this->*g_processActorPick)(handled, pick, actor);
      g_hoverActorGid = saved;
    }

    void Update() {
      Shifted(g_followUpdateTick, g_followShift, [&] {
        if (g_followLostTick == 0) {
          (this->*g_update)();
        } else {
          Shifted(g_followLostTick, g_lostShift, [&] { (this->*g_update)(); });
        }
      });
    }
  };

  struct SwapButtonHook : UISwapBitmapButton {
    void Paint() {
      Shifted(g_session.m_swapEquipmentTick, g_swapShift, [&] { (this->*g_swapPaint)(); });
    }
  };

  struct SessionHook : CSession {
    int FriendDifferent(const char*) { return 0; }
    int GuildDifferent(const char*) { return 0; }
    void InitResourceNameTables() {
      With(g_serviceType, static_cast<int>(ServiceKorea), [&] { (this->*g_initResourceNameTables)(); });
    }
  };

  struct ModelHook : C3dActor {
    void MarkOccluded(const float* ray) {
      this->occlusionEnabled = 1;
      (this->*g_markOccluded)(ray);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_noGmSprite = host->SettingBool(kName, "NoGMSprite");
  g_gmWeaponTrail = g_noGmSprite && host->SettingBool(kName, "RestoreGMWeaponTrail");
  g_gvgDamage = host->SettingBool(kName, "ShowGvGDamage");
  const std::string res = host->SettingString(kName, "ResurrectionButton");
  g_resMode = res == "Hide" ? 1 : res == "Always" ? 2 : 0;
  g_swapShift = 10000u - static_cast<unsigned>(host->SettingInt(kName, "QuickSwitchDelay")) * 1000u;
  g_walkShift = 600u - static_cast<unsigned>(host->SettingInt(kName, "WalkDelay"));
  g_noAutoFollow = host->SettingBool(kName, "NoAutoFollow");
  g_followShift = 1000u - static_cast<unsigned>(host->SettingInt(kName, "AutoFollowDelay"));
  g_lostShift = 10000u - static_cast<unsigned>(host->SettingInt(kName, "AutoFollowStopDelay"));
  g_leaderLeave = host->SettingBool(kName, "AllowPartyLeaderLeave");
  g_guildInClan = host->SettingBool(kName, "AllowGuildInClan");
  const bool skipFriend = host->SettingBool(kName, "SkipFriendCheck");
  const bool skipGuild = host->SettingBool(kName, "SkipGuildCheck");
  const bool culling = host->SettingBool(kName, "ModelCulling");
  g_officialPal = host->SettingBool(kName, "OfficialPal");
  const bool intravision = host->SettingBool(kName, "Intravision");
  const bool bgEmblem = host->SettingBool(kName, "ShowBGEmblem");

  bool ok = true;
  if (g_noGmSprite) {
    ok = ok && ro::Hook(host, &CPc::LoadEquipAction, &PcHook::LoadEquipAction, g_loadEquipAction, kName) &&
         ro::Hook(host, &CPc::LoadEquipSprite, &PcHook::LoadEquipSprite, g_loadEquipSprite, kName);
  }
  if (g_gmWeaponTrail) {
    ok = ok && ro::HookVirtual(host, "?SetSprWeapon@CPc@@UAEXHH@Z", &PcHook::SetSprWeapon, g_setSprWeapon, kName) &&
         ro::HookVirtual(host, "?SetSprWeapon2@CPc@@UAEXHH@Z", &PcHook::SetSprWeapon2, g_setSprWeapon2, kName) &&
         ro::HookVirtual(host, "?SetSprShield@CPc@@UAEXH@Z", &PcHook::SetSprShield, g_setSprShield, kName);
  }
  if (g_gvgDamage) {
    ok = ok && ro::HookVirtual(host, "?SendMsg@CGameActor@@UAEXPAVCGameObject@@_J111111@Z", &ActorHook::SendMsg, g_actorSendMsg, kName);
  }
  if (g_resMode != 0) {
    ok = ok && ro::Hook(host, &CGameMode::HasResurrectionItem, &ModeHook::HasResurrectionItem, g_hasResurrectionItem, kName);
  }
  if (g_swapShift || g_leaderLeave || g_guildInClan) {
    ok = ok && ro::HookVirtual(host, "?SendMsg@CGameMode@@UAEHHHHHH@Z", &ModeHook::SendMsg, g_modeSendMsg, kName);
  }
  if (g_swapShift) {
    ok = ok && ro::HookVirtual(host, "?Paint@UISwapBitmapButton@@UAEXXZ", &SwapButtonHook::Paint, g_swapPaint, kName);
  }
  if (g_walkShift) {
    ok = ok && ro::Hook(host, &CGameMode::ProcessGroundClick, &ModeHook::ProcessGroundClick, g_processGroundClick, kName);
  }
  if (g_noAutoFollow) {
    ok = ok && ro::Hook(host, &CGameMode::ProcessActorPick, &ModeHook::ProcessActorPick, g_processActorPick, kName);
  }
  if (g_followShift || g_lostShift) {
    ok = ok && ro::HookVirtual(host, "?Update@CGameMode@@UAEXXZ", &ModeHook::Update, g_update, kName);
  }
  if (skipFriend) {
    ok = ok && ro::Hook(host, &CSession::IsFriendNameDifferent, &SessionHook::FriendDifferent, g_friendDifferent, kName);
  }
  if (skipGuild) {
    ok = ok && ro::Hook(host, &CSession::IsGuildMemberNameDifferent, &SessionHook::GuildDifferent, g_guildDifferent, kName);
  }
  if (culling) {
    ok = ok && ro::HookVirtual(host, "?MarkOccluded@C3dActor@@UAEXPBM@Z", &ModelHook::MarkOccluded, g_markOccluded, kName);
  }
  if (g_officialPal) {
    ok = ok && ro::Hook(host, &CSession::InitResourceNameTables, &SessionHook::InitResourceNameTables, g_initResourceNameTables, kName);
  }
  if (intravision) {
    ok = ok && ro::HookVirtual(host, "?Render@CPc@@UAEXH@Z", &IntravisionPcHook::Render, g_pcRender, kName) &&
         ro::HookVirtual(host, "?Render@CNpc@@UAEXH@Z", &IntravisionNpcHook::Render, g_npcRender, kName) &&
         ro::Hook(host, &CPc::ApplyLayerEffects, &IntravisionPcHook::ApplyLayerEffects, g_applyLayerEffects, kName) &&
         ro::Hook(host, &CRenderObject::DrawSpriteEffects, &IntravisionObjectHook::DrawSpriteEffects, g_drawSpriteEffects, kName);
  }
  if (bgEmblem) {
    ok = ok && HookEmblemCallers(host);
  }
  return ok;
}
