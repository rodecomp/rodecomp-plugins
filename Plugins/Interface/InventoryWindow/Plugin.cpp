// - MaxWidth/MaxHeight: UIItemWnd::SendMsg(UM_RESIZE, 7) clamps to 320x240; the case is reimplemented with the
//   configured maximum (the rest of the body is rodecomp's).
// - ExpandLimit: CGameMode::ReceiveExtraInventorySlots ignores a value above 100; values up to the limit are stored and
//   the inventory refreshed. (The login-mode copy of that packet's handler is inlined in CLoginMode::PollNetworkStatus
//   and keeps the 100 cap; the in-game packet sets it again on map entry.)
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Session/Session.h"
#include "UI/Buttons/UIBitmapButton.h"
#include "UI/Buttons/UICheckBox.h"
#include "UI/Items/UIItemWnd.h"
#include "UI/Lists/UIScrollBar.h"
#include "UI/Lists/UITabControl.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIResizer.h"
#include "UI/Window/UIWindowMgr.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "InventoryWindow";

  int g_maxWidth;
  int g_maxHeight;
  int g_expandLimit;

  int (UIItemWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  void (CGameMode::*g_receiveSlots)(const unsigned char*);

  struct ItemWndHook : UIItemWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message != UM_RESIZE || a != 7) {
        return (this->*g_sendMsg)(sender, message, a, b, c, d);
      }
      int width = b <= g_maxWidth ? static_cast<int>(b) : g_maxWidth;
      int height = c <= g_maxHeight ? static_cast<int>(c) : g_maxHeight;
      if (width < 232) {
        width = 232;
      }
      if (height < 102) {
        height = 102;
      }
      const int tabHeight = this->tabs->geometry.height;
      if (height < tabHeight + 38) {
        height = tabHeight + 38;
      }
      const int rowsHeight = tabHeight + (height - tabHeight - 38) / 32 * 32;
      const int newWidth = (width - 280) / 32 * 32 + 280;
      if (newWidth != this->geometry.width || rowsHeight + 38 != this->geometry.height) {
        this->Resize(newWidth, rowsHeight + 38);
        this->columns = (newWidth - 40) / 32;
        this->visibleRows = rowsHeight / 32;
        this->resizer->Move(this->geometry.width - 15, this->geometry.height - 15);
        this->scrollBar->Move(this->geometry.width - 15, 17);
        this->scrollBar->Resize(this->scrollBar->geometry.width, this->geometry.height - 38);
        this->UpdateScrollBar();
        this->Invalidate();
        const int positions[3] = {3, this->geometry.width - 28, this->geometry.width - 14};
        for (int index = 0; index < 3; ++index) {
          this->titleButtons[index]->Move(positions[index], 3);
        }
      }
      this->expandedHeight = this->geometry.height;
      g_windowMgr.StoreSnapRectangle(this);
      this->lockDropBox->Move(79, this->geometry.height - 16);
      this->optionBox->Move(103, this->geometry.height - 17);
      this->countButton->Move(23, this->geometry.height - 17);
      this->dealLockButton->Move(124, this->geometry.height - 19);
      if (this->filterWindow) {
        this->filterWindow->Move(30, this->geometry.height - 50);
      }
      this->filterButton->Move(157, this->geometry.height - 19);
      return 0;
    }
  };

  struct GameModeHook : CGameMode {
    void ReceiveSlotsDetour(const unsigned char* data) {
      const std::int16_t value = *reinterpret_cast<const std::int16_t*>(data + 2);
      if (value > 100 && value <= g_expandLimit) {
        g_session.extraInventorySlots = value;
        if (g_windowMgr.itemWnd) {
          static_cast<UIWindow*>(g_windowMgr.itemWnd)->SendMsg(nullptr, UM_REFRESHITEM, 0, 0, 0, 0);
        }
        return;
      }
      (this->*g_receiveSlots)(data);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_maxWidth = static_cast<int>(host->SettingInt(kName, "MaxWidth"));
  g_maxHeight = static_cast<int>(host->SettingInt(kName, "MaxHeight"));
  g_expandLimit = static_cast<int>(host->SettingInt(kName, "ExpandLimit"));
  return ((g_maxWidth == 320 && g_maxHeight == 240) ||
          ro::HookVirtual(host, "?SendMsg@UIItemWnd@@UAEHPAVUIWindow@@HHHHH@Z", &ItemWndHook::SendMsgDetour, g_sendMsg, kName)) &&
         (g_expandLimit == 100 || ro::Hook(host, &CGameMode::ReceiveExtraInventorySlots, &GameModeHook::ReceiveSlotsDetour, g_receiveSlots, kName));
}
