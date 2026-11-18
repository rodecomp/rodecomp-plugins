// Hooks per setting:
// - Who: CGameMode::ExecuteTalkType sends MM_REQ_NUM_USER for TT_REQ_NUM_USER itself on Taiwan (where the game prints
//   "Invalid command"); GetUserCountText (only called by the ZC_USER_COUNT handler for China non-admins) returns the
//   MSI_NUMPEOPLE number line instead of the vague "few/many users" text.
// - UnknownCommands: CSession::GetTalkType turns TT_UNKNOWN for a line starting with '/' into TT_NORMAL with offset 0,
//   so the whole line is sent as chat. FindServiceOption and SetOption, which use GetTalkType to look up option names
//   and test for TT_UNKNOWN, run with the rewrite off.
// WARP EnableShowName needs no hook: the plugin exe's rodecomp patch 0003_always_set_service_flags already makes
// CSession::SetServiceFlags store the name style flags on every service type.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/CLoginMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Session/Session.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "Contents/Configuration/ServiceOptions.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/MSGSTRINGID.h"
#include "UI/GameSettings/TalkType.h"

#include <cstdio>
#include <string>
#include <utility>

namespace {
  constexpr const char* kName = "SlashCommands";

  int g_optionLookups;

  void (CGameMode::*g_executeTalkType)(int, std::string*);
  const char* (*g_getUserCountText)(unsigned short);
  int (CSession::*g_getTalkType)(const char*, int*, std::string*);
  std::pair<int, bool> (*g_findServiceOption)(const char*);
  int (*g_setOption)(const char*, bool);

  struct GameModeHook : CGameMode {
    void ExecuteTalkType(int talkType, std::string* arguments) {
      if (talkType == TT_REQ_NUM_USER && g_serviceType == ServiceTaiwan) {
        this->SendMsg(MM_REQ_NUM_USER, 0, 0, 0, 0);
        return;
      }
      (this->*g_executeTalkType)(talkType, arguments);
    }
  };

  const char* UserCountTextDetour(unsigned short userCount) {
    static char text[128];
    std::snprintf(text, sizeof(text), LookupMessage(MSI_NUMPEOPLE), static_cast<int>(userCount));
    return text;
  }

  struct SessionHook : CSession {
    int GetTalkType(const char* text, int* talkType, std::string* parts) {
      const int offset = (this->*g_getTalkType)(text, talkType, parts);
      if (*talkType == TT_UNKNOWN && g_optionLookups == 0 && text != nullptr && text[0] == '/') {
        *talkType = TT_NORMAL;
        return 0;
      }
      return offset;
    }
  };

  std::pair<int, bool> FindServiceOptionDetour(const char* name) {
    ++g_optionLookups;
    const std::pair<int, bool> result = g_findServiceOption(name);
    --g_optionLookups;
    return result;
  }

  int SetOptionDetour(const char* name, bool value) {
    ++g_optionLookups;
    const int result = g_setOption(name, value);
    --g_optionLookups;
    return result;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool who = host->SettingBool(kName, "Who");
  const bool unknown = host->SettingBool(kName, "UnknownCommands");
  if (!who && !unknown) {
    host->Log("%s: Who and UnknownCommands are both off, nothing to do", kName);
    return true;
  }
  return (!who ||
          (ro::Hook(host, &CGameMode::ExecuteTalkType, &GameModeHook::ExecuteTalkType, g_executeTalkType, kName) &&
           ro::Hook(host, &GetUserCountText, &UserCountTextDetour, g_getUserCountText, kName))) &&
         (!unknown || (ro::Hook(host, &CSession::GetTalkType, &SessionHook::GetTalkType, g_getTalkType, kName) &&
                       ro::Hook(host, &FindServiceOption, &FindServiceOptionDetour, g_findServiceOption, kName) &&
                       ro::Hook(host, &SetOption, &SetOptionDetour, g_setOption, kName)));
}
