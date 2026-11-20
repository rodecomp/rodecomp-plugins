// Hooks per setting:
// - PartyLimit: UIMessengerGroupWnd::Paint marks the window; while it paints, a TextDrawHooks rewriter (DrawTextAt
//   only) rewrites the "<count>/12" member counter to "<count>/PartyLimit".
// - NoAdventurerAgency: after UIMessengerGroupWnd::OnCreate, button 0 (the Adventurer Agency, id 544) is flagged in
//   hiddenButtons, so LayoutButtons moves it off screen in party mode and shifts the others left, as it does for the
//   window's other hidden buttons.
// - NoEquipSwap / NoEquipAttribute: after UIEquipWnd::OnCreate the swap button (508) / attribute button (582) is moved
//   off screen (the window only toggles their visibility afterwards), and UIEquipWnd::SendMsg drops UM_COMMAND 508 /
//   582, including the swap OnCreate replays when swap mode was saved.
// - NoEquipTitleTab: while UIEquipWnd::OnCreate runs, the UITabControl::AddTab detour skips the Title tab; UIEquipWnd::
//   SendMsg maps UM_TAB index 2 and above one up, so the damage skin tab (now index 2) still opens page 3, as WARP's
//   inc ecx does.
// - TraitButton: UIStatusWnd::SendMsg drops UM_COMMAND 550 (the trait expand button); Hidden also moves the button off
//   screen after UIStatusWnd::OnCreate.
#include "RoPlugin.h"
#include "TextDrawHooksApi.h"

#include "Client/Session/Session.h"
#include "Contents/Messenger/MessengerWindows.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/MSGSTRINGID.h"
#include "UI/Buttons/UIBitmapButton.h"
#include "UI/Buttons/UIBmpTextButton.h"
#include "UI/Items/UIEquipWnd.h"
#include "UI/Lists/UITabControl.h"
#include "UI/Status/UIStatusWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindow.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
  constexpr const char* kName = "WindowButtons";
  constexpr int kEquipSwap = 508;
  constexpr int kEquipAttribute = 582;
  constexpr int kTraitButton = 550;

  int g_partyLimit;
  bool g_noAgency;
  bool g_noSwap;
  bool g_noTitleTab;
  bool g_noAttribute;
  bool g_traitDisabled;
  bool g_traitHidden;

  bool g_inPartyPaint;
  bool g_inEquipCreate;

  void (UIMessengerGroupWnd::*g_partyPaint)();
  void (UIMessengerGroupWnd::*g_partyOnCreate)(int, int);
  void (UIEquipWnd::*g_equipOnCreate)(int, int);
  int (UIEquipWnd::*g_equipSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  void (UITabControl::*g_addTab)(const char*, const char*);
  void (UIStatusWnd::*g_statusOnCreate)(int, int);
  int (UIStatusWnd::*g_statusSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  char g_counter[16];

  void Rewrite(UIWindow*, bool right, int&, int&, const char*& text, void*) {
    if (!g_inPartyPaint || right) {
      return;
    }
    char counter[16];
    std::sprintf(counter, "%d/%d", g_session.PartyCount(), 12);
    if (std::strcmp(text, counter) == 0) {
      std::sprintf(g_counter, "%d/%d", g_session.PartyCount(), g_partyLimit);
      text = g_counter;
    }
  }

  struct PartyHook : UIMessengerGroupWnd {
    void PaintDetour() {
      g_inPartyPaint = true;
      (this->*g_partyPaint)();
      g_inPartyPaint = false;
    }

    void OnCreateDetour(int width, int height) {
      (this->*g_partyOnCreate)(width, height);
      this->hiddenButtons[0] = 1;
      this->LayoutButtons();
    }
  };

  struct TabHook : UITabControl {
    void AddTabDetour(const char* label, const char* tooltip) {
      if (g_inEquipCreate && label != nullptr && std::strcmp(label, LookupMessage(MSI_TITLE_EQUIPMENT)) == 0) {
        return;
      }
      (this->*g_addTab)(label, tooltip);
    }
  };

  struct EquipHook : UIEquipWnd {
    void OnCreateDetour(int width, int height) {
      g_inEquipCreate = true;
      (this->*g_equipOnCreate)(width, height);
      g_inEquipCreate = false;
      if (g_noSwap && this->swapButton != nullptr) {
        this->swapButton->Move(-100, -100);
      }
      if (g_noAttribute && this->abilityButton != nullptr) {
        this->abilityButton->Move(-100, -100);
      }
    }

    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_COMMAND && ((g_noSwap && a == kEquipSwap) || (g_noAttribute && a == kEquipAttribute))) {
        return 0;
      }
      if (g_noTitleTab && message == UM_TAB && this->m_viewOther == 0 && a >= 2) {
        ++a;
      }
      return (this->*g_equipSendMsg)(sender, message, a, b, c, d);
    }
  };

  struct StatusHook : UIStatusWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_statusOnCreate)(width, height);
      this->m_expandButton->Move(-100, -100);
    }

    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_COMMAND && a == kTraitButton) {
        return 0;
      }
      return (this->*g_statusSendMsg)(sender, message, a, b, c, d);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_partyLimit = static_cast<int>(host->SettingInt(kName, "PartyLimit"));
  g_noAgency = host->SettingBool(kName, "NoAdventurerAgency");
  g_noSwap = host->SettingBool(kName, "NoEquipSwap");
  g_noTitleTab = host->SettingBool(kName, "NoEquipTitleTab");
  g_noAttribute = host->SettingBool(kName, "NoEquipAttribute");
  const char* trait = host->SettingString(kName, "TraitButton");
  g_traitHidden = std::strcmp(trait, "Hidden") == 0;
  g_traitDisabled = g_traitHidden || std::strcmp(trait, "Disabled") == 0;

  const bool party = g_partyLimit != 12;
  const bool equipCreate = g_noSwap || g_noTitleTab || g_noAttribute;
  if (!party && !g_noAgency && !equipCreate && !g_traitDisabled) {
    host->Log("%s: every setting is at the client's value, nothing to do", kName);
    return true;
  }
  return (!party ||
          (ro::HookVirtual(host, "?Paint@UIMessengerGroupWnd@@UAEXXZ", &PartyHook::PaintDetour, g_partyPaint, kName) &&
           TextDrawHooks_AddRewriter(&Rewrite, nullptr))) &&
         (!g_noAgency || ro::HookVirtual(host, "?OnCreate@UIMessengerGroupWnd@@UAEXHH@Z", &PartyHook::OnCreateDetour,
                                         g_partyOnCreate, kName)) &&
         (!equipCreate ||
          (ro::HookVirtual(host, "?OnCreate@UIEquipWnd@@UAEXHH@Z", &EquipHook::OnCreateDetour, g_equipOnCreate, kName) &&
           ro::HookVirtual(host, "?SendMsg@UIEquipWnd@@UAEHPAVUIWindow@@HHHHH@Z", &EquipHook::SendMsgDetour, g_equipSendMsg,
                           kName))) &&
         (!g_noTitleTab || ro::Hook(host, &UITabControl::AddTab, &TabHook::AddTabDetour, g_addTab, kName)) &&
         (!g_traitDisabled || ro::HookVirtual(host, "?SendMsg@UIStatusWnd@@UAEHPAVUIWindow@@HHHHH@Z",
                                              &StatusHook::SendMsgDetour, g_statusSendMsg, kName)) &&
         (!g_traitHidden || ro::HookVirtual(host, "?OnCreate@UIStatusWnd@@UAEXHH@Z", &StatusHook::OnCreateDetour,
                                            g_statusOnCreate, kName));
}
