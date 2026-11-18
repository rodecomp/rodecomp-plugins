// CGameActor::SpawnDamageDigits clamps a value above 999999 to 999999 unless TT_EXTEND_DAMAGE_ON_OFF is on. Only in
// that case the hook replaces it: the same digit effects, built from the full value. Every other call runs the original.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMgr.h"
#include "Contents/Actors/ActorEnums.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Configuration/ServiceOptions.h"
#include "Contents/Effects/Core/MsgEffect.h"
#include "Contents/World/World.h"
#include "UI/GameSettings/TalkType.h"

#include <cstdint>
#include <vector>

namespace {
  constexpr const char* kName = "DamageNumbers";

  void (CGameActor::*g_original)(int, int, std::uint32_t, int);

  struct ActorHook : CGameActor {
    void SpawnDamageDigits(int unused, int damage, std::uint32_t color, int type) {
      if (damage <= 999999 || ServiceOptions::Get(TT_EXTEND_DAMAGE_ON_OFF)) {
        (this->*g_original)(unused, damage, color, type);
        return;
      }

      std::vector<int> digits;
      for (int remaining = damage; remaining != 0; remaining /= 10) {
        digits.push_back(remaining % 10);
      }

      CGameMode* mode = g_modeMgr.GetGameMode();
      if (type == 22) {
        for (auto it = this->msgEffectList.begin(); it != this->msgEffectList.end();) {
          CMsgEffect* effect = static_cast<CMsgEffect*>(*it);
          if (effect->m_type == 22) {
            this->ClearStatusEffectInstance(effect);
            effect->SendMsg(this, AM_MASTER_DEAD, 0, 0, 0, 0, 0, 0);
            it = this->msgEffectList.erase(it);
            mode->scene->RemoveObject(effect);
          } else {
            ++it;
          }
        }
      }

      const int count = static_cast<int>(digits.size());
      for (int index = 0; index < count; ++index) {
        CMsgEffect* effect = new CMsgEffect;
        effect->render.color = color;
        effect->SendMsg(nullptr, AM_SETMASTERGID, static_cast<std::uint32_t>(this->render.accountId), type, 0, 0, 0, 0);
        effect->SendMsg(nullptr, AM_SETPOS2, reinterpret_cast<std::intptr_t>(&this->render.position), digits[index],
                        static_cast<std::uint32_t>((count - index * 2) * 4 - 4), 0, 0, 0);
        effect->SendMsg(this, AM_SET_MASTER_POINTER, 0, 0, 0, 0, 0, 0);
        effect->render.useGroundLighting = 0;
        if (type == 22) {
          effect->render.scale = 0.5f;
        }
        mode->AddSceneObject(effect);
        this->msgEffectList.emplace_back(effect);
      }
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CGameActor::SpawnDamageDigits, &ActorHook::SpawnDamageDigits, g_original, kName);
}
