// Data limits and loading rules; each setting installs its own hooks, only when it changes something:
// - EmblemMaxSize: CGuildMgr::Create sets m_timeout, the emblem size limit that CEmblemRes::Load (decompression buffer,
//   1.2x) and the emblem upload check read.
// - ViewIdLimit: CSession::InitResourceNameTables sizes the headgear name table to 3000; the detour grows it and asks
//   ReqAccName for the ids from 3000 up, as the original does below 3000.
// - No3DBoneLimit: C3dGrannyModelRes::PlayAnimation gives up for 3D monster ids above 9. For those ids (up to the
//   library's 100) the detour loads model\3dmob_bone\<id>_<action>.gr2 for every action, as the original does for
//   actions 1 to 4 below 10, and the "too many vertex granny model!" error box is dropped.
// - EffectToolAllMaps: EffectLists::LoadMapEffectScript returns before loading Lua Files\EffectTool\<map> when the map
//   has no emitter effect; the detour runs the rest of the function on those maps.
// - StateIconsFromLub: CSession::GetEFSTImgFileName returns only what stateiconimginfo's GetEFSTImgFileName gives, never
//   its built-in icons (nullptr when the Lua function returns nothing).
// - ItemInfoPerServer: CItemInfoMgr::Load does nothing at launch. When a character server is chosen
//   (CCheckAttendanceMgr::LoadLocal, called right after g_session.m_serverName is set) the item info file is run and
//   its main is called with the server name, then the items are copied into g_itemDescriptions, which plugins cannot
//   link: CItemInfoMgr::SetItemInfo, called at launch by LoadItemNameTables, records its address. Choosing a server
//   again reloads: the Lua description lines of the reloaded items replace the previous ones.
// - CaptchaBufferSize: the two AddImageData functions are reimplemented with the decompression buffer size as setting.
// - HomunAiSkills: the AI's SkillGround / SkillObject accept every skill, not only homunculus and mercenary skills.
extern "C" {
#include "../ThirdParty/lua/src/lua.h"
#include "../ThirdParty/lua/src/lauxlib.h"
#include "../ThirdParty/lua/src/lualib.h"
}
#include "../ThirdParty/zlib/zlib.h"

#include "RoPlugin.h"

#include "Client/Application/ErrorDialog.h"
#include "Client/Mode/CGameMode.h"
#include "Client/Mode/ModeMessage.h"
#include "Client/Mode/ModeMgr.h"
#include "Client/Session/Session.h"
#include "Contents/Actors/ActorStateFlags.h"
#include "Contents/Actors/GameActor.h"
#include "Contents/Actors/Pc.h"
#include "Contents/Effects/Core/EffectLists.h"
#include "Contents/Effects/Particles/Particle.h"
#include "Contents/Events/CheckAttendanceMgr.h"
#include "Contents/Guild/GuildMgr.h"
#include "Contents/Items/ItemDescription.h"
#include "Contents/Items/ItemInfoMgr.h"
#include "Contents/Skills/SkillInfo.h"
#include "Contents/World/World.h"
#include "File/ResourcePath.h"
#include "Network/Connection/Connection.h"
#include "Network/Packets/PacketId.h"
#include "Resource/Models/GrannyModelResource.h"
#include "Script/LuaScript.h"
#include "Script/NLuaState.h"
#include "UI/Buttons/UIBmpRadioBtn.h"
#include "UI/Macro/UICaptchaPreviewWnd.h"
#include "UI/Macro/UIMacroDetectorWnd.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <windows.h>

extern const char* g_animationNames[5];
// Declared only in MercenaryAI.cpp.
int AiSkillGround(lua_State* state);
int AiSkillObject(lua_State* state);

namespace {
  constexpr const char* kName = "DataTweaks";
  constexpr int kDefaultViewIds = 3000;
  constexpr const char* kDefaultAccessory = "_병아리모자";  // the name the original fills unknown ids with
  constexpr unsigned kFirstBoneJob = 10;
  constexpr unsigned kLibraryJobs = 100;

  std::map<int, ITEM_DESC>* g_descriptions;
  int g_emblemMaxSize;
  int g_viewIdLimit;
  uLongf g_captchaSize;

  // ---- EmblemMaxSize ----
  decltype(&CGuildMgr::Create) g_guildMgrCreate;

  bool GuildMgrCreateDetour() {
    const bool created = g_guildMgrCreate();
    if (created) {
      g_guildMgr->m_timeout = g_emblemMaxSize;
    }
    return created;
  }

  // ---- ViewIdLimit ----
  void (CSession::*g_initResourceNameTables)();

  struct SessionHook : CSession {
    void InitResourceNameTablesDetour() {
      (this->*g_initResourceNameTables)();
      this->m_accessoryNames.resize(g_viewIdLimit, kDefaultAccessory);
      for (int index = kDefaultViewIds; index < g_viewIdLimit; ++index) {
        const char* name = nullptr;
        if (this->m_hotkeyScript->Call(std::string("ReqAccName"), "d>s", index, &name) && name != nullptr) {
          this->m_accessoryNames[index] = name;
        }
      }
    }

    const char* GetEFSTImgFileNameDetour(int efstId, int type) {
      const char* fileName = nullptr;
      this->m_hotkeyScript->Call("GetEFSTImgFileName", "dd>s", efstId, type, &fileName);
      return fileName != nullptr && *fileName != '\0' ? fileName : nullptr;
    }
  };

  // ---- No3DBoneLimit ----
  granny_control* (C3dGrannyModelRes::*g_playAnimation)(unsigned, granny_model_instance*, granny_control*, int);
  decltype(static_cast<int (*)(const char*, UINT)>(&ShowErrorDialog)) g_showErrorDialog;

  struct GrannyHook : C3dGrannyModelRes {
    granny_control* PlayAnimationDetour(unsigned action, granny_model_instance* instance, granny_control* control, int loopCount) {
      const unsigned job = this->job;
      if (control != nullptr || job < kFirstBoneJob || job >= kLibraryJobs || action > 4) {
        return (this->*g_playAnimation)(action, instance, control, loopCount);
      }
      GrannyAnimationLibrary* library = this->animations;
      if (library->animations[job][action] == nullptr) {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "model\\3dmob_bone\\%u_%s.gr2", job, g_animationNames[action]);
        if (!ResourceExists(path, false)) {
          return nullptr;
        }
        library->Load(path, job, action);
        if (library->animations[job][action] == nullptr) {
          return nullptr;
        }
      }
      // The original plays an animation already in the library whatever the id.
      return (this->*g_playAnimation)(action, instance, control, loopCount);
    }
  };

  int ShowErrorDialogDetour(const char* text, UINT type) {
    if (text != nullptr && std::strcmp(text, "too many vertex granny model!") == 0) {
      return 0;
    }
    return g_showErrorDialog(text, type);
  }

  // ---- EffectToolAllMaps ----
  void (EffectLists::*g_loadMapEffectScript)(bool);

  struct EffectListsHook : EffectLists {
    void LoadMapEffectScriptDetour(bool reload) {
      if (!this->indexed.empty()) {
        (this->*g_loadMapEffectScript)(reload);
        return;
      }
      const std::string name = CurrentMapScriptName();
      const std::string path = "Lua Files\\EffectTool\\" + name;
      if (!HasEffectScript(path, name) && !g_session.m_hotkeyScript->Load(path.c_str(), true, false)) {
        return;
      }
      int version = 0;
      const std::string versionKey = EmitterKey(name, "effect_version");
      g_session.m_hotkeyScript->Call("GetValue", "s>d", versionKey.c_str(), &version);
      switch (version) {
      case 0:
      case 1:
        ReadEffectScriptV1(name);
        break;
      case 2:
        ReadEffectScriptV2(name);
        break;
      }
    }
  };

  // ---- StateIconsFromLub ----
  const char* (CSession::*g_getEFSTImgFileName)(int, int);

  // ---- ItemInfoPerServer ----
  bool (CItemInfoMgr::*g_itemInfoLoad)();
  void (CItemInfoMgr::*g_setItemInfo)(std::map<int, ITEM_DESC>&) const;
  bool (CCheckAttendanceMgr::*g_loadLocal)(std::string);

  // CItemInfoMgr::Load with the server name passed to main.
  bool LoadItemInfo(const std::string& server) {
    NLuaState lua;
    if (!lua.IsValid()) {
      return false;
    }
    lua_State* state = lua.GetState();
    luaL_openlibs(state);
    lua_register(state, "AddItem", CItemInfoMgr::LuaAddItem);
    lua_register(state, "AddItemUnidentifiedDesc", CItemInfoMgr::LuaAddItemUnidentifiedDesc);
    lua_register(state, "AddItemIdentifiedDesc", CItemInfoMgr::LuaAddItemIdentifiedDesc);
    lua_register(state, "AddItemEffectInfo", CItemInfoMgr::LuaAddItemEffectInfo);
    lua_register(state, "AddItemIsCostume", CItemInfoMgr::LuaAddItemIsCostume);
    lua_register(state, "AddItemPackageID", CItemInfoMgr::LuaAddItemPackageID);
    if (luaL_loadfile(state, "SystemEN\\iteminfo.lua") != 0 || lua_pcall(state, 0, 0, 0) != 0) {
      MessageBoxA(nullptr, lua_tostring(state, -1), "CItemInfoMgr", 0);
      return false;
    }
    lua_getglobal(state, "main");
    lua_pushstring(state, server.c_str());
    if (lua_pcall(state, 1, 2, 0) != 0) {
      MessageBoxA(nullptr, lua_tostring(state, -1), "CItemInfoMgr", 0);
      return false;
    }
    if (!lua_isstring(state, -1)) {
      MessageBoxA(nullptr, "second return value is not string...", "CItemInfoMgr", 0);
      return false;
    }
    if (!lua_toboolean(state, -2)) {
      const std::string message = std::string("error found :") + lua_tostring(state, -1);
      MessageBoxA(nullptr, message.c_str(), "CItemInfoMgr", 0);
      return false;
    }
    return true;
  }

  struct ItemInfoHook : CItemInfoMgr {
    // At launch: the file is run once a character server is chosen.
    bool LoadDetour() {
      return true;
    }

    // LoadItemNameTables calls it at launch with g_itemDescriptions, a global plugins cannot link.
    void SetItemInfoDetour(std::map<int, ITEM_DESC>& descriptions) const {
      g_descriptions = &descriptions;
      (this->*g_setItemInfo)(descriptions);
    }
  };

  struct CheckAttendanceHook : CCheckAttendanceMgr {
    bool LoadLocalDetour(std::string suffix) {
      if (g_descriptions == nullptr) {
        return (this->*g_loadLocal)(std::move(suffix));
      }
      for (auto& item : g_itemInfoMgr->m_items) {
        ITEM_DESC& desc = EnsureItemDescription(item.first, *g_descriptions);
        desc.identifiedDescription.clear();
        desc.unidentifiedDescription.clear();
        item.second.identifiedDescription.clear();
        item.second.unidentifiedDescription.clear();
      }
      if (LoadItemInfo(g_session.m_serverName)) {
        g_itemInfoMgr->SetItemInfo(*g_descriptions);
      }
      return (this->*g_loadLocal)(std::move(suffix));
    }
  };

  // ---- CaptchaBufferSize ----
  void (UICaptchaPreviewWnd::*g_previewAddImageData)(const void*, int);
  void (UIMacroDetectorWnd::*g_detectorAddImageData)(const void*, int);

  struct MacroDetectorImageReceivedPacket {
    std::int16_t id;
  };

  struct CaptchaPreviewHook : UICaptchaPreviewWnd {
    void AddImageDataDetour(const void* data, int size) {
      memcpy_s(this->m_imageData + this->m_imageReceived, 0x400, data, size);
      this->m_imageReceived += size;
      if (this->m_imageSize == this->m_imageReceived && this->m_imageData && this->m_imageReceived) {
        uLongf length = g_captchaSize;
        Bytef* image = new (std::nothrow) Bytef[g_captchaSize];
        uncompress(image, &length, this->m_imageData, this->m_imageReceived);
        this->m_image->m_bitmap.DecodeBitmap(image, length, false);
        delete[] image;
        this->Show(1);
      }
    }
  };

  struct MacroDetectorHook : UIMacroDetectorWnd {
    void AddImageDataDetour(const void* data, int size) {
      memcpy_s(this->m_imageData + this->m_imageReceived, 0x400, data, size);
      this->m_imageReceived += size;
      if (this->m_imageSize != this->m_imageReceived) {
        return;
      }
      if (this->m_imageData && this->m_imageReceived) {
        uLongf length = g_captchaSize;
        Bytef* image = new (std::nothrow) Bytef[g_captchaSize];
        uncompress(image, &length, this->m_imageData, this->m_imageReceived);
        this->m_image->m_bitmap.DecodeBitmap(image, length, false);
        delete[] image;
        this->m_image->Invalidate();
        this->Invalidate();
      }
      MacroDetectorImageReceivedPacket packet;
      packet.id = ID_CZ_COMPLETE_APPLY_MACRO_DETECTOR_CAPTCHA;
      GetRagConnection().Send(GetRagConnection().GetPacketSize(packet.id), &packet);
    }
  };

  // ---- HomunAiSkills ----
  decltype(&AiSkillGround) g_aiSkillGround;
  decltype(&AiSkillObject) g_aiSkillObject;

  bool UsableState(const CGameActor* actor) {
    const std::uint32_t state = actor->render.state;
    return state == 1 || state == 0 || state == 4;
  }

  int AiSkillGroundDetour(lua_State* state) {
    const std::uint32_t aid = static_cast<std::uint32_t>(luaL_checknumber(state, 1));
    int skillArgs[4];
    skillArgs[0] = static_cast<int>(luaL_checknumber(state, 3));
    skillArgs[1] = static_cast<int>(luaL_checknumber(state, 4));
    skillArgs[2] = static_cast<int>(luaL_checknumber(state, 5));
    skillArgs[3] = static_cast<int>(luaL_checknumber(state, 2));
    if (!g_session.IsHomunAid(aid) && !g_session.IsMercenaryAid(aid)) {
      return 0;
    }
    CGameMode* mode = g_modeMgr.GetGameMode();
    if (CGameActor* actor = mode->scene->GetGameActorByAID(aid); actor != nullptr && UsableState(actor)) {
      mode->SendMsg(MM_USE_SKILL_TOGROUND, reinterpret_cast<int>(skillArgs), 0, 0, 0);
    }
    mode->SendMsg(MM_SETLOCKONMOUSE, 0, 0, 0, 0);
    return 0;
  }

  int AiSkillObjectDetour(lua_State* state) {
    const std::uint32_t aid = static_cast<std::uint32_t>(luaL_checknumber(state, 1));
    const unsigned level = static_cast<unsigned>(luaL_checknumber(state, 2));
    const std::uint32_t skillId = static_cast<std::uint32_t>(luaL_checknumber(state, 3));
    const std::uint32_t target = static_cast<std::uint32_t>(luaL_checknumber(state, 4));
    if (!g_session.IsHomunAid(aid) && !g_session.IsMercenaryAid(aid)) {
      return 0;
    }
    CGameMode* mode = g_modeMgr.GetGameMode();
    CGameActor* targetActor = nullptr;
    if (target == g_session.m_aid) {
      targetActor = mode->scene->player;
    } else {
      targetActor = mode->scene->GetGameActorByAID(target);
      if (targetActor != nullptr) {
        const std::uint32_t flags = targetActor->GetStateFlags();
        if (IsInvisible(flags) || IsHiding(flags) || IsCloaking(flags)) {
          return 0;
        }
      }
    }
    CGameActor* actor = mode->scene->GetGameActorByAID(aid);
    if (targetActor != nullptr && actor != nullptr && UsableState(actor)) {
      mode->SendMsg(MM_USE_SKILL, skillId, target, level, 0);
    }
    mode->SendMsg(MM_SETLOCKONMOUSE, 0, 0, 0, 0);
    if (IsSupportSkill(skillId)) {
      lua_pushnumber(state, 1.0);
      return 1;
    }
    return 0;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_emblemMaxSize = static_cast<int>(host->SettingInt(kName, "EmblemMaxSize"));
  g_viewIdLimit = static_cast<int>(host->SettingInt(kName, "ViewIdLimit"));
  const bool boneLimit = host->SettingBool(kName, "No3DBoneLimit");
  const bool effectTool = host->SettingBool(kName, "EffectToolAllMaps");
  const bool stateIcons = host->SettingBool(kName, "StateIconsFromLub");
  const bool itemInfoPerServer = host->SettingBool(kName, "ItemInfoPerServer");
  g_captchaSize = static_cast<uLongf>(host->SettingInt(kName, "CaptchaBufferSize"));
  const bool homunSkills = host->SettingBool(kName, "HomunAiSkills");
  void (UICaptchaPreviewWnd::*previewAddImageData)(const void*, int) = &UICaptchaPreviewWnd::AddImageData;
  void (UIMacroDetectorWnd::*detectorAddImageData)(const void*, int) = &UIMacroDetectorWnd::AddImageData;
  int (*showErrorDialog)(const char*, UINT) = &ShowErrorDialog;
  return (g_emblemMaxSize == 0xC800 || ro::Hook(host, &CGuildMgr::Create, &GuildMgrCreateDetour, g_guildMgrCreate, kName)) &&
         (g_viewIdLimit <= kDefaultViewIds ||
          ro::Hook(host, &CSession::InitResourceNameTables, &SessionHook::InitResourceNameTablesDetour, g_initResourceNameTables, kName)) &&
         (!boneLimit ||
          (ro::Hook(host, &C3dGrannyModelRes::PlayAnimation, &GrannyHook::PlayAnimationDetour, g_playAnimation, kName) &&
           ro::Hook(host, showErrorDialog, &ShowErrorDialogDetour, g_showErrorDialog, kName))) &&
         (!effectTool ||
          ro::Hook(host, &EffectLists::LoadMapEffectScript, &EffectListsHook::LoadMapEffectScriptDetour, g_loadMapEffectScript, kName)) &&
         (!stateIcons ||
          ro::Hook(host, &CSession::GetEFSTImgFileName, &SessionHook::GetEFSTImgFileNameDetour, g_getEFSTImgFileName, kName)) &&
         (!itemInfoPerServer ||
          (ro::Hook(host, &CItemInfoMgr::Load, &ItemInfoHook::LoadDetour, g_itemInfoLoad, kName) &&
           ro::Hook(host, &CItemInfoMgr::SetItemInfo, &ItemInfoHook::SetItemInfoDetour, g_setItemInfo, kName) &&
           ro::Hook(host, &CCheckAttendanceMgr::LoadLocal, &CheckAttendanceHook::LoadLocalDetour, g_loadLocal, kName))) &&
         (g_captchaSize == 0x9AB0 ||
          (ro::Hook(host, previewAddImageData, &CaptchaPreviewHook::AddImageDataDetour, g_previewAddImageData, kName) &&
           ro::Hook(host, detectorAddImageData, &MacroDetectorHook::AddImageDataDetour, g_detectorAddImageData, kName))) &&
         (!homunSkills ||
          (ro::Hook(host, &AiSkillGround, &AiSkillGroundDetour, g_aiSkillGround, kName) &&
           ro::Hook(host, &AiSkillObject, &AiSkillObjectDetour, g_aiSkillObject, kName)));
}
