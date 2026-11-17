// Hooks per setting:
// - Username / Password: UILoginWnd::SendMsg marks the window while UM_COMMAND/ID_LOGIN runs; for that window the
//   UIEditCtrl::Length detour raises the ID edit's length to at least 4 (Username) and the password edit's to at least
//   6 (Password), so the login goes through.
// - CharacterName: UINewMakeCharWnd::SendMsg and UIChangeNameWnd::SendMsg mark the window's name edit while a
//   UM_COMMAND/ID_OK runs; the UIEditCtrl::Length detour reports 1-3 characters as 4 for that edit (both "< 4" checks).
//   The ">= 24" checks see lengths of 4 and more unchanged.
// - GuildNameSpaces: the virtual CGameMode::SendMsg runs, for MM_REQ_MAKE_GUILD, a copy of the rodecomp case without
//   the strchr(name, ' ') rejection; every other event goes to the original.
// Shared hook: UIEditCtrl::Length (noinline) is hooked once when any of Username, Password or CharacterName is on; it
// applies the login rule while the login window's ID_LOGIN runs and the name rule while the char-creation/rename ID_OK
// runs, and returns the original length otherwise.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Client/Session/Session.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Actors/Pc.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "Contents/Tables/ForbiddenWords.h"
#include "Contents/Tables/MessageTable.h"
#include "Contents/World/World.h"
#include "Network/Connection/Connection.h"
#include "Network/Packets/PacketId.h"
#include "Resource/MSGSTRINGID.h"
#include "UI/Edit/UIEditCtrl.h"
#include "UI/Login/UIChangeNameWnd.h"
#include "UI/Login/UILoginWnd.h"
#include "UI/Login/UINewMakeCharWnd.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "Utility/Text/FormatString.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
  constexpr const char* kName = "WARP_InputLimits";

  bool g_username;
  bool g_password;
  bool g_characterName;
  bool g_guildNameSpaces;

#pragma pack(push, 1)
  struct GameModePacketDwordPayload24 {
    /* 0000 */ std::uint16_t packetId;
    /* 0002 */ std::uint32_t argument;
    /* 0006 */ std::uint8_t payload[24];
  };
#pragma pack(pop)
  static_assert(sizeof(GameModePacketDwordPayload24) == 30);

  int (UIEditCtrl::*g_length)() const;
  int (UILoginWnd::*g_loginSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (UINewMakeCharWnd::*g_makeCharSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (UIChangeNameWnd::*g_changeNameSendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  int (CGameMode::*g_gameModeSendMsg)(int, int, int, int, int);

  const UILoginWnd* g_login;
  const UIEditCtrl* g_nameEdit;

  struct EditHook : UIEditCtrl {
    int LengthDetour() const {
      const int length = (this->*g_length)();
      const UIEditCtrl* edit = this;
      if (g_login != nullptr) {
        if (g_username && edit == g_login->idEdit && length < 4) {
          return 4;
        }
        if (g_password && edit == g_login->passwordEdit && length < 6) {
          return 6;
        }
      }
      if (edit == g_nameEdit && length >= 1 && length < 4) {
        return 4;
      }
      return length;
    }
  };

  struct LoginHook : UILoginWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      if (message != UM_COMMAND || a != ID_LOGIN) {
        return (this->*g_loginSendMsg)(sender, message, a, b, c, d);
      }
      const UILoginWnd* previous = g_login;
      g_login = this;
      const int result = (this->*g_loginSendMsg)(sender, message, a, b, c, d);
      g_login = previous;
      return result;
    }
  };

  template <class Window, class Original>
  int CheckedNameSendMsg(Window* window, Original original, const UIEditCtrl* edit, UIWindow* sender, int message,
                         std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
    if (message != UM_COMMAND || a != ID_OK) {
      return (window->*original)(sender, message, a, b, c, d);
    }
    const UIEditCtrl* previous = g_nameEdit;
    g_nameEdit = edit;
    const int result = (window->*original)(sender, message, a, b, c, d);
    g_nameEdit = previous;
    return result;
  }

  struct MakeCharHook : UINewMakeCharWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      return CheckedNameSendMsg(static_cast<UINewMakeCharWnd*>(this), g_makeCharSendMsg, this->m_nameEdit, sender, message,
                                a, b, c, d);
    }
  };

  struct ChangeNameHook : UIChangeNameWnd {
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      return CheckedNameSendMsg(static_cast<UIChangeNameWnd*>(this), g_changeNameSendMsg, this->m_edit, sender, message,
                                a, b, c, d);
    }
  };

  struct GameModeHook : CGameMode {
    int SendMsgDetour(int event, int argument1, int argument2, int argument3, int argument4) {
      if (event != MM_REQ_MAKE_GUILD) {
        return (this->*g_gameModeSendMsg)(event, argument1, argument2, argument3, argument4);
      }
      const char* name = reinterpret_cast<const char*>(argument1);
      const std::size_t length = std::strlen(name);
      char text[256];
      if ((static_cast<unsigned char>(name[0]) == 0xA1 && static_cast<unsigned char>(name[1]) == 0xA1) ||
          (static_cast<unsigned char>(name[length - 2]) == 0xA1 && static_cast<unsigned char>(name[length - 1]) == 0xA1)) {
        std::sprintf(text, LookupMessage(MSI_BILLING_CHN_3));
        if (std::strstr(text, "NO MSG")) {
          std::sprintf(text, "Can NOT use space in Guild Name !!");
        }
        g_windowMgr.SendNotice(UIM_PUSHINTOCHATHISTORY, reinterpret_cast<std::intptr_t>(text), 0x6E96FF, 0, 0, 0);
        return 0;
      }
      if (g_forbiddenWords.Contains(name)) {
        StringFormat(text, sizeof(text), static_cast<std::size_t>(-1), LookupMessage(MSI_SELECTCHARWND_BILLING_INFO));
        g_windowMgr.SendNotice(UIM_PUSHINTOCHATHISTORY, reinterpret_cast<std::intptr_t>(text), 0x6E96FF, 0, 0, 0);
        return 0;
      }
      if (g_serviceType == ServiceBrazil && std::strchr(name, 0x2323)) {
        g_windowMgr.Confirm(LookupMessage(MSI_BILLING_1), 0, true, false, 0, "Message", 280, 120, 0);
        return 0;
      }
      if (length >= 24) {
        g_windowMgr.Confirm(LookupMessage(MSI_NAMELENGTH_TOO_LONG), 0, true, false, 0, "Message", 280, 120, 0);
        return 0;
      }
      if (g_modeMgr.GetGameMode()->scene->player->hasSp == 1) {
        std::sprintf(text, LookupMessage(MSI_TIME_LINE_MINUTE2));
        g_windowMgr.SendNotice(UIM_PUSHINTOCHATHISTORY, reinterpret_cast<std::intptr_t>(text), 0x6E96FF, 0, 0, 0);
        return 0;
      }
      GameModePacketDwordPayload24 packet;
      packet.packetId = ID_CZ_REQ_MAKE_GUILD;
      packet.argument = g_session.m_gid;
      std::strncpy(reinterpret_cast<char*>(packet.payload), name, 23);
      packet.payload[23] = 0;
      GetRagConnection().Send(GetRagConnection().GetPacketSize(packet.packetId), &packet);
      return 0;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_username = host->SettingBool(kName, "Username");
  g_password = host->SettingBool(kName, "Password");
  g_characterName = host->SettingBool(kName, "CharacterName");
  g_guildNameSpaces = host->SettingBool(kName, "GuildNameSpaces");
  const bool login = g_username || g_password;
  if (!login && !g_characterName && !g_guildNameSpaces) {
    host->Log("%s: Username, Password, CharacterName and GuildNameSpaces are all off, nothing to do", kName);
    return true;
  }
  return ((!login && !g_characterName) ||
          ro::Hook(host, &UIEditCtrl::Length, &EditHook::LengthDetour, g_length, kName)) &&
         (!login || ro::HookVirtual(host, "?SendMsg@UILoginWnd@@UAEHPAVUIWindow@@HHHHH@Z", &LoginHook::SendMsgDetour,
                                    g_loginSendMsg, kName)) &&
         (!g_characterName ||
          (ro::HookVirtual(host, "?SendMsg@UINewMakeCharWnd@@UAEHPAVUIWindow@@HHHHH@Z", &MakeCharHook::SendMsgDetour,
                           g_makeCharSendMsg, kName) &&
           ro::HookVirtual(host, "?SendMsg@UIChangeNameWnd@@UAEHPAVUIWindow@@HHHHH@Z", &ChangeNameHook::SendMsgDetour,
                           g_changeNameSendMsg, kName))) &&
         (!g_guildNameSpaces || ro::HookVirtual(host, "?SendMsg@CGameMode@@UAEHHHHHH@Z", &GameModeHook::SendMsgDetour,
                                                g_gameModeSendMsg, kName));
}
