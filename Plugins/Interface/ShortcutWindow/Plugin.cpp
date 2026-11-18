// - AllItems: inside UIShortCutWnd::DragAndDrop, CSession::LookupClientItemByIdentity reports a non-wearable item
//   (identity[2] == 0) of an unlisted type as TYPE_SPECIAL, the case the drop switch accepts, as WARP sends every type
//   to the same case. Using it later goes through MM_USEITEM.
// - HideItemBar: after UIShortCutWnd::OnCreate and SendMsg the private m_showItems/m_hideItems (0x184/0x188, the last
//   two fields; sizeof is pinned to 0x18C) are made invisible.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Items/ItemType.h"
#include "UI/Shortcuts/UIShortCutWnd.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "ShortcutWindow";

  bool g_inDrop;

  void (UIShortCutWnd::*g_dragAndDrop)(int, int, std::intptr_t);
  void (UIShortCutWnd::*g_onCreate)(int, int);
  int (UIShortCutWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  ITEM_INFO (CSession::*g_lookup)(std::uint32_t) const;

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
      auto* base = reinterpret_cast<unsigned char*>(this);
      for (const int offset : {0x184, 0x188}) {
        if (UIWindow* button = *reinterpret_cast<UIWindow**>(base + offset)) {
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
  const bool all = host->SettingBool(kName, "AllItems");
  const bool hide = host->SettingBool(kName, "HideItemBar");
  return (!all || (ro::HookVirtual(host, "?DragAndDrop@UIShortCutWnd@@UAEXHHH@Z", &ShortcutHook::DragAndDropDetour, g_dragAndDrop, kName) &&
                   ro::Hook(host, &CSession::LookupClientItemByIdentity, &SessionHook::LookupDetour, g_lookup, kName))) &&
         (!hide || (ro::HookVirtual(host, "?OnCreate@UIShortCutWnd@@UAEXHH@Z", &ShortcutHook::OnCreateDetour, g_onCreate, kName) &&
                    ro::HookVirtual(host, "?SendMsg@UIShortCutWnd@@UAEHPAVUIWindow@@HHHHH@Z", &ShortcutHook::SendMsgDetour, g_sendMsg, kName)));
}
