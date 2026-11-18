// For every extra hit of a multi-hit skill, CGameActor::ProcessSkillMessage sends the target AM_WILL_BE_ATTACKED with
// the hit's ActorDamageMessage (skillTarget = hit number). For CH_CHAINCRUSH it first queues the same message on the
// attacker with action 108, which replays the attack animation at the hit's time. CGameActor::SendMsg is hooked: when
// a target receives such a hit of a chosen skill, the attacker gets that queued action-108 message, as Chain Crush's.
#include "RoPlugin.h"

#include "Contents/Actors/ActorEnums.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Skills/SkillId.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "SkillAttackAnimation";
  constexpr int kAttackAction = 0x6C;

  bool g_sonicBlow;
  bool g_arrowVulcan;

  void (CGameActor::*g_original)(CGameObject*, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                                 std::int64_t, std::int64_t);

  struct ActorHook : CGameActor {
    void SendMsg(CGameObject* sender, std::int64_t message, std::int64_t b, std::int64_t c, std::int64_t d, std::int64_t e,
                 std::int64_t f, std::int64_t g) {
      if (message == AM_WILL_BE_ATTACKED && sender != nullptr && sender != this) {
        const ActorDamageMessage* damage = reinterpret_cast<const ActorDamageMessage*>(static_cast<std::intptr_t>(b));
        if (damage->skillTarget > 0 &&
            ((g_sonicBlow && damage->skillId == AS_SONICBLOW) || (g_arrowVulcan && damage->skillId == CG_ARROWVULCAN))) {
          ActorDamageMessage attack = *damage;
          attack.action = kAttackAction;
          static_cast<CGameActor*>(sender)->willBeAttackedList.push_back(attack);
        }
      }
      (this->*g_original)(sender, message, b, c, d, e, f, g);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_sonicBlow = host->SettingBool(kName, "SonicBlow");
  g_arrowVulcan = host->SettingBool(kName, "ArrowVulcan");
  if (!g_sonicBlow && !g_arrowVulcan) {
    host->Log("%s: no skill chosen", kName);
    return true;
  }
  return ro::HookVirtual(host, "?SendMsg@CGameActor@@UAEXPAVCGameObject@@_J111111@Z", &ActorHook::SendMsg, g_original, kName);
}
