// Per setting:
// - AuraSprites: after CLevel99Effect::Open (which loads pikapika2.bmp for the floating sparkles and, through
//   AddRings, its only caller, ring_blue.tga for the rings), the sparkles get effect\auraring.bmp and the rings
//   effect\aurafloat.tga, WARP's mapping. The warp portal and other effects load the stock textures from their own code.
// - AuraLimits: CGameActor::UpdateMaxLevelAura is reimplemented from the list: the first entry whose job range holds
//   the actor's effective job and whose level range holds its base level sends that aura's message (the ones the game
//   uses: AM_MAKE_LEVEL99_EFFECT, 0x93 for 150, AM_MAKE_LEVEL160_EFFECT, 0x96 for 185, AM_MAKE_LEVEL4TH_EFFECT); with no
//   match hasLevelAura is cleared, as the game does below max level. The "hide aura" option still wins.
#include "RoPlugin.h"

#include "Contents/Actors/ActorEnums.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Configuration/ServiceOptions.h"
#include "Contents/Effects/Particles/Particle.h"
#include "Contents/Effects/Skills/Level99Effect.h"
#include "UI/GameSettings/TalkType.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
  constexpr const char* kName = "Auras";

  struct Limit {
    int firstJob, lastJob;
    int minLevel, maxLevel;
    int message;
  };

  std::vector<Limit> g_limits;

  void (CLevel99Effect::*g_open)();
  void (CGameActor::*g_updateMaxLevelAura)();

  struct Level99Hook : CLevel99Effect {
    void Open() {
      (this->*g_open)();
      for (CParticle* particle : this->sparkles) {
        particle->SetTexture("effect\\auraring.bmp");
      }
      for (CParticle* particle : this->sprites) {
        particle->SetTexture("effect\\aurafloat.tga");
      }
    }
  };

  struct ActorHook : CGameActor {
    void UpdateMaxLevelAura() {
      if (g_serviceOptions.Get(TT_HIDE_AURA_ON_OFF) != 0) {
        return;
      }
      const int job = this->GetEffectiveJob(-1);
      const int level = this->characterLevel;
      for (const Limit& limit : g_limits) {
        if (job >= limit.firstJob && job <= limit.lastJob && level >= limit.minLevel && level <= limit.maxLevel) {
          this->SendMsg(nullptr, limit.message, 0, 0, 0, 0, 0, 0);
          return;
        }
      }
      this->hasLevelAura = 0;
    }
  };

  int AuraMessage(const char* aura) {
    if (std::strcmp(aura, "99") == 0) return AM_MAKE_LEVEL99_EFFECT;
    if (std::strcmp(aura, "150") == 0) return 0x93;
    if (std::strcmp(aura, "160") == 0) return AM_MAKE_LEVEL160_EFFECT;
    if (std::strcmp(aura, "185") == 0) return 0x96;
    if (std::strcmp(aura, "4th") == 0) return AM_MAKE_LEVEL4TH_EFFECT;
    return 0;
  }

  // "firstJob[-lastJob] : minLevel[-maxLevel] = aura"
  bool ParseLimit(const char* text, Limit& limit) {
    char jobs[32], levels[32], aura[8], extra;
    if (sscanf_s(text, " %31[0-9 -] : %31[0-9 -] = %7s %c", jobs, 32u, levels, 32u, aura, 8u, &extra, 1u) != 3) {
      return false;
    }
    auto range = [](const char* part, int& first, int& last) {
      char extra;
      const int read = sscanf_s(part, " %d - %d %c", &first, &last, &extra, 1u);
      if (read == 1) {
        last = first;
      }
      return (read == 1 || read == 2) && first >= 0 && first <= last;
    };
    limit.message = AuraMessage(aura);
    return range(jobs, limit.firstJob, limit.lastJob) && range(levels, limit.minLevel, limit.maxLevel) && limit.message != 0;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const int count = host->SettingCount(kName, "AuraLimits");
  for (int index = 0; index < count; ++index) {
    const char* text = host->SettingItem(kName, "AuraLimits", index);
    Limit limit{};
    if (!ParseLimit(text, limit)) {
      host->Log("%s: AuraLimits entry \"%s\" is not 'firstJob[-lastJob] : minLevel[-maxLevel] = 99|150|160|185|4th'", kName, text);
      return false;
    }
    g_limits.push_back(limit);
  }

  bool ok = true;
  if (host->SettingBool(kName, "AuraSprites")) {
    ok = ok && ro::HookVirtual(host, "?Open@CLevel99Effect@@UAEXXZ", &Level99Hook::Open, g_open, kName);
  }
  if (!g_limits.empty()) {
    ok = ok && ro::Hook(host, &CGameActor::UpdateMaxLevelAura, &ActorHook::UpdateMaxLevelAura, g_updateMaxLevelAura, kName);
  }
  return ok;
}
