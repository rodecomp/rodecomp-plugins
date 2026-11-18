// Hooks CGameMode::ReceivePetAct, the ZC_PET_ACT handler and the only pet caller of CSession::FormatMonsterSpeech,
// to scope a FormatMonsterSpeech hook. After the original composed "<name> : <line>" into `output`, the marker is
// inserted after (or before) the name. The output buffer is 256 bytes; a line that would overflow is left alone.
// Like WARP, the marker also shows in the pet's speech bubble (the same buffer feeds both).
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Client/Session/Session.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
  constexpr const char* kName = "PetTalkMarker";
  constexpr std::size_t kBuffer = 256;

  std::string g_marker;
  bool g_before;
  bool g_inPetAct;

  void (CGameMode::*g_receivePetAct)(unsigned char*);
  void (CSession::*g_format)(char*, char*, std::uint32_t, int);

  struct SessionHook : CSession {
    void FormatDetour(char* text, char* output, std::uint32_t aid, int feminine) {
      (this->*g_format)(text, output, aid, feminine);
      if (!g_inPetAct) {
        return;
      }
      char* separator = std::strstr(output, " : ");
      if (separator == nullptr || std::strlen(output) + g_marker.size() + 1 >= kBuffer) {
        return;
      }
      std::string line(output);
      const std::size_t at = static_cast<std::size_t>(separator - output);
      if (g_before) {
        line.insert(0, g_marker + " ");
      } else {
        line.insert(at, " " + g_marker);
      }
      std::memcpy(output, line.c_str(), line.size() + 1);
    }
  };

  struct GameModeHook : CGameMode {
    void ReceivePetActDetour(unsigned char* packet) {
      g_inPetAct = true;
      (this->*g_receivePetAct)(packet);
      g_inPetAct = false;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  std::string text = host->SettingString(kName, "Text");
  if (text.empty() || text.size() > 12 || text.find('%') != std::string::npos) {
    host->Log("%s: Text must be 1-12 characters without %%, using \"pet\"", kName);
    text = "pet";
  }
  const std::string brackets = host->SettingString(kName, "Brackets");
  const char* open = brackets == "Square" ? "[" : brackets == "Angled" ? "<" : brackets == "None" ? "" : "(";
  const char* close = brackets == "Square" ? "]" : brackets == "Angled" ? ">" : brackets == "None" ? "" : ")";
  g_marker = std::string(open) + text + close;
  g_before = std::strcmp(host->SettingString(kName, "Side"), "Before") == 0;
  return ro::Hook(host, &CSession::FormatMonsterSpeech, &SessionHook::FormatDetour, g_format, kName) &&
         ro::Hook(host, &CGameMode::ReceivePetAct, &GameModeHook::ReceivePetActDetour, g_receivePetAct, kName);
}
