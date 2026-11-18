// UIChoose3Wnd::SendMsg(UM_SETCHOOSE) grows the menu by one line per option between m_minimumLines and
// m_maximumLines. Setting m_maximumLines to m_minimumLines before each add disables that growth, so the list box
// scrolls like old clients (WARP turns the growth test always false).
#include "RoPlugin.h"

#include "UI/Dialogs/UIChooseWnd.h"
#include "UI/Window/UIEnums.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "NpcSelectMenu";

  int (UIChoose3Wnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  struct ChooseHook : UIChoose3Wnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_SETCHOOSE) {
        this->m_maximumLines = this->m_minimumLines;
      }
      return (this->*g_sendMsg)(sender, message, a, b, c, d);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::HookVirtual(host, "?SendMsg@UIChoose3Wnd@@UAEHPAVUIWindow@@HHHHH@Z", &ChooseHook::SendMsgDetour, g_sendMsg, kName);
}
