// EquipmentResourceMgr::GetShieldResourceName maps a shield item to its sprite type with CSession::GetItemDescription40
// (its only caller), checks the type with CSession::HasJobShieldSprite and names it with CSession::GetRobeName (the
// shield suffixes, also its only use). ShieldTable and ShieldTable_F are loaded with LuaFiles after jobName_F into
// g_session.m_hotkeyScript, the same state as WARP. Then:
// - GetItemDescription40: GetShieldID(item) when it is not 0, else the original;
// - HasJobShieldSprite: for the custom types (5 .. Max_Shield - 1), ValidateShieldID(type, job);
// - GetRobeName: for the custom types, ReqShieldName(type).
// Built-in types (1 .. 4) and per-item sprites (ids above 2000) keep the game's rules.
#include "RoPlugin.h"
#include "LuaFilesApi.h"

#include "Client/Session/Session.h"
#include "Script/LuaScript.h"

#include <string>

namespace {
  constexpr const char* kName = "CustomShields";
  constexpr int kFirstCustom = 5;

  int (CSession::*g_getItemDescription40)(int);
  bool (CSession::*g_hasJobShieldSprite)(int, int) const;
  std::string (CSession::*g_getRobeName)(unsigned);

  bool IsCustom(const CSession* session, int type) {
    return type >= kFirstCustom && session->m_hotkeyScript && type < session->m_hotkeyScript->GlobalInteger("Max_Shield");
  }

  struct SessionHook : CSession {
    int GetItemDescription40(int id) {
      int type = 0;
      if (this->m_hotkeyScript && this->m_hotkeyScript->Call("GetShieldID", "d>d", id, &type) && type != 0) {
        return type;
      }
      return (this->*g_getItemDescription40)(id);
    }

    bool HasJobShieldSprite(int job, int shield) const {
      if (!IsCustom(this, shield)) {
        return (this->*g_hasJobShieldSprite)(job, shield);
      }
      bool valid = false;
      this->m_hotkeyScript->Call("ValidateShieldID", "dd>b", shield, job, &valid);
      return valid;
    }

    std::string GetRobeName(unsigned robe) {
      if (!IsCustom(this, static_cast<int>(robe))) {
        return (this->*g_getRobeName)(robe);
      }
      const char* name = nullptr;
      if (!this->m_hotkeyScript->Call("ReqShieldName", "d>s", static_cast<int>(robe), &name) || !name) {
        return "";
      }
      return name;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  LuaFiles_LoadAfter("Lua Files\\DataInfo\\jobName_F", "Lua Files\\DataInfo\\ShieldTable");
  LuaFiles_LoadAfter("Lua Files\\DataInfo\\jobName_F", "Lua Files\\DataInfo\\ShieldTable_F");
  return ro::Hook(host, &CSession::GetItemDescription40, &SessionHook::GetItemDescription40, g_getItemDescription40, kName) &&
         ro::Hook(host, &CSession::HasJobShieldSprite, &SessionHook::HasJobShieldSprite, g_hasJobShieldSprite, kName) &&
         ro::Hook(host, &CSession::GetRobeName, &SessionHook::GetRobeName, g_getRobeName, kName);
}
