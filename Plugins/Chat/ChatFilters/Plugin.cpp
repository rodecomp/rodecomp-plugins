// - NoSwearFilter: ForbiddenWords::Load refuses "manner.txt" (its only file), so g_forbiddenWords stays empty and
//   Contains/Mask never match.
// - NoLookalikeWarning: CSession::IsDifferentName always answers 1 ("clearly different"); its only callers are
//   IsFriendNameDifferent and IsGuildMemberNameDifferent, which gate the MSI_ID_WARNING(_GUILD) lines.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Tables/ForbiddenWords.h"

#include <cstring>

namespace {
  constexpr const char* kName = "ChatFilters";

  bool (ForbiddenWords::*g_load)(const char*);
  int (CSession::*g_isDifferentName)(const char*, const char*);

  struct ForbiddenWordsHook : ForbiddenWords {
    bool LoadDetour(const char* name) {
      if (name != nullptr && _stricmp(name, "manner.txt") == 0) {
        return false;
      }
      return (this->*g_load)(name);
    }
  };

  struct SessionHook : CSession {
    int IsDifferentNameDetour(const char*, const char*) { return 1; }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool swear = host->SettingBool(kName, "NoSwearFilter");
  const bool lookalike = host->SettingBool(kName, "NoLookalikeWarning");
  return (!swear || ro::Hook(host, &ForbiddenWords::Load, &ForbiddenWordsHook::LoadDetour, g_load, kName)) &&
         (!lookalike || ro::Hook(host, &CSession::IsDifferentName, &SessionHook::IsDifferentNameDetour, g_isDifferentName, kName));
}
