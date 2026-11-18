// Hooks per setting:
// - MoveIcon: UIWindowMgr::EnsureWindow, for WID_NCASH_CALLWND when it creates the icon window (nCashCallWnd was
//   null), moves it to IconX, IconY after the original; a negative coordinate counts from the right/bottom edge of the
//   viewport, as WARP's does.
// - ZeroSecondaryPoints: UICashShopWnd::Paint runs with m_secondaryPoints set to 0, restored afterwards, so the
//   "%d C" line next to the refresh label shows 0 C.
// - PreviewInShop: CGameMode::ReceiveTypedItemList (ZC_ACK_SCHEDULER_CASHITEM) reads 8-byte records (item id, price).
//   With the setting the detour parses the packet itself, with WARP's 14-byte records that add the view sprite (+8) and
//   the equip location (+0Ah), stored in the item (sprite, identity[2]) for the equipment preview. The original is
//   not called: its stride would misread the packet.
// - PreviewInTrader: CGameMode::ReceivePendingItem1718List (ZC_PC_CASH_POINT_ITEMLIST) reads 0x0D-byte records. With
//   the setting the detour parses WARP's 0x19-byte records, which add the sprite (+0Dh) and location (+0Fh). They go
//   into RawItemBasic, which CSession::AddShopItem copies into the shop item. (WARP writes them into an unnamed list at
//   0x015D4818 instead; Gravity's source has no name for it.) The original is not called.
#include "RoPlugin.h"

#include "Client/Application/Launcher.h"
#include "Client/Mode/CGameMode.h"
#include "Client/Session/Session.h"
#include "Contents/Items/ItemInfo.h"
#include "Render/Renderer.h"
#include "UI/CashShop/UICashShopWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindow.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"

#include <cstdint>
#include <cstdlib>

namespace {
  constexpr const char* kName = "CashShop";

  int g_iconX;
  int g_iconY;

#pragma pack(push, 1)
  struct PreviewTypedItem {
    /* 0000 */ int itemId;
    /* 0004 */ std::uint32_t price;
    /* 0008 */ std::uint16_t viewSprite;
    /* 000A */ std::uint32_t location;
  };

  struct PreviewTypedItemList {
    /* 0000 */ std::int16_t id;
    /* 0002 */ std::int16_t length;
    /* 0004 */ std::int16_t count;
    /* 0006 */ std::int16_t type;
    /* 0008 */ PreviewTypedItem entries[1];
  };

  struct PreviewTraderItem {
    /* 0000 */ std::uint32_t price;
    /* 0004 */ std::uint32_t realPrice;
    /* 0008 */ unsigned char type;
    /* 0009 */ std::int32_t itemId;
    /* 000D */ std::uint16_t sprite;
    /* 000F */ std::uint32_t location;
    /* 0013 */ unsigned char padding_0013[6];
  };

  struct PreviewTraderItemList {
    /* 0000 */ std::int16_t packetType;
    /* 0002 */ std::int16_t packetLength;
    /* 0004 */ int cashPoints;
    /* 0008 */ int kafraPoints;
    /* 000C */ PreviewTraderItem items[1];
  };
#pragma pack(pop)
  static_assert(sizeof(PreviewTypedItem) == 14);
  static_assert(sizeof(PreviewTraderItem) == 0x19);

  UIWindow* (UIWindowMgr::*g_ensureWindow)(int);
  void (UICashShopWnd::*g_paint)();
  void (CGameMode::*g_receiveTypedItemList)(const unsigned char*);
  void (CGameMode::*g_receivePendingItem1718List)(const unsigned char*);

  struct WindowMgrHook : UIWindowMgr {
    UIWindow* EnsureWindowDetour(int id) {
      const bool created = id == WID_NCASH_CALLWND && this->nCashCallWnd == nullptr;
      UIWindow* window = (this->*g_ensureWindow)(id);
      if (created && window != nullptr) {
        const int x = g_iconX < 0 ? g_renderer->viewport.width + g_iconX : g_iconX;
        const int y = g_iconY < 0 ? g_renderer->viewport.height + g_iconY : g_iconY;
        window->Move(x, y);
      }
      return window;
    }
  };

  struct CashShopHook : UICashShopWnd {
    void PaintDetour() {
      const int points = this->m_secondaryPoints;
      this->m_secondaryPoints = 0;
      (this->*g_paint)();
      this->m_secondaryPoints = points;
    }
  };

  struct GameModeHook : CGameMode {
    void ReceiveTypedItemListDetour(const unsigned char* data) {
      const auto* packet = reinterpret_cast<const PreviewTypedItemList*>(data);
      if (g_launchPassCommandLine) {
        return;
      }
      for (int i = 0; i < packet->count; ++i) {
        const PreviewTypedItem& entry = packet->entries[i];
        char name[256];
        ITEM_INFO item;
        item.identity[1] = -1;
        item.prices[0] = entry.price;
        item.prices[1] = entry.price;
        item.identity[0] = 4;
        item.identified = 1;
        item.damaged = 0;
        item.refiningLevel = 0;
        std::uint16_t type = packet->type;
        if (type == 8) {
          type = 0xFFFF;
        }
        item.sprite = entry.viewSprite;
        item.identity[2] = entry.location;
        item.bindOnEquip = type;
        item.SetItemId(entry.itemId);
        int capacity = sizeof(name);
        item.GetIdDisplayName(name, capacity, 0);
        item.SetName(name);
        if (static_cast<unsigned int>(packet->type) <= 10) {
          g_session.inventoryCategories[packet->type].push_back(item);
        }
      }
    }

    void ReceivePendingItem1718ListDetour(const unsigned char* data) {
      const auto* packet = reinterpret_cast<const PreviewTraderItemList*>(data);
      if (g_session.m_channelCopId == 1 || g_windowMgr.nCashWnd || g_windowMgr.LookupWindow(WID_CASHSHOP_WND)) {
        // The original's early returns (the channel notice included) do not depend on the record size.
        (this->*g_receivePendingItem1718List)(data);
        return;
      }
      g_session.ClearTradeItems();
      const int count = (packet->packetLength - 12) / static_cast<int>(sizeof(PreviewTraderItem));
      for (int i = 0; i < count; ++i) {
        const PreviewTraderItem& entry = packet->items[i];
        RawItemBasic item = {};
        _itoa(entry.itemId, item.itemId, 10);
        item.price = entry.price;
        item.realPrice = entry.realPrice;
        item.type = entry.type;
        item.id = -1;
        item.identified = 1;
        item.sprite = entry.sprite;
        item.location = entry.location;
        g_session.AddShopItem(item, 0);
      }
      g_session.m_pendingAid = packet->cashPoints;
      g_session.m_pendingGid = packet->kafraPoints;
      this->isOnQuest = 1;

      UIWindow* window = g_windowMgr.EnsureWindow(WID_ITEMSHOPWND);
      window->SendMsg(nullptr, UM_SETTITLE, 0, 1, 0, 0);
      window->SendMsg(nullptr, UM_REFRESHITEM, 0, 0, 0, 0);
      window = g_windowMgr.EnsureWindow(WID_ITEMPURCHASEWND);
      window->SendMsg(nullptr, UM_SETTITLE, 1, 0, 0, 0);
      window->SendMsg(nullptr, UM_REFRESHITEM, 0, 0, 0, 0);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool moveIcon = host->SettingBool(kName, "MoveIcon");
  const bool zero = host->SettingBool(kName, "ZeroSecondaryPoints");
  const bool previewShop = host->SettingBool(kName, "PreviewInShop");
  const bool previewTrader = host->SettingBool(kName, "PreviewInTrader");
  g_iconX = static_cast<int>(host->SettingInt(kName, "IconX"));
  g_iconY = static_cast<int>(host->SettingInt(kName, "IconY"));
  if (!moveIcon && !zero && !previewShop && !previewTrader) {
    host->Log("%s: every setting is off, nothing to do", kName);
    return true;
  }
  return (!moveIcon ||
          ro::Hook(host, &UIWindowMgr::EnsureWindow, &WindowMgrHook::EnsureWindowDetour, g_ensureWindow, kName)) &&
         (!zero || ro::HookVirtual(host, "?Paint@UICashShopWnd@@UAEXXZ", &CashShopHook::PaintDetour, g_paint, kName)) &&
         (!previewShop || ro::Hook(host, &CGameMode::ReceiveTypedItemList, &GameModeHook::ReceiveTypedItemListDetour,
                                   g_receiveTypedItemList, kName)) &&
         (!previewTrader || ro::Hook(host, &CGameMode::ReceivePendingItem1718List,
                                     &GameModeHook::ReceivePendingItem1718ListDetour, g_receivePendingItem1718List, kName));
}
