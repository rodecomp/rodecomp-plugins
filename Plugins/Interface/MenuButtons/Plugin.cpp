// UIMenuIconWnd::OnCreate builds the button grid from a fixed list. After the original:
// - Hide: each listed button (and its notification badge) is removed from the window and its button maps.
// - Custom: each iconName=behavesLike entry adds a UIBitmapButton with bt_<iconName>.bmp / bt_<iconName>_press.bmp and
//   the id kCustomBase + index; the UIMenuIconWnd::SendMsg detour turns UM_COMMAND for that id into the behavesLike
//   button's id. (The stock icon class only adds the hover tooltip, so custom buttons have none.)
// The remaining buttons are then laid out again in the client's order, five per row as OnCreate does, and the window
// grows a row for every five buttons past twenty.
#include "RoPlugin.h"

#include "UI/Buttons/UIBitmapButton.h"
#include "UI/Menu/UIMenuIconWnd.h"
#include "UI/Window/UIEnums.h"
#include "Utility/Text/FormatString.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {
  constexpr const char* kName = "MenuButtons";
  constexpr int kCustomBase = 0x7100;

  struct Button {
    const char* name;
    int id;
  };
  // UIMenuIconWnd::OnCreate's list, in its order, minus booking (never created).
  constexpr Button kButtons[] = {
    {"status", ID_STATUS},
    {"equip", ID_EQUIP},
    {"item", ID_ITEMS},
    {"skill", ID_SKILL},
    {"party", ID_FRIEND},
    {"guild", MENUICON_GUILD},
    {"battle", MENUICON_BATTLE},
    {"quest", ID_QUEST},
    {"map", ID_MAP},
    {"navigation", MENUICON_NAVIGATION},
    {"option", ID_OPTION},
    {"bank", MENUICON_BANK},
    {"rec", MENUICON_REC},
    {"mail", MENUICON_MAIL},
    {"achievement", MENUICON_ACHIEVEMENT},
    {"tip", MENUICON_TIP},
    {"shop", MENUICON_SHOP},
    {"keyboard", ID_OPEN_HOTKEY},
    {"attendance", MENUICON_ATTENDANCE},
    {"adventurerAgency", ID_TOGGLE_PARTY_BOARD},
    {"repute", MENUICON_REPUTE},
    {"adventureguide", MENUICON_ADVENTURE_GUIDE},
    {"probability", MENUICON_PROBABILITY},
  };

  struct Custom {
    std::string icon;
    int target;
  };

  std::vector<int> g_hidden;
  std::vector<Custom> g_custom;

  void (UIMenuIconWnd::*g_onCreate)(int, int);
  int (UIMenuIconWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  int IdOf(const char* name) {
    for (const Button& button : kButtons) {
      if (std::strcmp(name, button.name) == 0) {
        return button.id;
      }
    }
    return 0;
  }

  void Place(UIWindow* button, int position) {
    button->Move(position % 5 * 42 + 9, position / 5 * 43 + 23);
  }

  struct MenuHook : UIMenuIconWnd {
    void Remove(std::map<int, UIButton*>& buttons, int id) {
      auto found = buttons.find(id);
      if (found != buttons.end()) {
        this->RemoveChild(found->second, 1);
        buttons.erase(found);
      }
    }

    void OnCreateDetour(int width, int height) {
      (this->*g_onCreate)(width, height);
      for (int id : g_hidden) {
        this->Remove(this->menuButtons, id);
        this->Remove(this->notificationButtons, id);
      }
      for (std::size_t index = 0; index < g_custom.size(); ++index) {
        const std::string& icon = g_custom[index].icon;
        UIBitmapButton* button = new UIBitmapButton();
        button->SetBitmap(FormatOwnedString("유저인터페이스\\menu_icon\\bt_%s.bmp", icon.c_str()).c_str(), 0, 0);
        button->SetBitmap(FormatOwnedString("유저인터페이스\\menu_icon\\bt_%s.bmp", icon.c_str()).c_str(), 1, 0);
        button->SetBitmap(FormatOwnedString("유저인터페이스\\menu_icon\\bt_%s_press.bmp", icon.c_str()).c_str(), 2, 0);
        button->SetSize(button->BitmapWidth(), button->BitmapHeight());
        button->SetId(kCustomBase + static_cast<int>(index));
        button->Enable(0);
        this->AddChild(button);
        this->menuButtons[kCustomBase + static_cast<int>(index)] = button;
      }

      int position = 0;
      for (const Button& stock : kButtons) {
        auto found = this->menuButtons.find(stock.id);
        if (found == this->menuButtons.end()) {
          continue;
        }
        Place(found->second, position);
        auto badge = this->notificationButtons.find(stock.id);
        if (badge != this->notificationButtons.end()) {
          badge->second->Move(position % 5 * 42 + 9, position / 5 * 43 + 17);
        }
        ++position;
      }
      for (std::size_t index = 0; index < g_custom.size(); ++index) {
        Place(this->menuButtons[kCustomBase + static_cast<int>(index)], position++);
      }
      const int rows = (position + 4) / 5;
      this->Resize(220, rows > 4 ? 33 + rows * 41 : 197);
      this->Invalidate();
    }

    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_COMMAND && a >= kCustomBase && a < kCustomBase + static_cast<int>(g_custom.size())) {
        a = g_custom[a - kCustomBase].target;
      }
      return (this->*g_sendMsg)(sender, message, a, b, c, d);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  for (int index = 0, count = host->SettingCount(kName, "Hide"); index < count; ++index) {
    const char* name = host->SettingItem(kName, "Hide", index);
    if (const int id = IdOf(name)) {
      g_hidden.push_back(id);
    } else {
      host->Log("%s: Hide: unknown button %s, ignored", kName, name);
    }
  }
  for (int index = 0, count = host->SettingCount(kName, "Custom"); index < count; ++index) {
    const std::string entry = host->SettingItem(kName, "Custom", index);
    const std::size_t equals = entry.find('=');
    const std::string icon = equals == std::string::npos ? "" : entry.substr(0, equals);
    const int target = equals == std::string::npos ? 0 : IdOf(entry.substr(equals + 1).c_str());
    if (icon.empty() || target == 0) {
      host->Log("%s: Custom: cannot read %s (expected iconName=behavesLike), ignored", kName, entry.c_str());
      continue;
    }
    g_custom.push_back({icon, target});
  }
  if (g_hidden.empty() && g_custom.empty()) {
    host->Log("%s: Hide and Custom are empty, nothing to do", kName);
    return true;
  }
  return ro::HookVirtual(host, "?OnCreate@UIMenuIconWnd@@UAEXHH@Z", &MenuHook::OnCreateDetour, g_onCreate, kName) &&
         (g_custom.empty() || ro::HookVirtual(host, "?SendMsg@UIMenuIconWnd@@UAEHPAVUIWindow@@HHHHH@Z",
                                              &MenuHook::SendMsgDetour, g_sendMsg, kName));
}
