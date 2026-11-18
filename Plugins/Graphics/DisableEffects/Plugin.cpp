// Disables four effects and restores one, one bool setting each; a hook is installed only when a switch on needs it.
// - RestoreSongs: CSkill::OnProcess has no case for the Bard/Dancer song units 158-175, so their ground effect is
//   never launched. After the original, a song unit without an effect launches its EF_BOTTOM_* effect and keeps it
//   alive exactly as the Land Protector case does (WARP's patch routes those units into that case with the song's id).
// - FrostJoke / Scream: removes the chat line and caster animation, as the mod's emptied MM_SONG_TALK path and
//   BA_FROSTJOKE / DC_SCREAM cases do. CRagEffect::NotifyGameMode sends MM_SONG_TALK with value 1 for Frost Joke only
//   and value 0 for Scream only, so those values are dropped there. CGameMode::ReceiveSkillUse's cases are inside a
//   large function whose head must still run: the detour marks a use of the skill by its caster's account id, and the
//   callees the case reaches are skipped for that caster while the mark is set: SetFrameSequence with the case's exact
//   arguments, LaunchEffect(EF_TALK_FROSTJOKE / EF_TALK_SCREAM) and, Frost Joke only, SetSequenceInterval(10). A call
//   with the same arguments on the caster from elsewhere during that ReceiveSkillUse would be skipped too.
// - Earthquake: blocks CView::Start and CView::SetAmplitudes, whose only callers are CGameMode::SendMsg's MM_QUAKE and
//   MM_SETQUAKEINFO cases, which the mod emptied.
// - Blindness: hooks the virtual CGameActor::SendMsg (CPc and CPlayer forward AM_STATUS_CHANGE to it):
//   EFST_HANDICAPSTATE_DEEPBLIND runs the mod's replacement case; every other message goes to the original.
//   CPlayer::ReapplySkillEffects is ~650 lines, so it is not copied: while it runs, the EF_BLIND launch (only in its
//   Deep Blind case) is skipped, and when one was skipped g_overrideColor is cleared after it, as the mod's case does.
//   The case's other effects (its EF_STATUS_STATE relaunch, g_overrideColor = 1 until the function returns) still run.
// Shared hooks, one detour each:
// - NotifyGameMode (FrostJoke or Scream): drops value 1 when FrostJoke is on, value 0 when Scream is on.
// - ReceiveSkillUse (FrostJoke or Scream): marks BA_FROSTJOKE only when FrostJoke is on, DC_SCREAM only when Scream is on.
// - SetFrameSequence (FrostJoke or Scream) / SetSequenceInterval (FrostJoke): skip by the marked skill's exact arguments.
// - EffectActor::LaunchEffect (FrostJoke, Scream or Blindness): skips the marked skill's talk effect for its caster, and
//   EF_BLIND while ReapplySkillEffects runs (hooked only when Blindness is on).
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Session/Session.h"
#include "Contents/Actors/ActorEnums.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Actors/Pc.h"
#include "Contents/Actors/RenderObject.h"
#include "Contents/Camera/Camera.h"
#include "Contents/Effects/Core/EffectActor.h"
#include "Contents/Effects/Core/EffectTypes.h"
#include "Contents/Effects/Core/RagEffect.h"
#include "Contents/Effects/Primitives/EffectPrim.h"
#include "Contents/Skills/SkillId.h"
#include "Contents/Actors/Skill.h"
#include "Utility/Math/Vector3d.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "DisableEffects";

  bool g_frostJoke;
  bool g_scream;

  // Skill whose ReceiveSkillUse is running (0 if none) and its caster.
  std::uint16_t g_skillId;
  std::uint32_t g_sourceAid;

  bool g_reapplying;
  bool g_blindSkipped;

  // ---- Shared: FrostJoke / Scream / Blindness ----

  void (CRagEffect::*g_originalNotifyGameMode)(char);
  void (CGameMode::*g_originalReceiveSkillUse)(std::uint16_t, int, std::uint32_t, std::uint32_t, bool);
  void (CRenderObject::*g_originalSetSequenceInterval)(unsigned char);
  void (CRenderObject::*g_originalSetFrameSequence)(
    unsigned char, unsigned char, unsigned char, unsigned char, unsigned char, int, std::uint32_t, int
  );
  CRagEffect* (EffectActor::*g_originalLaunchEffect)(int, vector3d, float);

  struct RagEffectHook : CRagEffect {
    void NotifyGameMode(char value) {
      if ((g_frostJoke && value == 1) || (g_scream && value == 0)) {
        return;
      }
      (this->*g_originalNotifyGameMode)(value);
    }
  };

  struct GameModeHook : CGameMode {
    void ReceiveSkillUse(std::uint16_t skillId, int level, std::uint32_t sourceAid, std::uint32_t targetAid, bool result) {
      if (!(g_frostJoke && skillId == BA_FROSTJOKE) && !(g_scream && skillId == DC_SCREAM)) {
        (this->*g_originalReceiveSkillUse)(skillId, level, sourceAid, targetAid, result);
        return;
      }
      const std::uint16_t savedSkill = g_skillId;
      const std::uint32_t savedAid = g_sourceAid;
      g_skillId = skillId;
      g_sourceAid = sourceAid;
      (this->*g_originalReceiveSkillUse)(skillId, level, sourceAid, targetAid, result);
      g_skillId = savedSkill;
      g_sourceAid = savedAid;
    }
  };

  struct RenderObjectHook : CRenderObject {
    void SetSequenceInterval(unsigned char interval) {
      if (g_skillId == BA_FROSTJOKE && interval == 10 && this->render.accountId == g_sourceAid) {
        return;
      }
      (this->*g_originalSetSequenceInterval)(interval);
    }

    void SetFrameSequence(
      unsigned char animation, unsigned char firstFrame, unsigned char lastFrame, unsigned char finishAnimation,
      unsigned char finishFrame, int timed, std::uint32_t duration, int loop
    ) {
      if (g_skillId != 0 && this->render.accountId == g_sourceAid && animation == 0x60 && finishAnimation == 0x60 && timed == 1) {
        if (g_skillId == BA_FROSTJOKE && firstFrame == 4 && lastFrame == 5 && finishFrame == 4 && duration == 1800 && loop == 1) {
          return;
        }
        if (g_skillId == DC_SCREAM && firstFrame == 0 && lastFrame == 0 && finishFrame == 0 && duration == 1200 && loop == 0) {
          return;
        }
      }
      (this->*g_originalSetFrameSequence)(animation, firstFrame, lastFrame, finishAnimation, finishFrame, timed, duration, loop);
    }
  };

  struct EffectActorHook : EffectActor {
    CRagEffect* LaunchEffect(int effectId, vector3d deltaPos, float rotation) {
      if (g_skillId != 0 && this->render.accountId == g_sourceAid &&
          ((g_skillId == BA_FROSTJOKE && effectId == EF_TALK_FROSTJOKE) || (g_skillId == DC_SCREAM && effectId == EF_TALK_SCREAM))) {
        return nullptr;
      }
      // g_reapplying is set only by the ReapplySkillEffects detour, installed when Blindness is on.
      if (g_reapplying && effectId == EF_BLIND) {
        g_blindSkipped = true;
        return nullptr;
      }
      return (this->*g_originalLaunchEffect)(effectId, deltaPos, rotation);
    }
  };

  bool HookSkillTalk(const RoHostApi* host) {
    return ro::Hook(host, &CRagEffect::NotifyGameMode, &RagEffectHook::NotifyGameMode, g_originalNotifyGameMode, kName) &&
           ro::Hook(host, &CGameMode::ReceiveSkillUse, &GameModeHook::ReceiveSkillUse, g_originalReceiveSkillUse, kName) &&
           (!g_frostJoke ||
            ro::Hook(host, &CRenderObject::SetSequenceInterval, &RenderObjectHook::SetSequenceInterval, g_originalSetSequenceInterval, kName)) &&
           ro::Hook(host, &CRenderObject::SetFrameSequence, &RenderObjectHook::SetFrameSequence, g_originalSetFrameSequence, kName);
  }

  bool HookLaunchEffect(const RoHostApi* host) {
    CRagEffect* (EffectActor::*launchEffect)(int, vector3d, float) = &EffectActor::LaunchEffect;
    return ro::Hook(host, launchEffect, &EffectActorHook::LaunchEffect, g_originalLaunchEffect, kName);
  }

  // ---- Earthquake ----

  void (CView::*g_originalStart)(int, unsigned, float, float, float);
  void (CView::*g_originalSetAmplitudes)(float, float, float);

  struct ViewHook : CView {
    void Start(int, unsigned, float, float, float) {}
    void SetAmplitudes(float, float, float) {}
  };

  bool HookEarthquake(const RoHostApi* host) {
    return ro::Hook(host, &CView::Start, &ViewHook::Start, g_originalStart, kName) &&
           ro::Hook(host, &CView::SetAmplitudes, &ViewHook::SetAmplitudes, g_originalSetAmplitudes, kName);
  }

  // ---- Blindness ----

  void (CGameActor::*g_originalSendMsg)(CGameObject*, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t);
  void (CPlayer::*g_originalReapplySkillEffects)();

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

  bool HookBlindness(const RoHostApi* host) {
    return ro::HookVirtual(host, "?SendMsg@CGameActor@@UAEXPAVCGameObject@@_J111111@Z", &GameActorHook::SendMsgHook, g_originalSendMsg, kName) &&
           ro::Hook(host, &CPlayer::ReapplySkillEffects, &PlayerHook::ReapplySkillEffects, g_originalReapplySkillEffects, kName);
  }

  // ---- RestoreSongs ----

  constexpr int kFirstSongUnit = 158;
  // EF_BOTTOM_* for units 158 (Lullaby) to 175 (Service for You), in unit order.
  constexpr int kSongEffects[] = {
    EF_BOTTOM_LULLABY, EF_BOTTOM_RICHMANKIM, EF_BOTTOM_ETERNALCHAOS, EF_BOTTOM_DRUMBATTLEFIELD,
    EF_BOTTOM_RINGNIBELUNGEN, EF_BOTTOM_ROKISWEIL, EF_BOTTOM_INTOABYSS, EF_BOTTOM_SIEGFRIED, EF_BOTTOM_DISSONANCE,
    EF_BOTTOM_WHISTLE, EF_BOTTOM_ASSASSINCROSS, EF_BOTTOM_POEMBRAGI, EF_BOTTOM_APPLEIDUN, EF_BOTTOM_UGLYDANCE,
    EF_BOTTOM_HUMMING, EF_BOTTOM_DONTFORGETME, EF_BOTTOM_FORTUNEKISS, EF_BOTTOM_SERVICEFORYOU,
  };

  bool (CSkill::*g_originalOnProcess)();

  struct SkillHook : CSkill {
    bool OnProcessHook() {
      const bool result = (this->*g_originalOnProcess)();
      const unsigned index = static_cast<unsigned>(this->GetEffectiveJob(-1) - kFirstSongUnit);
      if (index >= sizeof(kSongEffects) / sizeof(kSongEffects[0])) {
        return result;
      }
      if (!this->m_effect) {
        this->m_effect = this->LaunchEffect(kSongEffects[index], vector3d{0.0f, 0.0f, 0.0f}, 0.0f);
      }
      if (this->m_effect) {
        const int duration = this->m_effect->GetDuration();
        this->m_effect->SetStateDuration(duration + 21);
      }
      return result;
    }
  };

  bool HookRestoreSongs(const RoHostApi* host) {
    return ro::HookVirtual(host, "?OnProcess@CSkill@@UAE_NXZ", &SkillHook::OnProcessHook, g_originalOnProcess, kName);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_frostJoke = host->SettingBool(kName, "FrostJoke");
  g_scream = host->SettingBool(kName, "Scream");
  const bool earthquake = host->SettingBool(kName, "Earthquake");
  const bool blindness = host->SettingBool(kName, "Blindness");
  const bool restoreSongs = host->SettingBool(kName, "RestoreSongs");
  if (!g_frostJoke && !g_scream && !earthquake && !blindness && !restoreSongs) {
    host->Log("%s: FrostJoke, Scream, Earthquake, Blindness and RestoreSongs are all off, nothing to do", kName);
    return true;
  }
  const bool skillTalk = g_frostJoke || g_scream;
  return (!skillTalk || HookSkillTalk(host)) && (!(skillTalk || blindness) || HookLaunchEffect(host)) &&
         (!earthquake || HookEarthquake(host)) && (!blindness || HookBlindness(host)) &&
         (!restoreSongs || HookRestoreSongs(host));
}
