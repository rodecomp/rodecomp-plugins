// Font and codepage overrides (WARP CustomFont group).
// Fonts: the game creates every system font in FontEntry::CreateSystemFont, its only CreateFontA call (embedded and
// private fonts go through CreateFontIndirectA and stay as they are, like WARP, which patched the CreateFontA calls).
// CreateSystemFont is hooked to mark the call, and gdi32!CreateFontA rewrites height, weight, charset and face only
// while the mark is set, so no other CreateFontA caller in the process is touched.
// Codepage: InitializeLanguage sets g_textCodePage per service type (overridden after it), LanguageCodePage maps a
// language to a codepage when g_inlineCharset is set (replaced), and UIChangeNameWnd::IsNameWithinLimit forces
// _setmbcp(949) before counting double-byte characters (replaced with the same count under the chosen codepage;
// _setmbcp(65001) is not a valid multibyte codepage, so 65001 leaves the 949 call as it is, as WARP does).
// Custom fonts (font families 20-28: RixSquirrel, RixLoveangel, ...; WARP EnableCustomFonts / FixFontsCharset):
// - TrueTypeFonts: the .eot embedding (LoadEmbedFont through T2Embed, Korean service type only) is skipped, and it no
//   longer exits the client when an .eot file is missing. Instead, after CSession::InitClientData (where the .eot files
//   would load), every System\Font\*.ttf is added as a private font with AddFontResourceExA. The client creates those
//   fonts with face names such as "RixSquirrel_10.eot" (CreateFontIndirectA from FontCache::Get and
//   CreateEmbeddedFont); a gdi32!CreateFontIndirectA detour strips the ".eot" so the face matches the TrueType family.
// - CustomFontsCharset: TextContext::SetFont gives families 20 and up charsets[family], a zero (ANSI) entry, on every
//   service type but Korea; the detour sets the service type's own charset instead, as WARP's table does.
//   TrueTypeFonts needs it and turns it on. Charset (system fonts, CreateFontA) and CustomFontsCharset (custom fonts,
//   TextContext) apply to disjoint font sets and never override each other.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Network/TextEncoding.h"
#include "Render/Text/EmbedFont.h"
#include "Render/Text/FontCache.h"
#include "Render/Text/TextContext.h"
#include "UI/Login/UIChangeNameWnd.h"
#include "Utility/Text/LanguageCharset.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mbctype.h>
#include <mbstring.h>
#include <string>
#include <windows.h>

void InitializeLanguage();

// TextContext::m_charset is private: reached through ro::PrivateField (RoPlugin.h) rather than its offset.
struct TextContextCharset : ro::PrivateField<TextContextCharset, unsigned TextContext::*> {};
template struct ro::GrantPrivate<TextContextCharset, &TextContext::m_charset>;

namespace {
  constexpr const char* kName = "Fonts";

  enum class HeightMode { Unchanged, Character, Cell, Offset, Limits };
  enum class WeightMode { Unchanged, All, Offset, Separate };

  struct Charset {
    const char* name;
    int value;
  };
  constexpr Charset kCharsets[] = {
    {"ANSI", ANSI_CHARSET},       {"BALTIC", BALTIC_CHARSET},     {"CHINESEBIG5", CHINESEBIG5_CHARSET},
    {"DEFAULT", DEFAULT_CHARSET}, {"EASTEUROPE", EASTEUROPE_CHARSET}, {"GB2312", GB2312_CHARSET},
    {"GREEK", GREEK_CHARSET},     {"HANGUL", HANGUL_CHARSET},     {"MAC", MAC_CHARSET},
    {"OEM", OEM_CHARSET},         {"RUSSIAN", RUSSIAN_CHARSET},   {"SHIFTJIS", SHIFTJIS_CHARSET},
    {"SYMBOL", SYMBOL_CHARSET},   {"TURKISH", TURKISH_CHARSET},   {"VIETNAMESE", VIETNAMESE_CHARSET},
    {"JOHAB", JOHAB_CHARSET},     {"ARABIC", ARABIC_CHARSET},     {"HEBREW", HEBREW_CHARSET},
    {"THAI", THAI_CHARSET},
  };

  constexpr int kFirstCustomFamily = 20;
  constexpr char kEot[] = ".eot";

  // Custom font charset per service type (g_serviceType 0..20), WARP FixFontsCharset's table.
  constexpr BYTE kServiceCharsets[] = {
    HANGUL_CHARSET, ANSI_CHARSET,    SHIFTJIS_CHARSET,   GB2312_CHARSET, CHINESEBIG5_CHARSET, THAI_CHARSET,
    ANSI_CHARSET,   ANSI_CHARSET,    ANSI_CHARSET,       ANSI_CHARSET,   ANSI_CHARSET,        ANSI_CHARSET,
    ANSI_CHARSET,   ANSI_CHARSET,    RUSSIAN_CHARSET,    VIETNAMESE_CHARSET, ANSI_CHARSET,    ANSI_CHARSET,
    ANSI_CHARSET,   ARABIC_CHARSET,  ANSI_CHARSET};

  const RoHostApi* g_host;
  const char* g_face;  // null: unchanged
  HeightMode g_heightMode;
  int g_height, g_heightOffset, g_heightMin, g_heightMax;
  WeightMode g_weightMode;
  int g_weight, g_weightOffset, g_normalWeight, g_boldWeight;
  int g_charset = -1;  // -1: unchanged
  int g_codePage = -1; // -1: unchanged

  thread_local bool g_inSystemFont;

  void (FontEntry::*g_originalCreateSystemFont)();
  decltype(&::CreateFontA) g_originalCreateFontA;
  decltype(&InitializeLanguage) g_originalInitializeLanguage;
  decltype(&LanguageCodePage) g_originalLanguageCodePage;
  int (UIChangeNameWnd::*g_originalIsNameWithinLimit)(std::string, int);
  void (TextContext::*g_originalSetFont)(int, int, unsigned char, unsigned char);
  void (CSession::*g_originalInitClientData)();
  decltype(&LoadEmbedFont) g_originalLoadEmbedFont;
  decltype(&::CreateFontIndirectA) g_originalCreateFontIndirectA;

  int AdjustHeight(int height) {
    switch (g_heightMode) {
    case HeightMode::Character:
      return -g_height;
    case HeightMode::Cell:
      return g_height;
    case HeightMode::Offset:
      return height + g_heightOffset;
    case HeightMode::Limits:
      // Like WARP: character heights (negative) are left alone, cell heights are clamped.
      if (height >= 0) {
        if (g_heightMin != 0 && height < g_heightMin) {
          height = g_heightMin;
        }
        if (g_heightMax != 0 && height > g_heightMax) {
          height = g_heightMax;
        }
      }
      return height;
    default:
      return height;
    }
  }

  int AdjustWeight(int weight) {
    switch (g_weightMode) {
    case WeightMode::All:
      return g_weight;
    case WeightMode::Offset:
      weight += g_weightOffset;
      return weight < 0 ? 0 : weight > 1000 ? 1000 : weight;
    case WeightMode::Separate:
      return weight == FW_NORMAL ? g_normalWeight : weight == FW_BOLD ? g_boldWeight : weight;
    default:
      return weight;
    }
  }

  HFONT WINAPI CreateFontHook(int height, int width, int escapement, int orientation, int weight, DWORD italic,
                              DWORD underline, DWORD strikeOut, DWORD charset, DWORD outPrecision, DWORD clipPrecision,
                              DWORD quality, DWORD pitchAndFamily, LPCSTR face) {
    if (g_inSystemFont) {
      height = AdjustHeight(height);
      weight = AdjustWeight(weight);
      if (g_charset >= 0) {
        charset = static_cast<DWORD>(g_charset);
      }
      if (g_face) {
        face = g_face;
      }
    }
    return g_originalCreateFontA(height, width, escapement, orientation, weight, italic, underline, strikeOut, charset,
                                 outPrecision, clipPrecision, quality, pitchAndFamily, face);
  }

  struct FontEntryHook : FontEntry {
    void CreateSystemFont() {
      g_inSystemFont = true;
      (this->*g_originalCreateSystemFont)();
      g_inSystemFont = false;
    }
  };

  void InitializeLanguageHook() {
    g_originalInitializeLanguage();
    g_textCodePage = static_cast<UINT>(g_codePage);
  }

  unsigned LanguageCodePageHook(std::uint16_t) {
    return static_cast<unsigned>(g_codePage);
  }

  struct ChangeNameHook : UIChangeNameWnd {
    // UIChangeNameWnd::IsNameWithinLimit with _setmbcp(<codepage>) in place of _setmbcp(949).
    int IsNameWithinLimit(std::string name, int limit) {
      _setmbcp(g_codePage);
      int width = 0;
      for (std::size_t index = 0; index < name.size(); ++index) {
        switch (_mbsbtype(reinterpret_cast<const unsigned char*>(name.c_str()), index)) {
        case _MBC_SINGLE:
          ++width;
          break;
        case _MBC_TRAIL:
          width += 2;
          break;
        }
      }
      return width < limit ? 1 : 0;
    }
  };

  struct TextContextHook : TextContext {
    void SetFont(int family, int height, unsigned char bold, unsigned char italic) {
      (this->*g_originalSetFont)(family, height, bold, italic);
      if (family >= kFirstCustomFamily && g_serviceType >= 0 &&
          g_serviceType < static_cast<int>(sizeof(kServiceCharsets))) {
        ro::Field<TextContextCharset>(*this) = kServiceCharsets[g_serviceType];
      }
    }
  };

  struct SessionHook : CSession {
    void InitClientData() {
      (this->*g_originalInitClientData)();
      WIN32_FIND_DATAA found;
      HANDLE search = FindFirstFileA("System\\Font\\*.ttf", &found);
      if (search == INVALID_HANDLE_VALUE) {
        g_host->Log("%s: no System\\Font\\*.ttf to load", kName);
        return;
      }
      do {
        const std::string path = std::string("System\\Font\\") + found.cFileName;
        const int added = AddFontResourceExA(path.c_str(), FR_PRIVATE, nullptr);
        g_host->Log("%s: %s: %d font(s)", kName, path.c_str(), added);
      } while (FindNextFileA(search, &found));
      FindClose(search);
    }
  };

  int LoadEmbedFontHook(HANDLE*, const char*, int) {
    return 0;
  }

  HFONT WINAPI CreateFontIndirectHook(const LOGFONTA* font) {
    const std::size_t length = font ? std::strlen(font->lfFaceName) : 0;
    if (length > sizeof(kEot) - 1 && _stricmp(font->lfFaceName + length - (sizeof(kEot) - 1), kEot) == 0) {
      LOGFONTA stripped = *font;
      stripped.lfFaceName[length - (sizeof(kEot) - 1)] = '\0';
      return g_originalCreateFontIndirectA(&stripped);
    }
    return g_originalCreateFontIndirectA(font);
  }

  template <class Mode, std::size_t N>
  Mode ParseMode(const char* text, const char* const (&names)[N]) {
    for (std::size_t index = 0; index < N; ++index) {
      if (std::strcmp(text, names[index]) == 0) {
        return static_cast<Mode>(index);
      }
    }
    return static_cast<Mode>(0);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_host = host;
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }

  const char* face = host->SettingString(kName, "FontName");
  g_face = face[0] != '\0' ? face : nullptr;

  static const char* const heightModes[] = {"Unchanged", "Character", "Cell", "Offset", "Limits"};
  g_heightMode = ParseMode<HeightMode>(host->SettingString(kName, "HeightMode"), heightModes);
  g_height = static_cast<int>(host->SettingInt(kName, "Height"));
  g_heightOffset = static_cast<int>(host->SettingInt(kName, "HeightOffset"));
  g_heightMin = static_cast<int>(host->SettingInt(kName, "HeightMin"));
  g_heightMax = static_cast<int>(host->SettingInt(kName, "HeightMax"));

  static const char* const weightModes[] = {"Unchanged", "All", "Offset", "Separate"};
  g_weightMode = ParseMode<WeightMode>(host->SettingString(kName, "WeightMode"), weightModes);
  g_weight = static_cast<int>(host->SettingInt(kName, "Weight"));
  g_weightOffset = static_cast<int>(host->SettingInt(kName, "WeightOffset"));
  g_normalWeight = static_cast<int>(host->SettingInt(kName, "NormalWeight"));
  g_boldWeight = static_cast<int>(host->SettingInt(kName, "BoldWeight"));

  const char* charset = host->SettingString(kName, "Charset");
  for (const Charset& entry : kCharsets) {
    if (std::strcmp(charset, entry.name) == 0) {
      g_charset = entry.value;
    }
  }

  g_codePage = static_cast<int>(host->SettingInt(kName, "CodePage"));

  const bool fonts = g_face || g_heightMode != HeightMode::Unchanged || g_weightMode != WeightMode::Unchanged || g_charset >= 0;
  if (fonts && !(ro::Hook(host, &FontEntry::CreateSystemFont, &FontEntryHook::CreateSystemFont, g_originalCreateSystemFont, kName) &&
                 ro::HookSystem(host, "gdi32.dll", "CreateFontA", &CreateFontHook, g_originalCreateFontA, kName))) {
    return false;
  }

  if (g_codePage >= 0) {
    if (!ro::Hook(host, &InitializeLanguage, &InitializeLanguageHook, g_originalInitializeLanguage, kName) ||
        !ro::Hook(host, &LanguageCodePage, &LanguageCodePageHook, g_originalLanguageCodePage, kName)) {
      return false;
    }
    if (g_codePage != CP_UTF8 &&
        !ro::Hook(host, &UIChangeNameWnd::IsNameWithinLimit, &ChangeNameHook::IsNameWithinLimit, g_originalIsNameWithinLimit, kName)) {
      return false;
    }
  }

  const bool trueTypeFonts = host->SettingBool(kName, "TrueTypeFonts");
  const bool customFontsCharset = trueTypeFonts || host->SettingBool(kName, "CustomFontsCharset");
  if (customFontsCharset &&
      !ro::Hook(host, &TextContext::SetFont, &TextContextHook::SetFont, g_originalSetFont, kName)) {
    return false;
  }
  if (trueTypeFonts &&
      !(ro::Hook(host, &CSession::InitClientData, &SessionHook::InitClientData, g_originalInitClientData, kName) &&
        ro::Hook(host, &LoadEmbedFont, &LoadEmbedFontHook, g_originalLoadEmbedFont, kName) &&
        ro::HookSystem(host, "gdi32.dll", "CreateFontIndirectA", &CreateFontIndirectHook, g_originalCreateFontIndirectA, kName))) {
    return false;
  }

  host->Log("%s: face %s, height mode %d, weight mode %d, system charset %d, codepage %d, TrueType fonts %d, custom fonts charset %d",
            kName, g_face ? g_face : "(game)", static_cast<int>(g_heightMode), static_cast<int>(g_weightMode), g_charset,
            g_codePage, trueTypeFonts, customFontsCharset);
  return true;
}
