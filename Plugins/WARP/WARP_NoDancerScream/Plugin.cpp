// Removes the Scream chat line and caster animation, as the mod's emptied MM_SONG_TALK path and DC_SCREAM case do.
// CRagEffect::NotifyGameMode sends MM_SONG_TALK with value 0 for Scream only, so that value is dropped there.
// CGameMode::ReceiveSkillUse's DC_SCREAM case is inside a large function whose head must still run: the detour marks
// a DC_SCREAM use by its caster's account id, and the callees it reaches (SetFrameSequence with the case's
// exact arguments, LaunchEffect(EF_TALK_SCREAM)) are skipped for that caster while the mark is set.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Contents/Actors/RenderObject.h"
#include "Contents/Effects/Core/EffectActor.h"
#include "Contents/Effects/Core/EffectTypes.h"
#include "Contents/Effects/Core/RagEffect.h"
#include "Contents/Skills/SkillId.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "WARP_NoDancerScream";

  int g_skillUseDepth;
  std::uint32_t g_sourceAid;

  void (CRagEffect::*g_originalNotifyGameMode)(char);
  void (CGameMode::*g_originalReceiveSkillUse)(std::uint16_t, int, std::uint32_t, std::uint32_t, bool);
  void (CRenderObject::*g_originalSetFrameSequence)(
    unsigned char, unsigned char, unsigned char, unsigned char, unsigned char, int, std::uint32_t, int
  );
  CRagEffect* (EffectActor::*g_originalLaunchEffect)(int, vector3d, float);

  struct RagEffectHook : CRagEffect {
    void NotifyGameMode(char value) {
      if (value != 0) {
        (this->*g_originalNotifyGameMode)(value);
      }
    }
  };

  struct GameModeHook : CGameMode {
    void ReceiveSkillUse(std::uint16_t skillId, int level, std::uint32_t sourceAid, std::uint32_t targetAid, bool result) {
      if (skillId != DC_SCREAM) {
        (this->*g_originalReceiveSkillUse)(skillId, level, sourceAid, targetAid, result);
        return;
      }
      const std::uint32_t savedAid = g_sourceAid;
      g_sourceAid = sourceAid;
      ++g_skillUseDepth;
      (this->*g_originalReceiveSkillUse)(skillId, level, sourceAid, targetAid, result);
      --g_skillUseDepth;
      g_sourceAid = savedAid;
    }
  };

  struct RenderObjectHook : CRenderObject {
    void SetFrameSequence(
      unsigned char animation, unsigned char firstFrame, unsigned char lastFrame, unsigned char finishAnimation,
      unsigned char finishFrame, int timed, std::uint32_t duration, int loop
    ) {
      if (g_skillUseDepth != 0 && this->render.accountId == g_sourceAid && animation == 0x60 && firstFrame == 0 && lastFrame == 0 &&
          finishAnimation == 0x60 && finishFrame == 0 && timed == 1 && duration == 1200 && loop == 0) {
        return;
      }
      (this->*g_originalSetFrameSequence)(animation, firstFrame, lastFrame, finishAnimation, finishFrame, timed, duration, loop);
    }
  };

  struct EffectActorHook : EffectActor {
    CRagEffect* LaunchEffect(int effectId, vector3d deltaPos, float rotation) {
      if (g_skillUseDepth != 0 && effectId == EF_TALK_SCREAM && this->render.accountId == g_sourceAid) {
        return nullptr;
      }
      return (this->*g_originalLaunchEffect)(effectId, deltaPos, rotation);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  CRagEffect* (EffectActor::*launchEffect)(int, vector3d, float) = &EffectActor::LaunchEffect;
  return ro::Hook(host, &CRagEffect::NotifyGameMode, &RagEffectHook::NotifyGameMode, g_originalNotifyGameMode, kName) &&
         ro::Hook(host, &CGameMode::ReceiveSkillUse, &GameModeHook::ReceiveSkillUse, g_originalReceiveSkillUse, kName) &&
         ro::Hook(host, &CRenderObject::SetFrameSequence, &RenderObjectHook::SetFrameSequence, g_originalSetFrameSequence, kName) &&
         ro::Hook(host, launchEffect, &EffectActorHook::LaunchEffect, g_originalLaunchEffect, kName);
}
