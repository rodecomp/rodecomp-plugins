// CSession::InitJobNameTable gives ids 6001..6016 fixed homunculus names (6017..6052 the mercenaries). After it, ids
// 6001..MaxId take ReqJobName(id) from g_session.m_hotkeyScript when it answers a non-empty name (the strings are kept
// here: LuaScript::Call returns Lua's own, which it pops). CSession::IsCompanionJob (6001..6052, the id check WARP
// widened) accepts 6001..MaxId.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Jobs/JobId.h"
#include "Script/LuaScript.h"

#include <deque>
#include <string>

namespace {
  constexpr const char* kName = "CustomHomunculi";

  int g_maxId;
  std::deque<std::string> g_names;

  void (CSession::*g_initJobNameTable)();
  int (CSession::*g_isCompanionJob)(unsigned) const;

  struct SessionHook : CSession {
    void InitJobNameTable() {
      (this->*g_initJobNameTable)();
      if (!this->m_hotkeyScript) {
        return;
      }
      for (int id = JT_MER_LIF; id <= g_maxId && id < static_cast<int>(this->m_jobNameTable.size()); ++id) {
        const char* name = nullptr;
        if (this->m_hotkeyScript->Call("ReqJobName", "d>s", id, &name) && name && name[0] != '\0') {
          this->m_jobNameTable[id] = g_names.emplace_back(name).c_str();
        }
      }
    }

    int IsCompanionJob(unsigned id) const {
      return (this->*g_isCompanionJob)(id) || id - JT_MER_LIF <= static_cast<unsigned>(g_maxId - JT_MER_LIF);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_maxId = static_cast<int>(host->SettingInt(kName, "MaxId"));
  return ro::Hook(host, &CSession::InitJobNameTable, &SessionHook::InitJobNameTable, g_initJobNameTable, kName) &&
         ro::Hook(host, &CSession::IsCompanionJob, &SessionHook::IsCompanionJob, g_isCompanionJob, kName);
}
