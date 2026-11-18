// SkillType and SkillType_F (Lua Files\SkillInfoz) are loaded with LuaFiles after SkillInfo_F into
// g_session.m_hotkeyScript, the state WARP loads them into. Then, per setting, the client's fixed id check asks Lua:
// - HomunSkills: CSession::IsHomunculusSkill (8000..8060) returns IsHomunSkill(id);
// - MercenarySkills: CSession::IsMercenarySkill (8200..8241, 8400..8457) returns IsMercenarySkill(id);
// - PlayerSkills: IsSupportSkill (the switch WARP's CustomPlayerSkills replaces) returns IsPlayerSkill(id) ~= 0.
// A Lua function that is not defined leaves the client's own check, so a missing SkillType file changes nothing
// (LuaScript::Call would show an error box for an undefined function).
extern "C" {
#include "../ThirdParty/lua/src/lua.h"
}

#include "RoPlugin.h"
#include "LuaFilesApi.h"

#include "Client/Session/Session.h"
#include "Contents/Skills/SkillInfo.h"
#include "Script/LuaScript.h"

namespace {
  constexpr const char* kName = "CompanionSkills";

  int (CSession::*g_isHomunculusSkill)(unsigned) const;
  int (CSession::*g_isMercenarySkill)(unsigned) const;
  bool (*g_isSupportSkill)(int);

  // Lua's `function(id)` answer, or -1 when the function is not defined.
  int Ask(const char* function, unsigned id) {
    LuaScript* script = g_session.m_hotkeyScript;
    lua_State* state = script ? script->GetState() : nullptr;
    if (!state) {
      return -1;
    }
    lua_getglobal(state, function);
    const bool defined = lua_isfunction(state, -1);
    lua_pop(state, 1);
    int result = 0;
    if (!defined || !script->Call(function, "d>d", static_cast<int>(id), &result)) {
      return -1;
    }
    return result;
  }

  struct SessionHook : CSession {
    int IsHomunculusSkill(unsigned id) const {
      const int result = Ask("IsHomunSkill", id);
      return result < 0 ? (this->*g_isHomunculusSkill)(id) : result != 0;
    }

    int IsMercenarySkill(unsigned id) const {
      const int result = Ask("IsMercenarySkill", id);
      return result < 0 ? (this->*g_isMercenarySkill)(id) : result != 0;
    }
  };

  bool IsSupportSkillDetour(int id) {
    const int result = Ask("IsPlayerSkill", static_cast<unsigned>(id));
    return result < 0 ? g_isSupportSkill(id) : result != 0;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool homun = host->SettingBool(kName, "HomunSkills");
  const bool mercenary = host->SettingBool(kName, "MercenarySkills");
  const bool player = host->SettingBool(kName, "PlayerSkills");
  if (!homun && !mercenary && !player) {
    return true;
  }
  LuaFiles_LoadAfter("Lua Files\\SkillInfoz\\SkillInfo_F", "Lua Files\\SkillInfoz\\SkillType");
  LuaFiles_LoadAfter("Lua Files\\SkillInfoz\\SkillInfo_F", "Lua Files\\SkillInfoz\\SkillType_F");

  bool ok = true;
  if (homun) {
    ok = ok && ro::Hook(host, &CSession::IsHomunculusSkill, &SessionHook::IsHomunculusSkill, g_isHomunculusSkill, kName);
  }
  if (mercenary) {
    ok = ok && ro::Hook(host, &CSession::IsMercenarySkill, &SessionHook::IsMercenarySkill, g_isMercenarySkill, kName);
  }
  if (player) {
    ok = ok && ro::Hook(host, &IsSupportSkill, &IsSupportSkillDetour, g_isSupportSkill, kName);
  }
  return ok;
}
