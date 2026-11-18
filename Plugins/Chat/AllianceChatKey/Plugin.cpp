// CGameMode::ExecuteTalkType's TT_NORMAL / TT_NORMAL_FROM_CHATROOM case strips a leading '#' and sends the rest to
// Alliance chat (MM_CHAT_ALLY); WARP AllianceChatHotkeySelector changes that compared character. The case is part of
// a ~1000-line switch, so the detour runs a copy of that case with Symbol in place of '#' and passes every other talk
// type to the original. Installed only when Symbol is not '#'.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Session/Session.h"
#include "Contents/Clan/ClanInfo.h"
#include "Contents/Guild/GuildInfo.h"
#include "UI/GameSettings/TalkType.h"
#include "UI/Window/UIWindowMgr.h"

#include <cstring>
#include <string>
#include <windows.h>

namespace {
  constexpr const char* kName = "AllianceChatKey";

  char g_symbol;

  void (CGameMode::*g_original)(int, std::string*);

  struct GameModeHook : CGameMode {
    void ExecuteTalkType(int talkType, std::string* arguments) {
      if (talkType != TT_NORMAL && talkType != TT_NORMAL_FROM_CHATROOM) {
        (this->*g_original)(talkType, arguments);
        return;
      }
      char text[364];
      int party = 0;
      int alternate = 0;
      std::strcpy(text, g_windowMgr.chatInputText.c_str());
      if (text[0] == '%') {
        if (std::strlen(text) <= 1) {
          return;
        }
        g_windowMgr.SetChatInputText(text + 1);
        party = 1;
      } else if (text[0] == '$') {
        if (std::strlen(text) <= 1) {
          return;
        }
        g_windowMgr.SetChatInputText(text + 1);
        alternate = 1;
      }
      if (HIBYTE(GetAsyncKeyState(VK_CONTROL))) {
        party = 1;
      }
      if (HIBYTE(GetAsyncKeyState(VK_MENU))) {
        alternate = 1;
      }
      bool clan = false;
      if (text[0] == g_symbol) {
        if (std::strlen(text) <= 1) {
          return;
        }
        g_windowMgr.SetChatInputText(text + 1);
        clan = true;
      }
      int clanChat = clan;
      if (HIBYTE(GetAsyncKeyState(VK_CAPITAL))) {
        clanChat = 1;
      }
      if (talkType == TT_NORMAL_FROM_CHATROOM) {
        this->SendMsg(MM_CHATMSG, 0, 0, 0, 0);
        return;
      }
      if (g_session.battleChat != 0) {
        this->SendMsg(MM_CHAT_BATTLEFIELD, 0, 0, 0, 0);
        return;
      }
      const int sendMode = g_session.m_chatTarget;
      if (sendMode == 2 && g_guildInfo.m_guildId != 0) {
        this->SendMsg(alternate ? MM_CHATMSG : MM_CHAT_GUILD, 0, 0, 0, 0);
        return;
      }
      if (sendMode == 1) {
        if (party == 0 && g_session.PartyCount() != 0) {
          this->SendMsg(MM_CHAT_PARTY, 0, 0, 0, 0);
        } else {
          this->SendMsg(MM_CHATMSG, 0, 0, 0, 0);
        }
        return;
      }
      if (sendMode == 3 && g_clanInfo->m_hasClan == 1) {
        this->SendMsg(MM_CHAT_CLAN, 0, 0, 0, 0);
        return;
      }
      if (sendMode == 4 && g_guildInfo.m_guildId != 0) {
        this->SendMsg(static_cast<char>(clanChat) ? MM_CHATMSG : MM_CHAT_ALLY, 0, 0, 0, 0);
        return;
      }
      if (party != 0 && g_session.PartyCount() != 0) {
        this->SendMsg(MM_CHAT_PARTY, 0, 0, 0, 0);
      } else if (alternate != 0) {
        this->SendMsg(MM_CHAT_GUILD, 0, 0, 0, 0);
      } else if (static_cast<char>(clanChat)) {
        this->SendMsg(MM_CHAT_ALLY, 0, 0, 0, 0);
      } else {
        this->SendMsg(MM_CHATMSG, 0, 0, 0, 0);
      }
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_symbol = host->SettingString(kName, "Symbol")[0];
  if (g_symbol == '#') {
    host->Log("%s: prefix is the game's '#', nothing to do", kName);
    return true;
  }
  host->Log("%s: alliance chat prefix '%c'", kName, g_symbol);
  return ro::Hook(host, &CGameMode::ExecuteTalkType, &GameModeHook::ExecuteTalkType, g_original, kName);
}
