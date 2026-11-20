// Small UI rules; each setting installs its own hooks, only when it differs from the client:
// - RodexTax: UIRodexComposeWnd::Paint charges 2500 zeny per attached item (plus 2% of the zeny sent) and draws it.
//   After the original Paint the detour recomputes m_tax with the setting, which the send checks read, and redraws the
//   tax box and the zeny field colour over the original's.
// - GuildExpLimit: UIGuildPositionManageWnd::SendMsg clamps a position's tax (exp donation) to 0..50; the detour handles
//   that edit (UM_COMMAND, ID_PAYRATE_EDIT) with the setting as limit, then runs the original's common tail.
// - AgencyLevelRange: the adventurer agency windows fill their level boxes with level-5..level+5. UIRegisterPartyWnd
//   (OnCreate, NormalizeLevels) and UIAdvenPartyBoardWnd (OnCreate, then reset and search in SendMsg; ResetSearch and
//   Search are inlined) get the setting's offsets instead.
// - EnglishArrows: UIHotkeyGuideWnd::OnCreate labels the arrow and menu keys with Korean double-byte symbols; the detour
//   relabels them "<", "^", ">", "v" and "=" as WARP does.
// - MvpDropIdentified: the ZC_MVP_GETTING_ITEM handler (inside CGameMode::ProcessIncomingPackets) names the drop with
//   an unidentified ITEM_INFO. It looks up MSI_YOU_RECEIVE_MVP_ITEM2 (used nowhere else) just before calling
//   ITEM_INFO::DisplayName, so the LookupMessage detour arms a flag that the next DisplayName call consumes, naming a
//   copy of the item marked identified.
// - HideZeroLastLogin: UIGuildMemberManageWnd::DrawMember formats a member's last login with
//   strftime(LookupMessage(MSI_TIME_TYPE4), _localtime32(&lastOfflineTime)), so 0 shows 1970-01-01. For such a member
//   the detour makes the LookupMessage detour return "" for MSI_TIME_TYPE4 (an empty date, as WARP writes) and sets
//   lastOfflineTime to one day while the original runs, so _localtime32 never gets a time before 1970 in a timezone
//   west of UTC (it would return null).
// - CloseCutinOnEsc: UIWindowMgr::ProcessHotkeyAction's Esc branch first handles the chat suggestions, a hovered
//   chat-tab or retained child, a modal window and replay; then WARP closes the NPC cutin (WID_ILLUSTWND) and clears
//   the shown image (MM_SHOW_IMAGE "", -1, 1) instead of closing the top window. The detour runs those same checks
//   and, when none applies and the cutin was open, does that and returns; otherwise the original runs.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Client/Replay/Replay.h"
#include "Client/Session/Session.h"
#include "Contents/Guild/GuildInfo.h"
#include "Contents/Items/ItemInfo.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/Bitmap/BitmapResource.h"
#include "Resource/Bitmap/TextureNames.h"
#include "Resource/MSGSTRINGID.h"
#include "Resource/ResourceMgr.h"
#include "UI/Buttons/UIBitmapButton.h"
#include "UI/Chat/UINewChatWnd.h"
#include "UI/Edit/UIEditCtrl.h"
#include "UI/Edit/UIEditCtrl_Currency.h"
#include "UI/GameSettings/UIHotkeyGuideWnd.h"
#include "UI/Guild/UIGuildWnd.h"
#include "UI/Party/UIAdvenPartyBoardWnd.h"
#include "UI/Party/UIRegisterPartyWnd.h"
#include "UI/Rodex/UIRodexComposeWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"
#include "Utility/Text/FormatString.h"
#include "Utility/Text/GroupedInteger.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
  constexpr const char* kName = "UiTweaks";
  constexpr int kClientRodexTax = 2500;
  constexpr int kClientGuildExpLimit = 50;
  constexpr int kClientAgencyRange = 5;

  int g_rodexTax;
  int g_guildExpLimit;
  int g_agencyBelow;
  int g_agencyAbove;
  bool g_mvp;
  bool g_mvpPending;
  bool g_zeroLastLogin;

  // ---- RodexTax ----
  void (UIRodexComposeWnd::*g_rodexPaint)();

  struct RodexHook : UIRodexComposeWnd {
    void PaintDetour() {
      (this->*g_rodexPaint)();
      if (this->m_zeny->Text() == nullptr) {
        return;
      }
      char inputBuf[260] = {};
      strcpy_s(inputBuf, sizeof(inputBuf), this->m_zeny->Text());
      g_session.IsNumberWithCommas(inputBuf);
      const std::int64_t inputZeny = _atoi64(inputBuf);
      const std::int64_t tax = static_cast<std::int64_t>(g_session.CountSelectionItems()) * g_rodexTax + (inputZeny * 2) / 100;
      this->m_tax = tax;

      this->DrawBitmap(9, 377, static_cast<CBitmapRes*>(ResourceMgr::Instance().Load(Resolve("유저인터페이스\\basic_interface\\rodexsystem\\renewal\\bg_tax.bmp"))), true);
      char taxBuf[260] = {};
      FormatGroupedInteger(tax, taxBuf, sizeof(taxBuf));
      const std::uint32_t taxColor = (inputZeny + tax > g_session.m_zeny) ? 0x4E71CF : 0;
      this->DrawRightAlignedText(104, 382, taxBuf, 0, 0, 11, taxColor, 0);
      if (this->m_zeny->Length() != 0) {
        if (inputZeny > g_session.m_zeny || inputZeny + tax > g_session.m_zeny) {
          this->m_zeny->TextColor(0xFF, 0, 0, 0xFFFFFFFF);
        } else {
          this->m_zeny->TextColor(0, 0, 0, 0xFFFFFFFF);
        }
      }
    }
  };

  // ---- GuildExpLimit ----
  int (UIGuildPositionManageWnd::*g_guildSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  struct GuildPositionHook : UIGuildPositionManageWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a1, std::intptr_t a2, std::intptr_t a3, std::intptr_t a4) {
      if (message != UM_COMMAND || a1 != ID_PAYRATE_EDIT) {
        return (this->*g_guildSendMsg)(sender, message, a1, a2, a3, a4);
      }
      if (!this->taxEdits[0]) {
        return 0;
      }
      int row = static_cast<int>(a1) - 276;
      for (int index = 0; index < 11; ++index) {
        if (this->taxEdits[index] == sender) {
          row = index;
          break;
        }
      }
      int found;
      GuildPositionEntry& position = this->PositionAt(this->m_scrollTop + row, &found);
      if (found) {
        position.tax = std::atoi(this->taxEdits[row]->Text());
        if (position.tax < 0 || position.tax > g_guildExpLimit) {
          g_windowMgr.SendNotice(UIM_PUSHINTOCHATHISTORY,
                                 reinterpret_cast<std::intptr_t>(FormatOwnedString(LookupMessage(MSI_CANNOT_OFFER_GUILD_EXP_MAX_RATE), g_guildExpLimit, 0).c_str()),
                                 0xFF, 0, 0, 0);
          position.tax = std::clamp(position.tax, 0, g_guildExpLimit);
        }
        this->m_positionMap[position.positionId] = position;
      }
      if (g_guildInfo.isMaster && this->m_positionMap.size()) {
        this->m_button->Move(this->geometry.width - 61, this->geometry.height - 23);
      } else {
        this->m_button->Move(-200, -200);
      }
      this->InvalidateChildren();
      return 0;
    }
  };

  // ---- AgencyLevelRange ----
  void (UIRegisterPartyWnd::*g_registerOnCreate)(int, int);
  void (UIRegisterPartyWnd::*g_normalizeLevels)();
  void (UIAdvenPartyBoardWnd::*g_boardOnCreate)(int, int);
  int (UIAdvenPartyBoardWnd::*g_boardSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  bool Digits(const char* text) {
    if (text == nullptr || *text == '\0') {
      return false;
    }
    for (; *text != '\0'; ++text) {
      if (!std::isdigit(static_cast<unsigned char>(*text))) {
        return false;
      }
    }
    return true;
  }

  void SetLevel(UIEditCtrl* edit, int level) {
    char number[10] = {};
    StringFormat(number, sizeof(number), _TRUNCATE, "%d", level);
    edit->SetText(number);
  }

  int MinimumLevel() {
    return std::max(1, g_session.m_level - g_agencyBelow);
  }

  int MaximumLevel() {
    return g_session.m_level + g_agencyAbove;
  }

  struct RegisterPartyHook : UIRegisterPartyWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_registerOnCreate)(width, height);
      SetLevel(this->minimum, MinimumLevel());
      SetLevel(this->maximum, MaximumLevel());
    }

    void NormalizeLevelsDetour() {
      if (!Digits(this->minimum->Text())) {
        SetLevel(this->minimum, MinimumLevel());
      }
      if (!Digits(this->maximum->Text())) {
        SetLevel(this->maximum, MaximumLevel());
      }
    }
  };

  struct PartyBoardHook : UIAdvenPartyBoardWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_boardOnCreate)(width, height);
      SetLevel(this->minimum, MinimumLevel());
      SetLevel(this->maximum, MaximumLevel());
    }

    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message == UM_COMMAND && a == ID_FIND) {
        if (!Digits(this->minimum->Text())) {
          SetLevel(this->minimum, MinimumLevel());
        }
        if (!Digits(this->maximum->Text())) {
          SetLevel(this->maximum, MaximumLevel());
        }
      }
      const int result = (this->*g_boardSendMsg)(sender, message, a, b, c, d);
      if (message == UM_COMMAND && a == ID_RESET) {
        SetLevel(this->minimum, MinimumLevel());
        SetLevel(this->maximum, MaximumLevel());
      }
      return result;
    }
  };

  // ---- EnglishArrows ----
  void (UIHotkeyGuideWnd::*g_hotkeyOnCreate)(int, int);

  struct HotkeyGuideHook : UIHotkeyGuideWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_hotkeyOnCreate)(width, height);
      static constexpr std::pair<int, const char*> kLabels[] = {{8, "<"}, {37, "<"}, {38, "^"}, {39, ">"}, {40, "v"}, {93, "="}};
      for (const auto& [key, text] : kLabels) {
        if (const auto found = this->m_labels.find(key); found != this->m_labels.end()) {
          found->second.text = text;
        }
      }
    }
  };

  // ---- MvpDropIdentified / HideZeroLastLogin ----
  decltype(&LookupMessage) g_lookupMessage;
  std::string (ITEM_INFO::*g_displayName)(unsigned char) const;

  const char* LookupMessageDetour(int id) {
    if (g_zeroLastLogin && id == MSI_TIME_TYPE4) {
      return "";
    }
    if (g_mvp && id == MSI_YOU_RECEIVE_MVP_ITEM2) {
      g_mvpPending = true;
    }
    return g_lookupMessage(id);
  }

  struct ItemInfoHook : ITEM_INFO {
    std::string DisplayNameDetour(unsigned char detailed) const {
      if (!g_mvpPending) {
        return (this->*g_displayName)(detailed);
      }
      g_mvpPending = false;
      ITEM_INFO identified(*this);
      identified.identified = 1;
      return (identified.*g_displayName)(detailed);
    }
  };

  // ---- HideZeroLastLogin ----
  bool (UIGuildMemberManageWnd::*g_drawMember)(GuildInfo::Member&, int&, int);

  struct GuildMemberHook : UIGuildMemberManageWnd {
    // DrawMember is protected: its address is taken from inside a derived class.
    static bool (UIGuildMemberManageWnd::*Target())(GuildInfo::Member&, int&, int) { return &GuildMemberHook::DrawMember; }

    bool DrawMemberDetour(GuildInfo::Member& member, int& index, int row) {
      if (member.lastOfflineTime != 0) {
        return (this->*g_drawMember)(member, index, row);
      }
      member.lastOfflineTime = 86400;
      g_zeroLastLogin = true;
      const bool result = (this->*g_drawMember)(member, index, row);
      g_zeroLastLogin = false;
      member.lastOfflineTime = 0;
      return result;
    }
  };

  // ---- CloseCutinOnEsc ----
  bool (UIWindowMgr::*g_processHotkeyAction)(int, int);

  struct WindowMgrHook : UIWindowMgr {
    // The Esc branch's checks that come before the point where WARP closes the cutin; true when one of them handles Esc.
    bool EscHandledEarlier() {
      const bool historyChat = this->windowHistory.begin() != this->windowHistory.end() && *this->windowHistory.begin() == 1;
      if ((this->IsHoverOnWindow01C8Button() || historyChat) && this->inputSuggestionsEnabled != 0 && this->chatModeChanged != 0) {
        return true;
      }
      if (this->chatWnd && this->hoverWindow) {
        UIWindow* parent = this->hoverWindow->parent;
        if (parent && this->hoverWindow->geometry.x != -100) {
          if (parent == reinterpret_cast<UIWindow*>(this->chatWnd->m_tabControl)) {
            return true;
          }
          for (const auto& retained : this->retainedWindows) {
            if (retained.second == parent) {
              return true;
            }
          }
        }
      }
      if (this->HasModalWindow()) {
        return true;
      }
      const int id = this->windowHistory.begin() != this->windowHistory.end() ? *this->windowHistory.begin() : -1;
      return g_replayAnimationScaling != 0 && (static_cast<unsigned int>(id) > 10040 || this->policy[id].actions[3] == 0);
    }

    bool ProcessHotkeyActionDetour(int action, int pressed) {
      if (action != VK_ESCAPE || !pressed || this->EscHandledEarlier() || !this->CloseWindow(WID_ILLUSTWND)) {
        return (this->*g_processHotkeyAction)(action, pressed);
      }
      if (this->autoMessageWnd != 0) {
        g_session.m_autoReplyEnabled = false;
      }
      g_modeMgr.GetGameMode()->SendMsg(MM_SHOW_IMAGE, reinterpret_cast<int>(""), -1, 1, 0);
      return false;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_rodexTax = static_cast<int>(host->SettingInt(kName, "RodexTax"));
  g_guildExpLimit = static_cast<int>(host->SettingInt(kName, "GuildExpLimit"));
  g_agencyBelow = static_cast<int>(host->SettingInt(kName, "AgencyLevelsBelow"));
  g_agencyAbove = static_cast<int>(host->SettingInt(kName, "AgencyLevelsAbove"));
  const bool arrows = host->SettingBool(kName, "EnglishArrows");
  g_mvp = host->SettingBool(kName, "MvpDropIdentified");
  const bool zeroDate = host->SettingBool(kName, "HideZeroLastLogin");
  const bool cutin = host->SettingBool(kName, "CloseCutinOnEsc");
  const bool agency = g_agencyBelow != kClientAgencyRange || g_agencyAbove != kClientAgencyRange;

  return (g_rodexTax == kClientRodexTax ||
          ro::HookVirtual<void (UIRodexComposeWnd::*)()>(host, "?Paint@UIRodexComposeWnd@@UAEXXZ", &RodexHook::PaintDetour, g_rodexPaint, kName)) &&
         (g_guildExpLimit == kClientGuildExpLimit ||
          ro::HookVirtual(host, "?SendMsg@UIGuildPositionManageWnd@@UAEHPAVUIWindow@@HHHHH@Z", &GuildPositionHook::SendMsgDetour, g_guildSendMsg, kName)) &&
         (!agency ||
          (ro::HookVirtual(host, "?OnCreate@UIRegisterPartyWnd@@UAEXHH@Z", &RegisterPartyHook::OnCreateDetour, g_registerOnCreate, kName) &&
           ro::HookVirtual(host, "?NormalizeLevels@UIRegisterPartyWnd@@AAEXXZ", &RegisterPartyHook::NormalizeLevelsDetour, g_normalizeLevels, kName) &&
           ro::HookVirtual(host, "?OnCreate@UIAdvenPartyBoardWnd@@UAEXHH@Z", &PartyBoardHook::OnCreateDetour, g_boardOnCreate, kName) &&
           ro::HookVirtual(host, "?SendMsg@UIAdvenPartyBoardWnd@@UAEHPAVUIWindow@@HHHHH@Z", &PartyBoardHook::SendMsgDetour, g_boardSendMsg, kName))) &&
         (!arrows ||
          ro::HookVirtual(host, "?OnCreate@UIHotkeyGuideWnd@@UAEXHH@Z", &HotkeyGuideHook::OnCreateDetour, g_hotkeyOnCreate, kName)) &&
         ((!g_mvp && !zeroDate) || ro::Hook(host, &LookupMessage, &LookupMessageDetour, g_lookupMessage, kName)) &&
         (!g_mvp || ro::Hook(host, &ITEM_INFO::DisplayName, &ItemInfoHook::DisplayNameDetour, g_displayName, kName)) &&
         (!zeroDate || ro::Hook(host, GuildMemberHook::Target(), &GuildMemberHook::DrawMemberDetour, g_drawMember, kName)) &&
         (!cutin || ro::Hook(host, &UIWindowMgr::ProcessHotkeyAction, &WindowMgrHook::ProcessHotkeyActionDetour, g_processHotkeyAction, kName));
}
