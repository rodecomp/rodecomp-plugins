// Both searches use shlwapi's StrStrIA, as WARP's shared routine does.
// - Storage: UIItemStoreFindWnd::FilterByName is reimplemented with StrStrIA in place of _mbsstr. (The storage
//   window's own filters, UIItemStoreWnd::FilterByName and UIItemStoreSubWnd::FilterByName2, are folded into one body
//   by /OPT:ICF and cannot be hooked; WARP patches only the _mbsstr search as well.)
// - CashShop: UICashShopWnd::SendMsg runs a copy of the UM_COMMAND/ID_SEARCH case with StrStrIA in place of
//   std::string::find; every other message goes to the original.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Items/ItemInfo.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/MSGSTRINGID.h"
#include "UI/CashShop/UICashShopWnd.h"
#include "UI/Edit/UIEditCtrl.h"
#include "UI/Items/UIItemStoreFindWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"

#include <windows.h>
#include <shlwapi.h>

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <string>

namespace {
  constexpr const char* kName = "CaseInsensitiveSearch";

  bool (UIItemStoreFindWnd::*g_filterByName)(std::string);
  int (UICashShopWnd::*g_cashShopSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  struct FindWndHook : UIItemStoreFindWnd {
    bool FilterByNameDetour(std::string query) {
      if (!this->UpdateItemNames()) {
        return false;
      }
      for (auto it = this->items.begin(); it != this->items.end();) {
        auto next = std::next(it);
        if (StrStrIA(it->displayName.c_str(), query.c_str()) == nullptr) {
          this->items.erase(it);
        }
        it = next;
      }
      return true;
    }
  };

  struct CashShopHook : UICashShopWnd {
    int Search() {
      std::string query(this->m_searchEdit->Text());
      if (query.empty() || query.compare(LookupMessage(MSI_ITEM_SEARCH)) == 0) {
        return 0;
      }
      g_session.ClearInventoryCategoryItems(10);
      for (int list = 0; list < 11; ++list) {
        if (list == 10 || list == 8 || list == 9) {
          continue;
        }
        for (int index = 0; index < g_session.GetInventoryCategorySize(list); ++index) {
          ITEM_INFO item = g_session.GetInventoryCategoryItemInfoBySequence(list, index);
          if (StrStrIA(item.displayName.c_str(), query.c_str()) != nullptr &&
              !g_session.HasInventorySearchItem(std::atoi(item.itemId.c_str()))) {
            g_session.inventoryCategories[10].push_back(item);
          }
        }
      }

      if (g_session.GetInventoryCategorySize(10) > 0) {
        this->m_items.clear();
        for (int index = 0; index < g_session.GetInventoryCategorySize(10); ++index) {
          this->m_items.push_back(g_session.GetInventoryCategoryItemInfoBySequence(10, index));
        }
        this->m_selectedTab = 10;
        this->m_page = 0;
        this->m_pageCount = this->m_items.size() / this->m_itemsPerPage + (this->m_items.size() % this->m_itemsPerPage != 0);
        for (auto tab = this->m_tabButtons.begin(); tab != this->m_tabButtons.end(); ++tab) {
          if (tab->second != nullptr) {
            tab->second->SendMsg(this, UM_SETSTATE, 0, 0, 0, 0);
          }
        }
        this->LayoutPage();
        return 0;
      }

      g_windowMgr.Confirm(LookupMessage(MSI_NOTHING_SEARCH_ITEM), 0, true, false, 0, "Message", 0x118, 0x78, 0);
      return 0;
    }

    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_COMMAND && a == ID_SEARCH) {
        return this->Search();
      }
      return (this->*g_cashShopSendMsg)(sender, message, a, b, c, d);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool storage = host->SettingBool(kName, "Storage");
  const bool cashShop = host->SettingBool(kName, "CashShop");
  if (!storage && !cashShop) {
    host->Log("%s: Storage and CashShop are off, nothing to do", kName);
    return true;
  }
  return (!storage ||
          ro::Hook(host, &UIItemStoreFindWnd::FilterByName, &FindWndHook::FilterByNameDetour, g_filterByName, kName)) &&
         (!cashShop || ro::HookVirtual(host, "?SendMsg@UICashShopWnd@@UAEHPAVUIWindow@@HHHHH@Z", &CashShopHook::SendMsgDetour,
                                       g_cashShopSendMsg, kName));
}
