// Error popups go to OutputDebugStringA instead of a message box, one bool setting per category; a hook is installed
// only when a category on needs it.
// - Lua: LuaScript::Error (every error of LuaScript::Load, Call and CallTable) formats the text and logs it.
//   LuaFiles_IgnoreFailure registers the five Lua Files\HatEffectInfo scripts, so LuaFiles' LuaScript::Load reports
//   success after logging a failure and CSession::LoadLuaDataFiles skips its MessageBoxA (those names are loaded
//   nowhere else). TraceValue,
//   CLuaInstance::LuaMessageBox, CLuaInstance::Execute and both CLuaInstance::LoadFile overloads are faithful copies
//   with MessageBoxA replaced, as they call it directly.
// - Quest: four sites (CUIOngoingQuestInfo, CUIRecommendedQuestInfo, UIRenewQuestUI and UIRenewQuestWnd::SendMsg) call
//   UIWindowMgr::Confirm, shared with every other confirm box, so its detour filters by the "not found" texts.
//   UIRenewDetailQuestInfoWnd::SetQuest calls MessageBoxA directly and is reimplemented.
// - Resource: the two standalone ShowErrorDialog overloads (std::string and const char*) prepare the render system for
//   a dialog as before, then log the text and return 0. The other two are inlined into their callers and keep their
//   message box: ShowErrorDialog(int) in C3dAttr::CellType, and ShowErrorDialog(CRenderSystemBase*, ...) in
//   CFile::OpenWrite and ImportWorldGround, which nothing calls in this source.
// - Palette: ShowErrorDialog(const char*, UINT), the overload CPaletteRes::Load calls when a palette file is missing:
//   that one message is logged and 0 returned, with no render system preparation.
// Overlap: Palette and Resource share one ShowErrorDialog(const char*, UINT) detour, installed when either is on.
// Resource on suppresses every text; else Palette suppresses the "CPaletteRes :: Cannot find File : " texts; any other
// text reaches the original. The std::string overload is hooked only when Resource is on.
extern "C" {
#include "../ThirdParty/lua/src/lua.h"
#include "../ThirdParty/lua/src/lauxlib.h"
}

#include "RoPlugin.h"
#include "LuaFilesApi.h"

#include "Client/Application/ErrorDialog.h"
#include "Contents/Quest/RenewQuestMgr.h"
#include "Contents/Tables/MessageTable.h"
#include "File/File.h"
#include "Render/DeviceContext/DCSurface.h"
#include "Render/RenderSystemBase.h"
#include "Resource/MSGSTRINGID.h"
#include "Script/LuaInstance.h"
#include "Script/LuaScript.h"
#include "UI/Quest/UIRenewQuestWnd.h"
#include "UI/Window/UIWindowMgr.h"
#include "Utility/Text/FormatString.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {
  constexpr const char* kName = "IgnoreErrors";

  // ---- Lua ----

  constexpr const char* kHatEffectScripts[] = {
    "Lua Files\\HatEffectInfo\\HatEffectIDs",        "Lua Files\\HatEffectInfo\\EffectHatItemTable",
    "Lua Files\\HatEffectInfo\\HatEffectInfo",       "Lua Files\\HatEffectInfo\\FootPrintEffectInfo",
    "Lua Files\\HatEffectInfo\\HatEffect_F",
  };

  void (LuaScript::*g_originalError)(const char*, const char*, ...);
  int (*g_originalTraceValue)(lua_State*);
  int (*g_originalLuaMessageBox)(lua_State*);
  bool (CLuaInstance::*g_originalExecute)(const std::string&);
  bool (CLuaInstance::*g_originalLoadFile)(const std::string&);
  bool (CLuaInstance::*g_originalLoadFileShowError)(const std::string&, bool);

  struct LuaScriptHook : LuaScript {
    void Error(const char*, const char* format, ...) {
      char text[500];
      va_list arguments;
      va_start(arguments, format);
      _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments);
      va_end(arguments);
      OutputDebugStringA(text);
    }
  };

  int TraceValue(lua_State* state) {
    std::string text = luaL_checkstring(state, 1);
    OutputDebugStringA(text.c_str());
    return 1;
  }

  int LuaMessageBox(lua_State* state) {
    const std::string text = lua_tostring(state, -1);
    OutputDebugStringA(text.c_str());
    return 0;
  }

  struct LuaInstanceHook : CLuaInstance {
    bool Execute(const std::string& name) {
      lua_getglobal(this->m_state, name.c_str());

      const int result = lua_pcall(this->m_state, 0, 0, 0);

      if (result != 0) {
        OutputDebugStringA(lua_tostring(this->m_state, -1));
        return false;
      }

      return true;
    }

    bool LoadFile(const std::string& path, bool showError) {
      if (luaL_loadfile(this->m_state, path.c_str()) == 0 && lua_pcall(this->m_state, 0, LUA_MULTRET, 0) == 0) {
        return true;
      }

      if (showError) {
        OutputDebugStringA(lua_tostring(this->m_state, -1));
      }

      return false;
    }

    bool LoadFile(const std::string& path) {
      CFile file;
      if (!file.Open(path.c_str(), 0, true, false)) {
        return false;
      }

      char* buffer = new char[file.GetSize() + 1];
      SecureZeroMemory(buffer, file.GetSize() + 1);
      file.Read(buffer, file.GetSize());
      file.Close();

      if (luaL_loadbuffer(this->m_state, buffer, file.GetSize(), "buf") == 0 && lua_pcall(this->m_state, 0, 0, 0) == 0) {
        delete[] buffer;
        return true;
      }

      const char* error = lua_tostring(this->m_state, -1);
      OutputDebugStringA(error);
      lua_pop(this->m_state, 1);
      delete[] buffer;

      return false;
    }
  };

  using LoadFileShowError = bool (CLuaInstance::*)(const std::string&, bool);
  using LoadFilePath = bool (CLuaInstance::*)(const std::string&);
  using LoadFileShowErrorDetour = bool (LuaInstanceHook::*)(const std::string&, bool);
  using LoadFilePathDetour = bool (LuaInstanceHook::*)(const std::string&);

  bool HookLua(const RoHostApi* host) {
    // Error (private) and LuaMessageBox (private static) cannot be named here; TraceValue has no header declaration.
    for (const char* script : kHatEffectScripts) {
      LuaFiles_IgnoreFailure(script);
    }
    return ro::HookVirtual(host, "?Error@LuaScript@@AAAXPBD0ZZ", &LuaScriptHook::Error, g_originalError, kName) &&
           ro::HookVirtual(host, "?TraceValue@@YAHPAUlua_State@@@Z", &TraceValue, g_originalTraceValue, kName) &&
           ro::HookVirtual(host, "?LuaMessageBox@CLuaInstance@@CAHPAUlua_State@@@Z", &LuaMessageBox, g_originalLuaMessageBox, kName) &&
           ro::Hook(host, &CLuaInstance::Execute, &LuaInstanceHook::Execute, g_originalExecute, kName) &&
           ro::Hook(host, static_cast<LoadFilePath>(&CLuaInstance::LoadFile), static_cast<LoadFilePathDetour>(&LuaInstanceHook::LoadFile),
                    g_originalLoadFile, kName) &&
           ro::Hook(host, static_cast<LoadFileShowError>(&CLuaInstance::LoadFile),
                    static_cast<LoadFileShowErrorDetour>(&LuaInstanceHook::LoadFile), g_originalLoadFileShowError, kName);
  }

  // ---- Quest ----

  constexpr char kNotFoundPrefix[] = "Not found Quest Info";

  int (UIWindowMgr::*g_originalConfirm)(const char*, int, int, bool, int, const char*, int, int, unsigned char);
  void (UIRenewDetailQuestInfoWnd::*g_originalSetQuest)(unsigned int, bool);

  struct ConfirmHook : UIWindowMgr {
    int Confirm(const char* text, int type, int defaultAccept, bool replace, int timeout, const char* caption, int width, int height,
                unsigned char protect) {
      if (text && (std::strncmp(text, kNotFoundPrefix, sizeof(kNotFoundPrefix) - 1) == 0 || std::strcmp(text, "not found quest info") == 0)) {
        OutputDebugStringA(text);
        return 0;
      }
      return (this->*g_originalConfirm)(text, type, defaultAccept, replace, timeout, caption, width, height, protect);
    }
  };

  struct DetailQuestHook : UIRenewDetailQuestInfoWnd {
    void SetQuest(unsigned int questId, bool recommended) {
      if (questId == 0) {
        return;
      }

      this->m_recommended = recommended;
      this->m_questId = questId;
      this->m_timeText = "";
      if (recommended) {
        this->m_ongoingWnd->Show(0);
        this->m_recommendedWnd->Show(1);
        this->m_recommendedWnd->SetQuest(this->m_questId);
        this->m_imageName = g_renewQuestMgr->GetRecommendedQuest(questId).backgroundImage;
        if (!this->m_imageName.empty()) {
          this->m_imageName = "유저인터페이스\\renew_questui\\" + this->m_imageName;
        }
      } else {
        const RenewQuestOngoingInfo& quest = g_renewQuestMgr->GetOngoingQuest(questId);
        if (quest.m_title.empty()) {
          OutputDebugStringA(FormatOwnedString("Not found Quest Info = %lu", questId).c_str());
          return;
        }

        this->m_recommendedWnd->Show(0);
        this->m_ongoingWnd->Show(1);
        this->m_ongoingWnd->SetQuest(this->m_questId);
        if (quest.m_year > 1970) {
          const std::string timeText = FormatOwnedString("%s [%02d/%02d/%02d %02d:%02d]", LookupMessage(MSI_QUESTUI_CLOSINGTIME), quest.m_year,
                                                         quest.m_month, quest.m_day, quest.m_hour, quest.m_minute);
          this->m_timeText = timeText;
        }
      }

      this->Invalidate();
    }
  };

  bool HookQuest(const RoHostApi* host) {
    return ro::Hook(host, &UIWindowMgr::Confirm, &ConfirmHook::Confirm, g_originalConfirm, kName) &&
           ro::Hook(host, &UIRenewDetailQuestInfoWnd::SetQuest, &DetailQuestHook::SetQuest, g_originalSetQuest, kName);
  }

  // ---- Palette / Resource (ShowErrorDialog) ----

  constexpr char kPalettePrefix[] = "CPaletteRes :: Cannot find File : ";

  using StringOverload = int (*)(const std::string&, const UINT&);
  using TextOverload = int (*)(const char*, UINT);

  bool g_resource;
  StringOverload g_originalString;
  TextOverload g_originalText;
  // GetRenderSystemHolder() is inline with a local static: call the exe's copy.
  RenderSystemHolder* (*g_getRenderSystemHolder)();

  int ShowStringDetour(const std::string& text, const UINT&) {
    g_getRenderSystemHolder()->PrepareErrorDialog();
    OutputDebugStringA(text.c_str());
    return 0;
  }

  int ShowTextDetour(const char* text, UINT type) {
    if (g_resource) {
      g_getRenderSystemHolder()->PrepareErrorDialog();
      OutputDebugStringA(text);
      return 0;
    }
    if (text && std::strncmp(text, kPalettePrefix, sizeof(kPalettePrefix) - 1) == 0) {
      OutputDebugStringA(text);
      return 0;
    }
    return g_originalText(text, type);
  }

  bool HookErrorDialog(const RoHostApi* host) {
    if (g_resource) {
      g_getRenderSystemHolder =
          reinterpret_cast<RenderSystemHolder* (*)()>(host->Resolve("?GetRenderSystemHolder@@YAPAURenderSystemHolder@@XZ"));
      if (!g_getRenderSystemHolder) {
        host->Log("%s: no exe function GetRenderSystemHolder", kName);
        return false;
      }
      if (!ro::Hook(host, static_cast<StringOverload>(&ShowErrorDialog), &ShowStringDetour, g_originalString, kName)) {
        return false;
      }
    }
    return ro::Hook(host, static_cast<TextOverload>(&ShowErrorDialog), &ShowTextDetour, g_originalText, kName);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool lua = host->SettingBool(kName, "Lua");
  const bool quest = host->SettingBool(kName, "Quest");
  const bool palette = host->SettingBool(kName, "Palette");
  g_resource = host->SettingBool(kName, "Resource");
  if (!lua && !quest && !palette && !g_resource) {
    host->Log("%s: Lua, Palette, Quest and Resource are all off, nothing to do", kName);
    return true;
  }
  // The text detour runs with Resource off only when Palette is on, so it needs no Palette flag.
  return (!lua || HookLua(host)) && (!quest || HookQuest(host)) && (!(palette || g_resource) || HookErrorDialog(host));
}
