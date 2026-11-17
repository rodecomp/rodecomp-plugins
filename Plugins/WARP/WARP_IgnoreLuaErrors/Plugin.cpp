// Lua error popups go to OutputDebugStringA:
// - LuaScript::Error (every error of LuaScript::Load, Call and CallTable) formats the text and logs it.
// - LuaScript::Load reports success for the five Lua Files\HatEffectInfo scripts after logging a failure, so
//   CSession::LoadLuaDataFiles skips its MessageBoxA (those names are loaded nowhere else).
// - TraceValue, CLuaInstance::LuaMessageBox, CLuaInstance::Execute and both CLuaInstance::LoadFile overloads are
//   faithful copies with MessageBoxA replaced, as they call it directly.
extern "C" {
#include "../ThirdParty/lua/src/lua.h"
#include "../ThirdParty/lua/src/lauxlib.h"
}

#include "RoPlugin.h"

#include "File/File.h"
#include "Script/LuaInstance.h"
#include "Script/LuaScript.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_IgnoreLuaErrors";

  constexpr const char* kHatEffectScripts[] = {
    "Lua Files\\HatEffectInfo\\HatEffectIDs",        "Lua Files\\HatEffectInfo\\EffectHatItemTable",
    "Lua Files\\HatEffectInfo\\HatEffectInfo",       "Lua Files\\HatEffectInfo\\FootPrintEffectInfo",
    "Lua Files\\HatEffectInfo\\HatEffect_F",
  };
  constexpr char kHatEffectDirectory[] = "Lua Files\\";

  void (LuaScript::*g_originalError)(const char*, const char*, ...);
  bool (LuaScript::*g_originalLoad)(const char*, bool, bool);
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

    bool Load(const char* name, bool resourcePath, bool looseOnly) {
      if ((this->*g_originalLoad)(name, resourcePath, looseOnly)) {
        return true;
      }
      for (const char* script : kHatEffectScripts) {
        if (std::strcmp(name, script) == 0) {
          // Same text as the MessageBoxA of CSession::LoadLuaDataFiles.
          OutputDebugStringA(("Fail to load " + std::string(name + sizeof(kHatEffectDirectory) - 1) + ".lua").c_str());
          return true;
        }
      }
      return false;
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
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  // Error (private) and LuaMessageBox (private static) cannot be named here; TraceValue has no header declaration.
  return ro::HookVirtual(host, "?Error@LuaScript@@AAAXPBD0ZZ", &LuaScriptHook::Error, g_originalError, kName) &&
         ro::Hook(host, &LuaScript::Load, &LuaScriptHook::Load, g_originalLoad, kName) &&
         ro::HookVirtual(host, "?TraceValue@@YAHPAUlua_State@@@Z", &TraceValue, g_originalTraceValue, kName) &&
         ro::HookVirtual(host, "?LuaMessageBox@CLuaInstance@@CAHPAUlua_State@@@Z", &LuaMessageBox, g_originalLuaMessageBox, kName) &&
         ro::Hook(host, &CLuaInstance::Execute, &LuaInstanceHook::Execute, g_originalExecute, kName) &&
         ro::Hook(host, static_cast<LoadFilePath>(&CLuaInstance::LoadFile), static_cast<LoadFilePathDetour>(&LuaInstanceHook::LoadFile),
                  g_originalLoadFile, kName) &&
         ro::Hook(host, static_cast<LoadFileShowError>(&CLuaInstance::LoadFile),
                  static_cast<LoadFileShowErrorDetour>(&LuaInstanceHook::LoadFile), g_originalLoadFileShowError, kName);
}
