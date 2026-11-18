// CSession::GetTalkType: the configured command toggles the GRF and reports talk type -1, which ExecuteTalkType's
// default case (GameSettings::ToggleOption) ignores, with offset -1 so the chat input is left alone. On, the GRF is
// added with PakRegistry::Add (newest archive is searched first); off, the restore list is added again on top, as WARP
// does. The state is saved in HKCU\Software\rodecomp-plugins\GrfSlashCommand and applied after CSession::Init.
#include "RoPlugin.h"

#include "Archive/PakRegistry.h"
#include "Client/Session/Session.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace {
  constexpr const char* kName = "GrfSlashCommand";
  constexpr const char* kKey = "Software\\rodecomp-plugins\\GrfSlashCommand";

  std::string g_command;
  std::string g_grf;
  std::vector<std::string> g_restore;
  bool g_enabled;

  int (CSession::*g_getTalkType)(const char*, int*, std::string*);
  void (CSession::*g_init)();

  bool LoadState() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueA(HKEY_CURRENT_USER, kKey, "Enabled", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
  }

  void SaveState() {
    const DWORD value = g_enabled ? 1 : 0;
    RegSetKeyValueA(HKEY_CURRENT_USER, kKey, "Enabled", REG_DWORD, &value, sizeof(value));
  }

  bool Apply() {
    if (g_enabled) {
      return g_pakRegistry.Add(g_grf.c_str());
    }
    bool ok = true;
    for (auto it = g_restore.rbegin(); it != g_restore.rend(); ++it) {
      ok = g_pakRegistry.Add(it->c_str()) && ok;
    }
    return ok;
  }

  struct SessionHook : CSession {
    int GetTalkTypeDetour(const char* text, int* talkType, std::string* parts) {
      if (text != nullptr && _stricmp(text, g_command.c_str()) == 0) {
        g_enabled = !g_enabled;
        const bool ok = Apply();
        SaveState();
        const std::string message = g_grf + (g_enabled ? " on" : " off") + (ok ? ", applied on the next map load." : ": a GRF failed to open.");
        g_windowMgr.SendNotice(UIM_PUSHINTOCHATHISTORY, reinterpret_cast<std::intptr_t>(message.c_str()), 0x00FFFF, 0, 0, 0);
        *talkType = -1;
        return -1;
      }
      return (this->*g_getTalkType)(text, talkType, parts);
    }

    void InitDetour() {
      (this->*g_init)();
      if (g_enabled) {
        Apply();
      }
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_command = host->SettingString(kName, "Command");
  g_grf = host->SettingString(kName, "Grf");
  if (g_command.size() < 2 || g_command[0] != '/' || g_grf.empty()) {
    host->Log("%s: Command must start with / and Grf must be set", kName);
    return false;
  }
  const int count = host->SettingCount(kName, "Restore");
  for (int i = 0; i < count; ++i) {
    g_restore.emplace_back(host->SettingItem(kName, "Restore", i));
  }
  g_enabled = LoadState();
  return ro::Hook(host, &CSession::GetTalkType, &SessionHook::GetTalkTypeDetour, g_getTalkType, kName) &&
         ro::Hook(host, &CSession::Init, &SessionHook::InitDetour, g_init, kName);
}
