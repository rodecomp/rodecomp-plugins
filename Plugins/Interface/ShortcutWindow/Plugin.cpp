// - RestoreAfterLoad: CCharacterLinkedUserDataMgr::LoadNormalItemSlots reads the saved shortcut bar (geometry, shown,
//   ItemSlotActive) into g_windowMgr.m_normalItemSlotWindow but does not apply it to an open shortcut window. When the
//   save has a NormalItemSlot list (the original returns early otherwise), the detour then does what WARP
//   FixSkillbarReset does: moves and resizes WID_SHORTCUTWND to the saved geometry, shows or hides it, sends it
//   UM_SETINFO with the saved window and UM_COMMAND 588/589 (item slots on/off).
// - AllItems: inside UIShortCutWnd::DragAndDrop, CSession::LookupClientItemByIdentity reports a non-wearable item
//   (identity[2] == 0) of an unlisted type as TYPE_SPECIAL, the case the drop switch accepts, as WARP sends every type
//   to the same case. Using it later goes through MM_USEITEM.
// - HideItemBar: after UIShortCutWnd::OnCreate and SendMsg the private m_showItems/m_hideItems (0x184/0x188, the last
//   two fields; sizeof is pinned to 0x18C) are made invisible.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Items/ItemType.h"
#include "Contents/UserData/CharacterLinkedUserDataMgr.h"
#include "UI/Buttons/UIBitmapButton.h"
#include "UI/Shortcuts/UIShortCutWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

#include <cstdint>

// Plugins get /I rodecomp/Source only; jsoncpp's headers include each other by relative path.
#include "../ThirdParty/jsoncpp/include/json/value.h"

namespace {
  constexpr const char* kName = "ShortcutWindow";

  bool g_inDrop;

  void (UIShortCutWnd::*g_dragAndDrop)(int, int, std::intptr_t);
  void (UIShortCutWnd::*g_onCreate)(int, int);
  int (UIShortCutWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  ITEM_INFO (CSession::*g_lookup)(std::uint32_t) const;
  void (CCharacterLinkedUserDataMgr::*g_loadNormalItemSlots)(Json::Value&);

  struct UserDataHook : CCharacterLinkedUserDataMgr {
    void LoadNormalItemSlotsDetour(Json::Value& data) {
      (this->*g_loadNormalItemSlots)(data);
      const Json::Value& root = data;
      if (root["NormalItemSlot"].isNull()) {
        return;
      }
      UIWindow* window = g_windowMgr.LookupWindow(WID_SHORTCUTWND);
      if (!window) {
        return;
      }
      NormalItemSlotWindow& saved = g_windowMgr.m_normalItemSlotWindow;
      window->geometry.x = saved.x;
      window->geometry.y = saved.y;
      window->geometry.width = saved.width;
      window->geometry.height = saved.height;
      g_windowMgr.ShowWindowById(WID_SHORTCUTWND, saved.show);
      window->SendMsg(nullptr, UM_SETINFO, reinterpret_cast<std::intptr_t>(&saved), 0, 0, 0);
      window->SendMsg(nullptr, UM_COMMAND, g_session.ItemSlotActive() ? 588 : 589, 0, 0, 0);
    }
  };

  bool Accepted(std::uint32_t type) {
    switch (type) {
    case TYPE_HEAL: case TYPE_SCHANGE: case TYPE_SPECIAL: case TYPE_ARROW: case TYPE_AMMO: case TYPE_THROWWEAPON:
    case TYPE_CANNONBALL: case TYPE_EVENT:
      return true;
    default:
      return false;
    }
  }

  struct SessionHook : CSession {
    ITEM_INFO LookupDetour(std::uint32_t id) const {
      ITEM_INFO item = (this->*g_lookup)(id);
      if (g_inDrop && item.identity[2] == 0 && !Accepted(item.identity[0])) {
        item.identity[0] = TYPE_SPECIAL;
      }
      return item;
    }
  };

  struct ShortcutHook : UIShortCutWnd {
    void HideButtons() {
      for (UIBitmapButton* button : {this->m_showItems, this->m_hideItems}) {
        if (button != nullptr) {
          button->geometry.visible = 0;
        }
      }
    }
    void DragAndDropDetour(int x, int y, std::intptr_t value) {
      g_inDrop = true;
      (this->*g_dragAndDrop)(x, y, value);
      g_inDrop = false;
    }
    void OnCreateDetour(int width, int height) {
      (this->*g_onCreate)(width, height);
      this->HideButtons();
    }
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      const int result = (this->*g_sendMsg)(sender, message, a, b, c, d);
      this->HideButtons();
      return result;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool restore = host->SettingBool(kName, "RestoreAfterLoad");
  const bool all = host->SettingBool(kName, "AllItems");
  const bool hide = host->SettingBool(kName, "HideItemBar");
  return (!restore || ro::Hook(host, &CCharacterLinkedUserDataMgr::LoadNormalItemSlots, &UserDataHook::LoadNormalItemSlotsDetour,
                               g_loadNormalItemSlots, kName)) &&
         (!all || (ro::HookVirtual(host, "?DragAndDrop@UIShortCutWnd@@UAEXHHH@Z", &ShortcutHook::DragAndDropDetour, g_dragAndDrop, kName) &&
                   ro::Hook(host, &CSession::LookupClientItemByIdentity, &SessionHook::LookupDetour, g_lookup, kName))) &&
         (!hide || (ro::HookVirtual(host, "?OnCreate@UIShortCutWnd@@UAEXHH@Z", &ShortcutHook::OnCreateDetour, g_onCreate, kName) &&
                    ro::HookVirtual(host, "?SendMsg@UIShortCutWnd@@UAEHPAVUIWindow@@HHHHH@Z", &ShortcutHook::SendMsgDetour, g_sendMsg, kName)));
}
