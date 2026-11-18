// Recolours chat lines. The game passes colours as 0x00BBGGRR to UIWindowMgr::SendNotice(UIM_PUSHINTOCHATHISTORY,
// text, colour, ...). Each packet handler below runs with a small remap table active; the SendNotice detour replaces
// the colour of chat-history lines pushed inside that handler (one lookup, no chaining), every other call passes.
// - CGameMode::ReceiveActorChat (ZC_NOTIFY_CHAT): 0xFFFFFF (public, others) -> Public; the GM/monster-job line's
//   0xFFFF (yellow), 0xFFFF00 (Thai) and 0x1D8DFF (Germany) -> GM.
// - CGameMode::ReceivePartyChat: 0xC8FF (yourself) -> PartySelf, 0xC8C8FF (others) -> Party.
// - CGameMode::ReceiveGuildChat: 0xB4FFB4 -> Guild.
// - CGameMode::ReceiveBroadcast (ZC_NOTIFY_PLAYERCHAT, your own line): 0xFF00 -> PublicSelf.
// A handler is hooked only when one of its colours differs from the game's.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "UI/Window/UIEnums.h"
#include "UI/Window/UIWindowMgr.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "ChatColors";

  struct Remap {
    std::intptr_t from;
    std::intptr_t to;
  };

  Remap g_actorChat[4];
  Remap g_partyChat[2];
  Remap g_guildChat[1];
  Remap g_broadcast[1];

  const Remap* g_active = nullptr;
  int g_activeCount = 0;

  int (UIWindowMgr::*g_sendNotice)(int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);
  void (CGameMode::*g_receiveActorChat)(const unsigned char*);
  void (CGameMode::*g_receivePartyChat)(const unsigned char*);
  void (CGameMode::*g_receiveGuildChat)(const unsigned char*);
  void (CGameMode::*g_receiveBroadcast)(const unsigned char*);

  // "RRGGBB" or "#RRGGBB" -> 0x00BBGGRR; false when malformed.
  bool ParseColor(const char* text, std::intptr_t& out) {
    if (*text == '#') {
      ++text;
    }
    std::uint32_t rgb = 0;
    int digits = 0;
    for (; *text; ++text, ++digits) {
      const char c = *text;
      int v;
      if (c >= '0' && c <= '9') {
        v = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        v = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        v = c - 'A' + 10;
      } else {
        return false;
      }
      rgb = (rgb << 4) | static_cast<std::uint32_t>(v);
    }
    if (digits != 6) {
      return false;
    }
    out = static_cast<std::intptr_t>(((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF));
    return true;
  }

  struct Scope {
    const Remap* saved;
    int savedCount;
    template <int N>
    explicit Scope(const Remap (&table)[N]) : saved(g_active), savedCount(g_activeCount) {
      g_active = table;
      g_activeCount = N;
    }
    ~Scope() {
      g_active = saved;
      g_activeCount = savedCount;
    }
  };

  struct WindowMgrHook : UIWindowMgr {
    int SendNotice(int selector, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d, std::intptr_t e) {
      if (selector == UIM_PUSHINTOCHATHISTORY) {
        for (int i = 0; i < g_activeCount; ++i) {
          if (g_active[i].from == b) {
            b = g_active[i].to;
            break;
          }
        }
      }
      return (this->*g_sendNotice)(selector, a, b, c, d, e);
    }
  };

  struct GameModeHook : CGameMode {
    void ReceiveActorChat(const unsigned char* data) {
      Scope scope(g_actorChat);
      (this->*g_receiveActorChat)(data);
    }
    void ReceivePartyChat(const unsigned char* data) {
      Scope scope(g_partyChat);
      (this->*g_receivePartyChat)(data);
    }
    void ReceiveGuildChat(const unsigned char* data) {
      Scope scope(g_guildChat);
      (this->*g_receiveGuildChat)(data);
    }
    void ReceiveBroadcast(const unsigned char* data) {
      Scope scope(g_broadcast);
      (this->*g_receiveBroadcast)(data);
    }
  };

  // Reads setting `key` into `to`; keeps `from` (the game's colour) when malformed. True when it changes the colour.
  bool Read(const RoHostApi* host, const char* key, std::intptr_t from, std::intptr_t& to) {
    to = from;
    const char* text = host->SettingString(kName, key);
    if (!ParseColor(text, to)) {
      host->Log("%s: setting %s: \"%s\" is not an RRGGBB colour, keeping the game's", kName, key, text);
      to = from;
    }
    return to != from;
  }

  template <int N>
  bool Changed(const Remap (&table)[N]) {
    for (const Remap& remap : table) {
      if (remap.from != remap.to) {
        return true;
      }
    }
    return false;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  std::intptr_t gm;
  Read(host, "GM", 0xFFFF, gm);
  g_actorChat[0] = {0xFFFF, gm};
  g_actorChat[1] = {0xFFFF00, gm};
  g_actorChat[2] = {0x1D8DFF, gm};
  g_actorChat[3].from = 0xFFFFFF;
  Read(host, "Public", 0xFFFFFF, g_actorChat[3].to);
  g_partyChat[0].from = 0xC8FF;
  Read(host, "PartySelf", 0xC8FF, g_partyChat[0].to);
  g_partyChat[1].from = 0xC8C8FF;
  Read(host, "Party", 0xC8C8FF, g_partyChat[1].to);
  g_guildChat[0].from = 0xB4FFB4;
  Read(host, "Guild", 0xB4FFB4, g_guildChat[0].to);
  g_broadcast[0].from = 0xFF00;
  Read(host, "PublicSelf", 0xFF00, g_broadcast[0].to);

  const bool actor = Changed(g_actorChat);
  const bool party = Changed(g_partyChat);
  const bool guild = Changed(g_guildChat);
  const bool broadcast = Changed(g_broadcast);
  if (!actor && !party && !guild && !broadcast) {
    host->Log("%s: every colour is the game's, nothing to do", kName);
    return true;
  }
  return ro::Hook(host, &UIWindowMgr::SendNotice, &WindowMgrHook::SendNotice, g_sendNotice, kName) &&
         (!actor || ro::Hook(host, &CGameMode::ReceiveActorChat, &GameModeHook::ReceiveActorChat, g_receiveActorChat, kName)) &&
         (!party || ro::Hook(host, &CGameMode::ReceivePartyChat, &GameModeHook::ReceivePartyChat, g_receivePartyChat, kName)) &&
         (!guild || ro::Hook(host, &CGameMode::ReceiveGuildChat, &GameModeHook::ReceiveGuildChat, g_receiveGuildChat, kName)) &&
         (!broadcast ||
          ro::Hook(host, &CGameMode::ReceiveBroadcast, &GameModeHook::ReceiveBroadcast, g_receiveBroadcast, kName));
}
