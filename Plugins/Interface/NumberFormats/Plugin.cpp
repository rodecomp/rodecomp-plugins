// Scopes TextDrawHooks rewriters to three Paints:
// - UIStatusWnd::Paint: the second DrawRightAlignedText of "%d + %d" form (MATK, drawn as m_matk2 + m_matk1) becomes
//   "%d ~ %d" with the same two values, as WARP swaps the format string.
// - UIItemWnd::Paint: the "/%d" limit text (g_session.extraInventorySlots + 100) uses the configured base.
// - UIExpandedBarterItemPurchaseWnd::Paint / UIExpandedBartermarketWnd::Paint: "%d z" gets thousand separators.
#include "RoPlugin.h"
#include "TextDrawHooksApi.h"

#include "Client/Session/Session.h"
#include "UI/Items/UIItemWnd.h"
#include "UI/Shop/UIBartermarketWnd.h"
#include "UI/Status/UIStatusWnd.h"
#include "Utility/Text/GroupedInteger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
  constexpr const char* kName = "NumberFormats";

  enum class Scope { None, Status, Item, Barter };
  Scope g_scope = Scope::None;
  int g_pairIndex;
  int g_inventoryLimit;
  char g_buffer[64];

  void (UIStatusWnd::*g_statusPaint)();
  void (UIItemWnd::*g_itemPaint)();
  void (UIExpandedBarterItemPurchaseWnd::*g_barterPurchasePaint)();
  void (UIExpandedBartermarketWnd::*g_barterMarketPaint)();

  void Rewrite(UIWindow*, bool right, int&, int&, const char*& text, void*) {
    switch (g_scope) {
    case Scope::Status: {
      int base;
      int bonus;
      if (right && std::strstr(text, " + ") != nullptr && std::sscanf(text, "%d + %d", &base, &bonus) == 2 && ++g_pairIndex == 2) {
        std::snprintf(g_buffer, sizeof(g_buffer), "%d ~ %d", base, bonus);
        text = g_buffer;
      }
      break;
    }
    case Scope::Item:
      if (!right && text[0] == '/' && std::atoi(text + 1) == g_session.extraInventorySlots + 100) {
        std::snprintf(g_buffer, sizeof(g_buffer), "/%d", g_session.extraInventorySlots + g_inventoryLimit);
        text = g_buffer;
      }
      break;
    case Scope::Barter: {
      const std::size_t length = std::strlen(text);
      if (right && length > 2 && length < 24 && std::strcmp(text + length - 2, " z") == 0) {
        char grouped[48];
        FormatGroupedInteger(std::atoi(text), grouped, sizeof(grouped));
        std::snprintf(g_buffer, sizeof(g_buffer), "%s z", grouped);
        text = g_buffer;
      }
      break;
    }
    case Scope::None:
      break;
    }
  }

  struct ScopeGuard {
    explicit ScopeGuard(Scope scope) { g_scope = scope; g_pairIndex = 0; }
    ~ScopeGuard() { g_scope = Scope::None; }
  };

  struct StatusHook : UIStatusWnd {
    void PaintDetour() { ScopeGuard guard(Scope::Status); (this->*g_statusPaint)(); }
  };
  struct ItemHook : UIItemWnd {
    void PaintDetour() { ScopeGuard guard(Scope::Item); (this->*g_itemPaint)(); }
  };
  struct BarterPurchaseHook : UIExpandedBarterItemPurchaseWnd {
    void PaintDetour() { ScopeGuard guard(Scope::Barter); (this->*g_barterPurchasePaint)(); }
  };
  struct BarterMarketHook : UIExpandedBartermarketWnd {
    void PaintDetour() { ScopeGuard guard(Scope::Barter); (this->*g_barterMarketPaint)(); }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool matk = host->SettingBool(kName, "TildeForMatk");
  g_inventoryLimit = static_cast<int>(host->SettingInt(kName, "InventoryLimit"));
  const bool barter = host->SettingBool(kName, "BarterZenySeparators");
  if (!matk && g_inventoryLimit == 0 && !barter) {
    host->Log("%s: every setting is off, nothing to do", kName);
    return true;
  }
  return TextDrawHooks_AddRewriter(&Rewrite, nullptr) &&
         (!matk || ro::HookVirtual(host, "?Paint@UIStatusWnd@@UAEXXZ", &StatusHook::PaintDetour, g_statusPaint, kName)) &&
         (g_inventoryLimit == 0 || ro::HookVirtual(host, "?Paint@UIItemWnd@@UAEXXZ", &ItemHook::PaintDetour, g_itemPaint, kName)) &&
         (!barter || (ro::HookVirtual(host, "?Paint@UIExpandedBarterItemPurchaseWnd@@UAEXXZ", &BarterPurchaseHook::PaintDetour, g_barterPurchasePaint, kName) &&
                      ro::HookVirtual(host, "?Paint@UIExpandedBartermarketWnd@@UAEXXZ", &BarterMarketHook::PaintDetour, g_barterMarketPaint, kName)));
}
