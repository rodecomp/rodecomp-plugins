// CGameMode::ShowRestrictedNotice(grade) is the one check every chat path (public, party, guild, whisper, ...) runs
// with CGameMode::sameChatRepeatCount: it refuses the line when grade >= 2. The detour returns false (allowed) below
// the configured limit and otherwise calls the original with a grade of at least 2, so the game's own exceptions
// (service mode 3, Germany) and its notice are kept. Unlimited never refuses.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"

#include <cstring>

namespace {
  constexpr const char* kName = "ChatRepeat";

  bool g_unlimited;
  int g_limit = 2;

  bool (*g_showRestrictedNotice)(int);

  bool ShowRestrictedNotice(int grade) {
    if (g_unlimited || grade < g_limit) {
      return false;
    }
    return g_showRestrictedNotice(grade < 2 ? 2 : grade);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const char* mode = host->SettingString(kName, "Mode");
  if (std::strcmp(mode, "Unlimited") == 0) {
    g_unlimited = true;
  } else if (std::strcmp(mode, "Custom") == 0) {
    g_limit = static_cast<int>(host->SettingInt(kName, "Limit"));
  } else {
    host->Log("%s: Mode is Default, nothing to do", kName);
    return true;
  }
  return ro::Hook(host, &CGameMode::ShowRestrictedNotice, &ShowRestrictedNotice, g_showRestrictedNotice, kName);
}
