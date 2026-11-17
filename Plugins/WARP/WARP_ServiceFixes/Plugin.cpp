// Hook versions of rodecomp's post-link patches 0001-0003, one hook per setting (only installed when it is on):
// - KoreanExternalSettings (0001): CSession::LoadExternalSettings runs the original with g_serviceType set to
//   ServiceKorea (0) and restores it after, so the switch takes case 0, the case the patch jumps to. The switch head is
//   the function's only g_serviceType read (the rest is g_serviceMode/g_launchServerName, LuaScript::Load/Call and
//   ShowErrorDialog, none of which read it), and the game runs single-threaded there, so nothing else sees the change.
// - NoMessageRot (0002): DecodeMessageRot returns its argument unchanged, as the patched jmp over the decode loop does.
// - ServiceFlags (0003): CSession::SetServiceFlags stores g_serviceFlag05 = first != 0 and g_serviceFlag06 =
//   second != 0 for every service type, as the patched jmp to the stores does, and returns g_serviceType like the
//   original.
// All three are exported, out-of-line functions with only direct calls (1, 24 and 3), so no call site is missed.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "Contents/Tables/MessageTable.h"

namespace {
  constexpr const char* kName = "WARP_ServiceFixes";

  void (CSession::*g_loadExternalSettings)();
  decltype(&DecodeMessageRot) g_decodeMessageRot;
  int (CSession::*g_setServiceFlags)(int, int);

  struct SessionHook : CSession {
    void LoadExternalSettingsDetour() {
      const int serviceType = g_serviceType;
      g_serviceType = ServiceKorea;
      (this->*g_loadExternalSettings)();
      g_serviceType = serviceType;
    }

    int SetServiceFlagsDetour(int first, int second) {
      g_serviceFlag05 = first != 0;
      g_serviceFlag06 = second != 0;
      return g_serviceType;
    }
  };

  char* DecodeMessageRotDetour(char* str) {
    return str;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool koreanExternalSettings = host->SettingBool(kName, "KoreanExternalSettings");
  const bool noMessageRot = host->SettingBool(kName, "NoMessageRot");
  const bool serviceFlags = host->SettingBool(kName, "ServiceFlags");
  if (!koreanExternalSettings && !noMessageRot && !serviceFlags) {
    host->Log("%s: KoreanExternalSettings, NoMessageRot and ServiceFlags are all off, nothing to do", kName);
    return true;
  }
  return (!koreanExternalSettings ||
          ro::Hook(host, &CSession::LoadExternalSettings, &SessionHook::LoadExternalSettingsDetour, g_loadExternalSettings, kName)) &&
         (!noMessageRot || ro::Hook(host, &DecodeMessageRot, &DecodeMessageRotDetour, g_decodeMessageRot, kName)) &&
         (!serviceFlags || ro::Hook(host, &CSession::SetServiceFlags, &SessionHook::SetServiceFlagsDetour, g_setServiceFlags, kName));
}
