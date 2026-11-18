// CGameActor/CItem/CSkill::SetRenderInfo widen a pick rectangle smaller than a function-local minimum (UiScaleX * 40
// or 34) before CActorPickNode::AddPickInfo. Each SetRenderInfo is hooked to remember its raw bounds and minimum; the
// AddPickInfo inside it rebuilds the horizontal and vertical extent from the raw bounds, widened to minimum >> Shift.
// (WARP patches the stores of those statics; they are not exported, so the rectangle is redone instead.)
#include "RoPlugin.h"

#include "Contents/Actors/ActorPickNode.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Actors/Item.h"
#include "Contents/Actors/Skill.h"
#include "Render/Renderer.h"

namespace {
  constexpr const char* kName = "ClickArea";

  int g_shift;
  struct Pending {
    bool active;
    float left, top, right, bottom;
    float factor;
  } g_pending;

  void (CGameActor::*g_actorInfo)(const float*, float, float);
  void (CItem::*g_itemInfo)(const float*, float, float);
  void (CSkill::*g_skillInfo)(const float*, float, float);
  void (CActorPickNode::*g_addPickInfo)(ActorPickInfo*);

  struct Scope {
    Scope(const float* bounds, float factor) {
      g_pending = {true, bounds[0] < bounds[2] ? bounds[0] : bounds[2], bounds[1], bounds[0] > bounds[2] ? bounds[0] : bounds[2], bounds[3], factor};
    }
    ~Scope() { g_pending.active = false; }
  };

  void Fit(float& low, float& high, float rawLow, float rawHigh, int minimum) {
    low = rawLow;
    high = rawHigh;
    const int size = static_cast<int>(high - low);
    if (size < minimum) {
      const float grow = static_cast<float>((minimum - size) / 2);
      low -= grow;
      high += grow;
    }
  }

  struct PickNodeHook : CActorPickNode {
    void AddPickInfoDetour(ActorPickInfo* info) {
      if (g_pending.active) {
        g_pending.active = false;
        const int minimum = static_cast<int>(g_renderer->UiScaleX() * g_pending.factor) >> g_shift;
        Fit(info->left, info->right, g_pending.left, g_pending.right, minimum);
        Fit(info->top, info->bottom, g_pending.top, g_pending.bottom, minimum);
      }
      (this->*g_addPickInfo)(info);
    }
  };

  struct ActorHook : CGameActor {
    void Detour(const float* bounds, float x, float y) { Scope scope(bounds, 40.0f); (this->*g_actorInfo)(bounds, x, y); }
  };
  struct ItemHook : CItem {
    void Detour(const float* bounds, float x, float y) { Scope scope(bounds, 34.0f); (this->*g_itemInfo)(bounds, x, y); }
  };
  struct SkillHook : CSkill {
    void Detour(const float* bounds, float x, float y) { Scope scope(bounds, 34.0f); (this->*g_skillInfo)(bounds, x, y); }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_shift = static_cast<int>(host->SettingInt(kName, "Shift"));
  if (g_shift == 0) {
    return true;
  }
  return ro::Hook(host, &CActorPickNode::AddPickInfo, &PickNodeHook::AddPickInfoDetour, g_addPickInfo, kName) &&
         ro::HookVirtual(host, "?SetRenderInfo@CGameActor@@UAEXPBMMM@Z", &ActorHook::Detour, g_actorInfo, kName) &&
         ro::HookVirtual(host, "?SetRenderInfo@CItem@@UAEXPBMMM@Z", &ItemHook::Detour, g_itemInfo, kName) &&
         ro::Hook(host, &CSkill::SetRenderInfo, &SkillHook::Detour, g_skillInfo, kName);
}
