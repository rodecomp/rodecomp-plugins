// Hooks per setting:
// - ChatBox / ChatRoomBox / WhisperBox: the virtual OnCreate of UINewChatWnd, UIChatRoomWnd and UIWhisperWnd runs, then
//   sets the created input edit's maximum (the game sets 70). The 255 cap fits the 256-byte buffers the chat room and
//   whisper windows copy the line into.
// - NpcDialog: CGameMode::ProcessIncomingPackets copies ZC_SAY_DIALOG / ZC_SAY_DIALOG2 text into a 0x803-byte stack
//   buffer, inlined in its packet switch. CConnection::ReadPacket(output, bytes, lengths), which only that loop calls,
//   runs; for a say-dialog packet whose text does not fit the game's buffer (and fits NpcDialog) the detour does that
//   case itself with a NpcDialog-sized buffer and rewrites the packet id to ZC_PING_LIVE, which the switch ignores.
//   Text longer than NpcDialog is cut to it.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMgr.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "Network/Connection/Connection.h"
#include "Network/Packets/PacketId.h"
#include "Network/Packets/PacketLengths.h"
#include "UI/Chat/UIChatRoomWnd.h"
#include "UI/Chat/UINewChatWnd.h"
#include "UI/Chat/UIWhisperWnd.h"
#include "UI/Edit/UIEditCtrl.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"
#include "UI/Window/WindowId.h"
#include "Utility/Text/LanguageCharset.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
  constexpr const char* kName = "ChatBoxLimits";
  constexpr int kGameBox = 70;
  constexpr unsigned kGameDialog = 0x803;  // the game's buffer, terminator included
  constexpr unsigned kMaxDialog = 4096;

  int g_chatBox = kGameBox;
  int g_chatRoomBox = kGameBox;
  int g_whisperBox = kGameBox;
  unsigned g_dialog = kGameDialog;  // buffer size, terminator included

  void (UINewChatWnd::*g_newChatOnCreate)(int, int);
  void (UIChatRoomWnd::*g_chatRoomOnCreate)(int, int);
  void (UIWhisperWnd::*g_whisperOnCreate)(int, int);
  bool (CConnection::*g_readPacket)(void*, unsigned short*, const PacketLengths&);

  struct NewChatHook : UINewChatWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_newChatOnCreate)(width, height);
      if (this->m_chatEdit) {
        this->m_chatEdit->maximum = g_chatBox;
      }
    }
  };

  struct ChatRoomHook : UIChatRoomWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_chatRoomOnCreate)(width, height);
      if (this->edit) {
        this->edit->maximum = g_chatRoomBox;
      }
    }
  };

  struct WhisperHook : UIWhisperWnd {
    void OnCreateDetour(int width, int height) {
      (this->*g_whisperOnCreate)(width, height);
      if (this->m_edit) {
        this->m_edit->maximum = g_whisperBox;
      }
    }
  };

#pragma pack(push, 1)
  struct SayDialogHeader {
    /* 0000 */ std::int16_t packetType;
    /* 0002 */ std::uint16_t length;
    /* 0004 */ int npcId;
  };
#pragma pack(pop)
  static_assert(sizeof(SayDialogHeader) == 8);

  // Shows an oversized say-dialog packet the way the game's switch case does; false leaves it to the game.
  bool ShowLongDialog(unsigned char* packet) {
    SayDialogHeader header;
    std::memcpy(&header, packet, sizeof(header));
    unsigned headerSize;
    int window;
    if (header.packetType == ID_ZC_SAY_DIALOG) {
      headerSize = 8;
      window = WID_SAYDIALOGWND;
    } else if (header.packetType == ID_ZC_SAY_DIALOG2) {
      headerSize = 9;
      window = WID_SAYDIALOGWND_OVERLAY;
    } else {
      return false;
    }
    if (header.length < headerSize) {
      return false;
    }
    const unsigned prefix = g_serviceType == ServiceGermany ? 3 : 0;
    unsigned length = header.length - headerSize;
    if (prefix + length < kGameDialog) {
      return false;
    }
    CGameMode* mode = g_modeMgr.GetGameMode();
    if (!mode) {
      return false;
    }
    if (prefix + length >= g_dialog) {
      length = g_dialog - 1 - prefix;
    }
    char text[kMaxDialog];
    if (prefix) {
      std::snprintf(text, sizeof(text), "|%02x", LanguageCharset(31));
    }
    std::memcpy(text + prefix, packet + headerSize, length);
    text[prefix + length] = 0;
    mode->isOnQuest = 1;
    mode->lastNaid = header.npcId;
    g_windowMgr.EnsureWindow(window)->SendMsg(nullptr, UM_SETSAYDIALOG, reinterpret_cast<std::intptr_t>(text), header.npcId, 0, 0);
    const std::int16_t ignored = ID_ZC_PING_LIVE;
    std::memcpy(packet, &ignored, sizeof(ignored));
    return true;
  }

  struct ConnectionHook : CConnection {
    bool ReadPacketDetour(void* output, unsigned short* bytes, const PacketLengths& lengths) {
      const bool received = (this->*g_readPacket)(output, bytes, lengths);
      if (received) {
        ShowLongDialog(static_cast<unsigned char*>(output));
      }
      return received;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_chatBox = static_cast<int>(host->SettingInt(kName, "ChatBox"));
  g_chatRoomBox = static_cast<int>(host->SettingInt(kName, "ChatRoomBox"));
  g_whisperBox = static_cast<int>(host->SettingInt(kName, "WhisperBox"));
  // NpcDialog is WARP's stack size: 2052 is the game's 0x803-byte buffer (terminator included) plus one.
  g_dialog = static_cast<unsigned>(host->SettingInt(kName, "NpcDialog")) - 1;
  if (g_dialog < kGameDialog) {
    g_dialog = kGameDialog;
  }
  const bool chat = g_chatBox != kGameBox;
  const bool chatRoom = g_chatRoomBox != kGameBox;
  const bool whisper = g_whisperBox != kGameBox;
  const bool dialog = g_dialog != kGameDialog;
  if (!chat && !chatRoom && !whisper && !dialog) {
    host->Log("%s: every limit is the game's, nothing to do", kName);
    return true;
  }
  return (!chat || ro::HookVirtual(host, "?OnCreate@UINewChatWnd@@UAEXHH@Z", &NewChatHook::OnCreateDetour,
                                   g_newChatOnCreate, kName)) &&
         (!chatRoom || ro::HookVirtual(host, "?OnCreate@UIChatRoomWnd@@UAEXHH@Z", &ChatRoomHook::OnCreateDetour,
                                       g_chatRoomOnCreate, kName)) &&
         (!whisper || ro::HookVirtual(host, "?OnCreate@UIWhisperWnd@@UAEXHH@Z", &WhisperHook::OnCreateDetour,
                                      g_whisperOnCreate, kName)) &&
         (!dialog || ro::Hook(host, static_cast<bool (CConnection::*)(void*, unsigned short*, const PacketLengths&)>(
                                        &CConnection::ReadPacket),
                              &ConnectionHook::ReadPacketDetour, g_readPacket, kName));
}
