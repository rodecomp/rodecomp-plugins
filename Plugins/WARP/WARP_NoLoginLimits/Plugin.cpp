// Hooks UILoginWnd::SendMsg to mark the window while UM_COMMAND/ID_LOGIN runs, and UIEditCtrl::Length (noinline, used
// by the ID_LOGIN minimum-length checks) to raise the ID edit's length (at least 4) and the password edit's (at least 6) for that window, so the login goes through.
#include "RoPlugin.h"

#include "UI/Edit/UIEditCtrl.h"
#include "UI/Login/UILoginWnd.h"
#include "UI/Window/UIEnums.h"

namespace {
  constexpr const char* kName = "WARP_NoLoginLimits";

  int (UILoginWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (UIEditCtrl::*g_length)() const;

  const UILoginWnd* g_login;

  struct EditHook : UIEditCtrl {
    int LengthDetour() const {
      const int length = (this->*g_length)();
      if (g_login == nullptr) {
        return length;
      }
      const UIEditCtrl* edit = this;
      if (edit == g_login->idEdit && length < 4) {
        return 4;
      }
      if (edit == g_login->passwordEdit && length < 6) {
        return 6;
      }
      return length;
    }
  };

  struct LoginHook : UILoginWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message != UM_COMMAND || a != ID_LOGIN) {
        return (this->*g_sendMsg)(sender, message, a, b, c, d);
      }
      const UILoginWnd* previous = g_login;
      g_login = this;
      const int result = (this->*g_sendMsg)(sender, message, a, b, c, d);
      g_login = previous;
      return result;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &UIEditCtrl::Length, &EditHook::LengthDetour, g_length, kName) &&
         ro::HookVirtual(host, "?SendMsg@UILoginWnd@@UAEHPAVUIWindow@@HHHHH@Z", &LoginHook::SendMsgDetour, g_sendMsg, kName);
}
