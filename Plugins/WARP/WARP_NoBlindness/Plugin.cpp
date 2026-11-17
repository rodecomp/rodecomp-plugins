// Hooks the virtual CGameActor::SendMsg (CPc and CPlayer forward AM_STATUS_CHANGE to it): EFST_HANDICAPSTATE_DEEPBLIND
// runs the mod's replacement case; every other message goes to the original. CPlayer::ReapplySkillEffects is ~650
// lines, so it is not copied: while it runs, the EF_BLIND launch (only in its Deep Blind case) is skipped, and when
// one was skipped g_overrideColor is cleared after it, as the mod's case does.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Actors/ActorEnums.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Actors/Pc.h"
#include "Contents/Effects/Core/EffectActor.h"
#include "Contents/Effects/Core/EffectTypes.h"
#include "Contents/Effects/Primitives/EffectPrim.h"
#include "Utility/Math/Vector3d.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "WARP_NoBlindness";

  bool g_reapplying;
  bool g_blindSkipped;

  void (CGameActor::*g_originalSendMsg)(CGameObject*, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t);
  void (CPlayer::*g_originalReapplySkillEffects)();
  CRagEffect* (EffectActor::*g_originalLaunchEffect)(int, vector3d, float);

  struct GameActorHook : CGameActor {
    void SendMsgHook(CGameObject* sender, std::int64_t message, std::int64_t b, std::int64_t c, std::int64_t d, std::int64_t e, std::int64_t f, std::int64_t g) {
      if (message != AM_STATUS_CHANGE || b != EFST_HANDICAPSTATE_DEEPBLIND) {
        (this->*g_originalSendMsg)(sender, message, b, c, d, e, f, g);
        return;
      }
      const bool on = c != 0;
      if (this->render.accountId == g_session.m_aid) {
        if (this->HasId(EF_BLIND) == true) {
          this->SendMsg(nullptr, AM_DELETE_EFFECT, EF_BLIND, 0, 0, 0, 0, 0);
        }
        g_overrideColor = 0;
        this->SetInstantValue(0x10, on);
      }
    }
  };

  struct PlayerHook : CPlayer {
    void ReapplySkillEffects() {
      g_reapplying = true;
      g_blindSkipped = false;
      (this->*g_originalReapplySkillEffects)();
      g_reapplying = false;
      if (g_blindSkipped) {
        g_overrideColor = 0;
      }
    }
  };

  struct EffectActorHook : EffectActor {
    CRagEffect* LaunchEffect(int effectId, vector3d deltaPos, float rotation) {
      if (g_reapplying && effectId == EF_BLIND) {
        g_blindSkipped = true;
        return nullptr;
      }
      return (this->*g_originalLaunchEffect)(effectId, deltaPos, rotation);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  CRagEffect* (EffectActor::*launchEffect)(int, vector3d, float) = &EffectActor::LaunchEffect;
  return ro::HookVirtual(host, "?SendMsg@CGameActor@@UAEXPAVCGameObject@@_J111111@Z", &GameActorHook::SendMsgHook, g_originalSendMsg, kName) &&
         ro::Hook(host, &CPlayer::ReapplySkillEffects, &PlayerHook::ReapplySkillEffects, g_originalReapplySkillEffects, kName) &&
         ro::Hook(host, launchEffect, &EffectActorHook::LaunchEffect, g_originalLaunchEffect, kName);
}
