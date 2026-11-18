// Hooks per setting:
// - DirectRegister: UILoginWnd::SendMsg, for UM_COMMAND/ID_MAKEACCOUNT at langtype 0, runs what every other langtype
//   runs (CModeMgr::Quit, then OpenWebPage) instead of the Hangame/confirm branch.
// - ReplayButton: UISelectServerWnd::OnCreate skips the Replay button (id ID_SELECT_REPLAY) unless the launcher passed
//   a command line; after the original, if the window has no such child, it is added exactly as OnCreate builds it.
//   The window's own "hide at cursor type 30" rule is kept.
// - DisconnectToLogin: CGameMode::ReceiveNotifyBan (the map server's ban/disconnect notice) closes the connection,
//   shows the reason and sends MM_QUIT. While it runs, the CGameMode::SendMsg detour turns that MM_QUIT into
//   MM_QUIT_TO_IDENTRY, which switches to the login mode.
#include "RoPlugin.h"

#include "Client/Application/Launcher.h"
#include "Client/Mode/CGameMode.h"
#include "Client/Mode/CLoginMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "UI/Buttons/UIBitmapButton.h"
#include "UI/Login/UILoginWnd.h"
#include "UI/Login/UISelectServerWnd.h"
#include "UI/Window/UIEnums.h"

#include <cstdint>
#include <string>

namespace {
  constexpr const char* kName = "LoginScreen";

  int (UILoginWnd::*g_loginSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  void (UISelectServerWnd::*g_selectServerOnCreate)(int, int);
  void (CGameMode::*g_receiveNotifyBan)(const unsigned char*);
  int (CGameMode::*g_gameModeSendMsg)(int, int, int, int, int);

  bool g_inNotifyBan;

  struct LoginHook : UILoginWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_COMMAND && a == ID_MAKEACCOUNT && g_serviceType == 0) {
        g_modeMgr.Quit();
        this->OpenWebPage();
        return 0;
      }
      return (this->*g_loginSendMsg)(sender, message, a, b, c, d);
    }
  };

  struct SelectServerHook : UISelectServerWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_selectServerOnCreate)(width, height);
      for (UIWindow* child : this->m_children) {
        if (child->geometry.id == ID_SELECT_REPLAY) {
          return;
        }
      }
      const std::string prefix("유저인터페이스\\replay_interface\\");
      UIBitmapButton* button = new UIBitmapButton();
      button->SetBitmap((prefix + "btn_replay.bmp").c_str(), 0, 0);
      button->SetBitmap((prefix + "btn_replay_a.bmp").c_str(), 1, 0);
      button->SetBitmap((prefix + "btn_replay_b.bmp").c_str(), 2, 0);
      button->SetSize(button->BitmapWidth(), button->BitmapHeight());
      if (g_modeMgr.GetLoginMode()->cursorType == 30) {
        button->Move(-100, -100);
      } else {
        button->Move(130, height - 24);
      }
      button->SetId(ID_SELECT_REPLAY);
      this->AddChild(button);
    }
  };

  struct GameModeHook : CGameMode {
    void ReceiveNotifyBanDetour(const unsigned char* packet) {
      g_inNotifyBan = true;
      (this->*g_receiveNotifyBan)(packet);
      g_inNotifyBan = false;
    }

    int SendMsgDetour(int event, int argument1, int argument2, int argument3, int argument4) {
      if (g_inNotifyBan && event == MM_QUIT) {
        event = MM_QUIT_TO_IDENTRY;
      }
      return (this->*g_gameModeSendMsg)(event, argument1, argument2, argument3, argument4);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool reg = host->SettingBool(kName, "DirectRegister");
  const bool replay = host->SettingBool(kName, "ReplayButton");
  const bool disconnect = host->SettingBool(kName, "DisconnectToLogin");
  if (!reg && !replay && !disconnect) {
    host->Log("%s: DirectRegister, ReplayButton and DisconnectToLogin are all off, nothing to do", kName);
    return true;
  }
  return (!reg || ro::HookVirtual(host, "?SendMsg@UILoginWnd@@UAEHPAVUIWindow@@HHHHH@Z", &LoginHook::SendMsgDetour,
                                  g_loginSendMsg, kName)) &&
         (!replay || ro::HookVirtual(host, "?OnCreate@UISelectServerWnd@@UAEXHH@Z", &SelectServerHook::OnCreateDetour,
                                     g_selectServerOnCreate, kName)) &&
         (!disconnect ||
          (ro::Hook(host, &CGameMode::ReceiveNotifyBan, &GameModeHook::ReceiveNotifyBanDetour, g_receiveNotifyBan, kName) &&
           ro::HookVirtual(host, "?SendMsg@CGameMode@@UAEHHHHHH@Z", &GameModeHook::SendMsgDetour, g_gameModeSendMsg, kName)));
}
