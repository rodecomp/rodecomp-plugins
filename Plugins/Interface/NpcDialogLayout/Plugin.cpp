// NPC dialog (WID_SAYDIALOGWND, g_windowMgr.sayDialogWnd) and choice menu (WID_CHOOSEWND, g_windowMgr.choose3Wnd)
// layout. Per setting:
// - CenterDialog: UISayDialogWnd::SendMsg(UM_SETSAYDIALOG) on the NPC dialog (ZC_SAY_DIALOG; the overlay dialog of
//   ZC_WAIT_DIALOG is another window and is left alone) moves it to ((viewport width - width) / 2,
//   (viewport height - height) / 2 - 40), clamped to y >= 0.
// - AttachMenu: the menu is put at (dialog x, dialog bottom + 4) after CGameMode::ReceiveWindow17Entries (ZC_MENU_LIST,
//   which (re)creates it) and after each NPC message. Dragging either window (UIFrameWnd::OnMouseMove while it has the
//   capture) moves the other to keep that offset. UIFrameWnd::Move is shared with UIChatRoomWnd::Move by /OPT:ICF and
//   cannot be hooked, so a move by other code (e.g. the saved placement on creation) is followed at the next message
//   or drag instead of at once.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Render/Renderer.h"
#include "UI/Dialogs/UIChooseWnd.h"
#include "UI/Dialogs/UISayDialogWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIFrameWnd.h"
#include "UI/Window/UIWindowMgr.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "NpcDialogLayout";
  constexpr int kMenuGap = 4;
  constexpr int kDialogLift = 40;

  bool g_center, g_attach;

  int (UISayDialogWnd::*g_sayDialogSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  void (CGameMode::*g_receiveMenu)(const unsigned char*);
  void (UIFrameWnd::*g_onMouseMove)(int, int);

  // Moves `window` to (x, y) unless it is already there (Move also re-snaps the window).
  void MoveTo(UIWindow* window, int x, int y) {
    if (window->geometry.x != x || window->geometry.y != y) {
      window->Move(x, y);
    }
  }

  void AttachMenuToDialog() {
    UIWindow* dialog = g_windowMgr.sayDialogWnd;
    UIWindow* menu = g_windowMgr.choose3Wnd;
    if (dialog && menu) {
      MoveTo(menu, dialog->geometry.x, dialog->geometry.y + dialog->geometry.height + kMenuGap);
    }
  }

  void AttachDialogToMenu() {
    UIWindow* dialog = g_windowMgr.sayDialogWnd;
    UIWindow* menu = g_windowMgr.choose3Wnd;
    if (dialog && menu) {
      MoveTo(dialog, menu->geometry.x, menu->geometry.y - dialog->geometry.height - kMenuGap);
    }
  }

  struct SayDialogHook : UISayDialogWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      const int result = (this->*g_sayDialogSendMsg)(sender, message, a, b, c, d);
      if (message == UM_SETSAYDIALOG && this == g_windowMgr.sayDialogWnd) {
        if (g_center && g_renderer && g_renderer->viewport.width > 0 && g_renderer->viewport.height > 0) {
          const int x = (g_renderer->viewport.width - this->geometry.width) / 2;
          const int y = (g_renderer->viewport.height - this->geometry.height) / 2 - kDialogLift;
          MoveTo(this, x, y < 0 ? 0 : y);
        }
        if (g_attach) {
          AttachMenuToDialog();
        }
      }
      return result;
    }
  };

  struct GameModeHook : CGameMode {
    void ReceiveMenuDetour(const unsigned char* packet) {
      (this->*g_receiveMenu)(packet);
      AttachMenuToDialog();
    }
  };

  struct FrameHook : UIFrameWnd {
    void OnMouseMoveDetour(int x, int y) {
      (this->*g_onMouseMove)(x, y);
      const UIFrameWnd* self = this;
      if (g_windowMgr.Capture() != self) {
        return;
      }
      if (self == g_windowMgr.sayDialogWnd) {
        AttachMenuToDialog();
      } else if (self == g_windowMgr.choose3Wnd) {
        AttachDialogToMenu();
      }
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_center = host->SettingBool(kName, "CenterDialog");
  g_attach = host->SettingBool(kName, "AttachMenu");
  return ((!g_center && !g_attach) ||
          ro::HookVirtual(host, "?SendMsg@UISayDialogWnd@@UAEHPAVUIWindow@@HHHHH@Z", &SayDialogHook::SendMsgDetour, g_sayDialogSendMsg, kName)) &&
         (!g_attach || (ro::Hook(host, &CGameMode::ReceiveWindow17Entries, &GameModeHook::ReceiveMenuDetour, g_receiveMenu, kName) &&
                        ro::HookVirtual(host, "?OnMouseMove@UIFrameWnd@@UAEXHH@Z", &FrameHook::OnMouseMoveDetour, g_onMouseMove, kName)));
}
