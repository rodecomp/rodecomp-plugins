// Korean map-name banner on non-Korean service types.
// CMapInfoMgr::ShowMapName (map entry, when the "show map name" option is on) builds a MapNameDisplay from
// AddMapSignName's two strings of system\mapInfo_true.lub (region line and map title) or, without them, MSI_NOWHERE.
// MapNameDisplay's constructor draws them through a TextContext with SetFont(family 0 / 29 on Korea): family 0 maps to
// the service type's font, whose charset (ANSI on America) also picks the codepage MultiByteToWideChar decodes the
// bytes with (TextContext::CodePage). The lub is Korean (CP949) in every official GRF, so on langtype != 0 the bytes
// come out as Latin-1 mojibake. The text never passes through UIWindow::DrawTextAt, so TextDrawHooks cannot reach it.
// Fix: ShowMapName is hooked; when the strings it will draw are CP949 Hangul (and nothing else non-ASCII),
// TextContext::SetFont gives the contexts created meanwhile HANGUL_CHARSET. That makes the text decode with codepage
// 949 and FontCache create the font with the Hangul charset (CreateSystemFont finds no Hangul "Arial" and lets Windows
// pick a Korean face). Translated, Latin text (accents included) fails the check and keeps the game's charset; on Korea
// the charset is already Hangul.
#include "RoPlugin.h"

#include "Contents/Map/MapInfoMgr.h"
#include "Render/Text/TextContext.h"

#include <cstddef>
#include <string>
#include <windows.h>

namespace {
  constexpr const char* kName = "KoreanText";

  bool g_korean;  // set while ShowMapName draws Korean text

  void (CMapInfoMgr::*g_showMapName)(std::string);
  void (TextContext::*g_setFont)(int, int, unsigned char, unsigned char);

  enum class Kind { Ascii, Korean, Other };

  // Korean: non-ASCII bytes present, every one in a KS X 1001 pair from the symbol rows (A1-AC, e.g. the middle dot) or
  // the Hangul syllable rows (B0-C8), trail A1-FE, which is how the official lubs spell Korean. Latin-1 text with
  // accented letters (a lone byte >= 0x80 next to ASCII, or a pair outside those rows) is Other. UHC's extended pairs
  // (trail 41-A0) are not accepted: "Ágora" (C1 67) would decode as Hangul.
  Kind Classify(const std::string& text) {
    Kind kind = Kind::Ascii;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const auto lead = static_cast<unsigned char>(text[i]);
      if (lead < 0x80) {
        continue;
      }
      if (i + 1 >= text.size()) {
        return Kind::Other;
      }
      const auto trail = static_cast<unsigned char>(text[i + 1]);
      if (!(((lead >= 0xA1 && lead <= 0xAC) || (lead >= 0xB0 && lead <= 0xC8)) && trail >= 0xA1 && trail <= 0xFE)) {
        return Kind::Other;
      }
      kind = Kind::Korean;
      ++i;
    }
    return kind;
  }

  struct MapInfoHook : CMapInfoMgr {
    void ShowMapName(std::string map) {
      // Mirrors the original's choice of strings: the lub's two sign lines, or MSI_NOWHERE when both are empty.
      bool korean = false;
      if (const MapInfo* info = this->FindMapInfo(map); info != nullptr && info->m_showDisplayName) {
        Kind first = Kind::Ascii;
        Kind second;
        if (info->m_sign.first.empty() && info->m_sign.second.empty()) {
          second = Classify(this->m_emptyName);
        } else {
          first = Classify(info->m_sign.first);
          second = Classify(info->m_sign.second);
        }
        // Both lines are drawn with one decision; a Latin line next to a Korean one keeps the game's charset.
        korean = first != Kind::Other && second != Kind::Other && (first == Kind::Korean || second == Kind::Korean);
      }
      g_korean = korean;
      (this->*g_showMapName)(std::move(map));
      g_korean = false;
    }
  };

  struct TextContextHook : TextContext {
    void SetFont(int family, int height, unsigned char bold, unsigned char italic) {
      (this->*g_setFont)(family, height, bold, italic);
      if (g_korean) {
        this->m_charset = HANGUL_CHARSET;
      }
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (!ro::Hook(host, &CMapInfoMgr::ShowMapName, &MapInfoHook::ShowMapName, g_showMapName, kName) ||
      !ro::Hook(host, &TextContext::SetFont, &TextContextHook::SetFont, g_setFont, kName)) {
    return false;
  }
  host->Log("%s: map-name banner drawn with the Hangul charset when its text is Korean", kName);
  return true;
}
