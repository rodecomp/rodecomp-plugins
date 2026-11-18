// - SkipServiceSelect / MissingLauncherMessage: after ReadClientInfo (the clientinfo.xml reader that sets both),
//   g_hideAccountList is forced on and g_runPatcherMessage replaced.
// - OpeningToServiceSelect: UILoginWnd::SendMsg handles the Opening button (id 285) like ID_CANCEL: sets
//   g_loginCancelRequested and sends LMM_GOTOSELECTACCOUNT.
#include "RoPlugin.h"

#include "Client/Mode/CLoginMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Client/Mode/CMode.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "UI/Login/UILoginWnd.h"
#include "UI/Window/UIEnums.h"

#include <cstdint>

extern bool g_hideAccountList;

namespace {
  constexpr const char* kName = "LoginFlow";
  constexpr std::intptr_t kOpeningButton = 285;

  bool g_skip;
  const char* g_message;

  void (*g_readClientInfo)(XmlNode*);
  int (UILoginWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  void ReadClientInfoDetour(XmlNode* clientInfo) {
    g_readClientInfo(clientInfo);
    if (g_skip) {
      g_hideAccountList = true;
    }
    if (g_message != nullptr) {
      g_runPatcherMessage = g_message;
    }
  }

  struct LoginWndHook : UILoginWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_COMMAND && a == kOpeningButton) {
        g_loginCancelRequested = 1;
        g_modeMgr.currentMode->SendMsg(LMM_GOTOSELECTACCOUNT, 0, 0, 0, 0);
        return 0;
      }
      return (this->*g_sendMsg)(sender, message, a, b, c, d);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_skip = host->SettingBool(kName, "SkipServiceSelect");
  const char* message = host->SettingString(kName, "MissingLauncherMessage");
  g_message = message[0] != '\0' ? message : nullptr;
  const bool opening = host->SettingBool(kName, "OpeningToServiceSelect");
  return ((!g_skip && g_message == nullptr) || ro::Hook(host, &ReadClientInfo, &ReadClientInfoDetour, g_readClientInfo, kName)) &&
         (!opening || ro::HookVirtual(host, "?SendMsg@UILoginWnd@@UAEHPAVUIWindow@@HHHHH@Z", &LoginWndHook::SendMsgDetour, g_sendMsg, kName));
}
