// Hooks the virtual CGameMode::SendMsg: MM_REQ_MAKE_GUILD runs a copy of the rodecomp case without the
// strchr(name, ' ') rejection; every other event goes to the original.
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
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "Utility/Text/FormatString.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
  constexpr const char* kName = "WARP_AllowSpaceInGuildName";

#pragma pack(push, 1)
  struct GameModePacketDwordPayload24 {
    /* 0000 */ std::uint16_t packetId;
    /* 0002 */ std::uint32_t argument;
    /* 0006 */ std::uint8_t payload[24];
  };
#pragma pack(pop)
  static_assert(sizeof(GameModePacketDwordPayload24) == 30);

  int (CGameMode::*g_original)(int, int, int, int, int);

  struct GameModeHook : CGameMode {
    int SendMsgHook(int event, int argument1, int argument2, int argument3, int argument4) {
      if (event != MM_REQ_MAKE_GUILD) {
        return (this->*g_original)(event, argument1, argument2, argument3, argument4);
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
  return ro::HookVirtual(host, "?SendMsg@CGameMode@@UAEHHHHHH@Z", &GameModeHook::SendMsgHook, g_original, kName);
}
