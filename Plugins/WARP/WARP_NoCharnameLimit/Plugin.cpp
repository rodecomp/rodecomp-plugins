// Hooks UINewMakeCharWnd::SendMsg and UIChangeNameWnd::SendMsg to mark the window's name edit while a UM_COMMAND/ID_OK
// runs, and UIEditCtrl::Length (noinline, called by both "< 4" checks) to report 1-3 characters as 4 for that edit.
// The ">= 24" checks see lengths of 4 and more unchanged.
#include "RoPlugin.h"

#include "UI/Edit/UIEditCtrl.h"
#include "UI/Login/UIChangeNameWnd.h"
#include "UI/Login/UINewMakeCharWnd.h"
#include "UI/Window/UIEnums.h"

namespace {
  constexpr const char* kName = "WARP_NoCharnameLimit";

  using SendMsgFn = int (UIWindow::*)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (UINewMakeCharWnd::*g_makeCharSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (UIChangeNameWnd::*g_changeNameSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (UIEditCtrl::*g_length)() const;

  const UIEditCtrl* g_nameEdit;

  struct EditHook : UIEditCtrl {
    int LengthDetour() const {
      const int length = (this->*g_length)();
      return this == g_nameEdit && length >= 1 && length < 4 ? 4 : length;
    }
  };

  template <class Window, class Original>
  int CheckedSendMsg(Window* window, Original original, const UIEditCtrl* edit, UIWindow* sender, int message,
                     std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
    if (message != UM_COMMAND || a != ID_OK) {
      return (window->*original)(sender, message, a, b, c, d);
    }
    const UIEditCtrl* previous = g_nameEdit;
    g_nameEdit = edit;
    const int result = (window->*original)(sender, message, a, b, c, d);
    g_nameEdit = previous;
    return result;
  }

  struct MakeCharHook : UINewMakeCharWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      return CheckedSendMsg(static_cast<UINewMakeCharWnd*>(this), g_makeCharSendMsg, this->m_nameEdit, sender, message, a, b, c, d);
    }
  };

  struct ChangeNameHook : UIChangeNameWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      return CheckedSendMsg(static_cast<UIChangeNameWnd*>(this), g_changeNameSendMsg, this->m_edit, sender, message, a, b, c, d);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &UIEditCtrl::Length, &EditHook::LengthDetour, g_length, kName) &&
         ro::HookVirtual(host, "?SendMsg@UINewMakeCharWnd@@UAEHPAVUIWindow@@HHHHH@Z", &MakeCharHook::SendMsgDetour,
                         g_makeCharSendMsg, kName) &&
         ro::HookVirtual(host, "?SendMsg@UIChangeNameWnd@@UAEHPAVUIWindow@@HHHHH@Z", &ChangeNameHook::SendMsgDetour,
                         g_changeNameSendMsg, kName);
}
