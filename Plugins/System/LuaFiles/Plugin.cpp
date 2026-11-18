// Hooks LuaScript::Load: the name is swapped for its registered replacement, then the registered extra files are
// loaded into the same LuaScript after it. Loads of the extra files go through the hook too, so they chain. A failed
// load of a name registered with LuaFiles_IgnoreFailure is logged with OutputDebugStringA and reported as success.
#include "RoPlugin.h"
#include "LuaFilesApi.h"

#include "Script/LuaScript.h"

#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace {
  constexpr const char* kName = "LuaFiles";

  std::vector<std::pair<std::string, std::string>> g_replacements;
  std::vector<std::pair<std::string, std::string>> g_extras;
  std::vector<std::string> g_ignored;

  bool (LuaScript::*g_original)(const char*, bool, bool);

  bool Same(const std::string& a, const char* b) {
    return lstrcmpiA(a.c_str(), b) == 0;
  }

  struct LuaScriptHook : LuaScript {
    bool Load(const char* name, bool resourcePath, bool looseOnly) {
      const char* loaded = name;
      for (auto it = g_replacements.rbegin(); it != g_replacements.rend(); ++it) {
        if (Same(it->first, name)) {
          loaded = it->second.c_str();
          break;
        }
      }
      bool result = (this->*g_original)(loaded, resourcePath, looseOnly);
      if (!result) {
        for (const auto& ignored : g_ignored) {
          if (Same(ignored, name)) {
            OutputDebugStringA(("LuaFiles: failed to load " + std::string(loaded) + ", ignored").c_str());
            result = true;
            break;
          }
        }
      }
      for (const auto& extra : g_extras) {
        if (Same(extra.first, name)) {
          this->Load(extra.second.c_str(), resourcePath, looseOnly);
        }
      }
      return result;
    }
  };
}

LUAFILES_API void LuaFiles_Replace(const char* original, const char* replacement) {
  g_replacements.emplace_back(original, replacement);
}

LUAFILES_API void LuaFiles_LoadAfter(const char* original, const char* extra) {
  g_extras.emplace_back(original, extra);
}

LUAFILES_API void LuaFiles_IgnoreFailure(const char* name) {
  g_ignored.emplace_back(name);
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &LuaScript::Load, &LuaScriptHook::Load, g_original, kName);
}
