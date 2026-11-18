// Each setting names a file the client loads; a non-empty value replaces that file's path. The client opens them through
// five functions, and each is hooked only when one of its files is overridden:
// - luaL_loadfile: AchievementList, CheckAttendance, ChangeMaterial, ItemInfo, MapInfo, MonsterSizeEffect,
//   PetEvolutionCln, PrivateAirplane, Tipbox (both the .lua and the .lub attempt), Towninfo.
// - LoadLuaResource: spopup and the Rune\ files loaded by CRuneSystemMgr::LoadScripts.
// - LuaScript::Load (name without extension): OngoingQuestInfoList and RecommendedQuestInfoList (both the _Sakray and
//   _True names), Rune\runesystemInfo.
// - CLuaInstance::LoadFile(path, showError): QuestClassificationInfo.
// - CFile::Open: clientinfo.xml (opened by the ClientInfo plugin; Gravity's exe has no reader of its own).
// - CSoundMgr::PlayStream: bgm\01.mp3, the login music and the default BGM of maps without one.
// - InitializeServiceStrings sets g_licenceFileName; the detour replaces it afterwards.
// Names are compared case-insensitively with the literal the client passes.
extern "C" {
#include "../ThirdParty/lua/src/lua.h"
#include "../ThirdParty/lua/src/lauxlib.h"
}

#include "RoPlugin.h"

#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Sound/SoundMgr.h"
#include "File/File.h"
#include "Script/LuaFileOperations.h"
#include "Script/LuaInstance.h"
#include "Script/LuaScript.h"

#include <cstring>
#include <string>

namespace {
  constexpr const char* kName = "CustomPaths";

  enum Loader { kLoadFile, kLuaResource, kLuaScript, kLuaInstance, kFileOpen, kStream, kLoaderCount };

  struct Path {
    const char* setting;
    Loader loader;
    const char* originals[2];
    const char* replacement;
  };

  Path g_paths[] = {
    {"DefaultBgm", kStream, {"bgm\\01.mp3"}},
    {"ItemInfo", kLoadFile, {"SystemEN\\iteminfo.lua"}},
    {"AchievementList", kLoadFile, {"system\\Achievement_list.lub"}},
    {"MonsterSizeEffect", kLoadFile, {"System\\monster_size_effect_new.lub"}},
    {"TownInfo", kLoadFile, {"System\\Towninfo.lub"}},
    {"PetEvolution", kLoadFile, {"system\\PetEvolutionCln_true.lub"}},
    {"Tipbox", kLoadFile, {"System\\tipbox.lua", "System\\tipbox.lub"}},
    {"CheckAttendance", kLoadFile, {"System\\CheckAttendance.lub"}},
    {"PrivateAirplane", kLoadFile, {"System\\PrivateAirplane_True.lub"}},
    {"MapInfo", kLoadFile, {"system\\mapInfo_true.lub"}},
    {"OngoingQuestInfoList", kLuaScript, {"system\\OngoingQuestInfoList_True", "system\\OngoingQuestInfoList_Sakray"}},
    {"RecommendedQuestInfoList", kLuaScript, {"system\\RecommendedQuestInfoList_True", "system\\RecommendedQuestInfoList_Sakray"}},
    {"Spopup", kLuaResource, {"System\\spopup.lub"}},
    {"ChangeMaterial", kLoadFile, {"System\\ChangeMaterial.lub"}},
    {"QuestClassificationInfo", kLuaInstance, {"System\\QuestClassificationInfo.lub"}},
    {"RuneDesc", kLuaResource, {"System\\Rune\\rune_desc.lub"}},
    {"RuneSetDesc", kLuaResource, {"System\\Rune\\runeset_desc.lub"}},
    {"ItemDecom", kLuaResource, {"System\\Rune\\itemDecom.lub"}},
    {"RuneInfo", kLuaResource, {"System\\Rune\\rune_info.lub"}},
    {"RuneSetInfo", kLuaResource, {"System\\Rune\\runeset_info.lub"}},
    {"RuneSystemTable", kLuaResource, {"System\\Rune\\runeSystem_table.lub"}},
    {"RuneSystemId", kLuaResource, {"System\\Rune\\runesystemid.lub"}},
    {"RuneSystemInfo", kLuaScript, {"System\\Rune\\runesystemInfo"}},
    {"RuneSetReward", kLuaResource, {"System\\Rune\\runeset_reward.lub"}},
    {"ClientInfo", kFileOpen, {"clientinfo.xml"}},
  };

  const char* g_licence;

  // The replacement for `name` among the overridden paths of `loader`, or nullptr.
  const char* Replacement(Loader loader, const char* name) {
    if (name == nullptr) {
      return nullptr;
    }
    for (const Path& path : g_paths) {
      if (path.loader != loader || path.replacement == nullptr) {
        continue;
      }
      for (const char* original : path.originals) {
        if (original != nullptr && _stricmp(original, name) == 0) {
          return path.replacement;
        }
      }
    }
    return nullptr;
  }

  decltype(&luaL_loadfile) g_loadFile;
  decltype(&LoadLuaResource) g_loadLuaResource;
  bool (LuaScript::*g_luaScriptLoad)(const char*, bool, bool);
  bool (CLuaInstance::*g_luaInstanceLoad)(const std::string&, bool);
  bool (CFile::*g_fileOpen)(const char*, int, bool, bool);
  void (CSoundMgr::*g_playStream)(const char*, int, bool);
  decltype(&InitializeServiceStrings) g_initializeServiceStrings;

  int LoadFileDetour(lua_State* state, const char* filename) {
    const char* replacement = Replacement(kLoadFile, filename);
    return g_loadFile(state, replacement ? replacement : filename);
  }

  bool LoadLuaResourceDetour(lua_State* state, const std::string& name, unsigned char allowLuaFallback) {
    if (const char* replacement = Replacement(kLuaResource, name.c_str())) {
      return g_loadLuaResource(state, std::string(replacement), allowLuaFallback);
    }
    return g_loadLuaResource(state, name, allowLuaFallback);
  }

  struct LuaScriptHook : LuaScript {
    bool Load(const char* name, bool resourcePath, bool looseOnly) {
      const char* replacement = Replacement(kLuaScript, name);
      return (this->*g_luaScriptLoad)(replacement ? replacement : name, resourcePath, looseOnly);
    }
  };

  struct LuaInstanceHook : CLuaInstance {
    bool LoadFile(const std::string& path, bool showError) {
      if (const char* replacement = Replacement(kLuaInstance, path.c_str())) {
        return (this->*g_luaInstanceLoad)(std::string(replacement), showError);
      }
      return (this->*g_luaInstanceLoad)(path, showError);
    }
  };

  struct FileHook : CFile {
    bool Open(const char* path, int mode, bool resourcePath, bool option) {
      const char* replacement = Replacement(kFileOpen, path);
      return (this->*g_fileOpen)(replacement ? replacement : path, mode, resourcePath, option);
    }
  };

  struct SoundMgrHook : CSoundMgr {
    void PlayStream(const char* name, int loopCount, bool fade) {
      const char* replacement = Replacement(kStream, name);
      (this->*g_playStream)(replacement ? replacement : name, loopCount, fade);
    }
  };

  void InitializeServiceStringsDetour(char* protection) {
    g_initializeServiceStrings(protection);
    g_licenceFileName = g_licence;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  bool used[kLoaderCount] = {};
  for (Path& path : g_paths) {
    const char* value = host->SettingString(kName, path.setting);
    if (*value != '\0') {
      path.replacement = value;
      used[path.loader] = true;
      host->Log("%s: %s -> %s", kName, path.originals[0], value);
    }
  }
  const char* licence = host->SettingString(kName, "Licence");
  if (*licence != '\0') {
    g_licence = licence;
    host->Log("%s: licence -> %s", kName, licence);
  }

  bool (CLuaInstance::*luaInstanceLoad)(const std::string&, bool) = &CLuaInstance::LoadFile;
  bool (CFile::*fileOpen)(const char*, int, bool, bool) = &CFile::Open;
  return (!used[kLoadFile] || ro::Hook(host, &luaL_loadfile, &LoadFileDetour, g_loadFile, kName)) &&
         (!used[kLuaResource] || ro::Hook(host, &LoadLuaResource, &LoadLuaResourceDetour, g_loadLuaResource, kName)) &&
         (!used[kLuaScript] || ro::Hook(host, &LuaScript::Load, &LuaScriptHook::Load, g_luaScriptLoad, kName)) &&
         (!used[kLuaInstance] || ro::Hook(host, luaInstanceLoad, &LuaInstanceHook::LoadFile, g_luaInstanceLoad, kName)) &&
         (!used[kFileOpen] || ro::Hook(host, fileOpen, &FileHook::Open, g_fileOpen, kName)) &&
         (!used[kStream] || ro::Hook(host, &CSoundMgr::PlayStream, &SoundMgrHook::PlayStream, g_playStream, kName)) &&
         (g_licence == nullptr ||
          ro::Hook(host, &InitializeServiceStrings, &InitializeServiceStringsDetour, g_initializeServiceStrings, kName));
}
