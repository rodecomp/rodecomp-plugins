// Registers the Replace and LoadAfter settings with LuaFiles, both names prefixed with "Lua Files\".
#include "RoPlugin.h"
#include "LuaFilesApi.h"

#include <cstring>
#include <string>

namespace {
  constexpr const char* kName = "LuaOverrides";
  constexpr const char* kPrefix = "Lua Files\\";

  template <class Register>
  void Read(const RoHostApi* host, const char* key, Register add) {
    const int count = host->SettingCount(kName, key);
    for (int index = 0; index < count; ++index) {
      const char* item = host->SettingItem(kName, key, index);
      const char* equals = std::strchr(item, '=');
      if (!equals || equals == item || equals[1] == '\0') {
        host->Log("%s: %s entry \"%s\" is not Original=File, ignored", kName, key, item);
        continue;
      }
      const std::string original = kPrefix + std::string(item, equals);
      const std::string file = kPrefix + std::string(equals + 1);
      add(original.c_str(), file.c_str());
    }
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  Read(host, "Replace", LuaFiles_Replace);
  Read(host, "LoadAfter", LuaFiles_LoadAfter);
  return true;
}
