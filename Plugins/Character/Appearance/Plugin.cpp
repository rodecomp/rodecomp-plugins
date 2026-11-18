// Body and hair resource names, per setting:
// - BodyPalettes / HeadPalettes / DoramHeadPalettes: EquipmentResourceMgr::GetBodyPalName and GetHeadPalName are
//   reimplemented and build WARP's shared names (the body / head names without the job or hair style, and without the
//   sex for Unisex) instead of the per job / per style ones. Default keeps the game's names.
// - Allow65kHairs: GetHeadName and GetHeadPalName clamp the style to the 46-entry human (11-entry Doram) name tables;
//   their reimplementations name a style past the table by its number (up to MaxHairStyle) and keep the game's
//   fallback (13 / 10) past that. GetAccessoryResourceName looks the style up in the human table without using the name,
//   so its detour moves an out-of-table style into the table before the original runs (no out-of-bounds read).
// - ExtraOutfits: GetBodyResourceName, GetRaceBodyResourceName and GetBodyPalName run with the outfit in scope:
//   CSession::IsSecondJob answers true for it (the costume path), CSession::GetSpriteJob is given the listed job in place
//   of the outfit id, and the costume number replaces the 1 the game puts in costume_1 and the _1 suffix. A custom
//   outfit keeps its palette number like the stock ones (GetBodyStyle only knows 4332..4349).
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Actors/Resources/EquipmentResourceMgr.h"
#include "Utility/Text/FormatString.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
  constexpr const char* kName = "Appearance";

  enum Share { kDefault, kMaleFemale, kUnisex };

  struct Outfit {
    int id;
    int job;
    int costume;
  };

  int g_body, g_head;
  bool g_doramHead, g_hairs;
  int g_maxHair;
  std::vector<Outfit> g_outfits;
  const Outfit* g_scope;  // the outfit whose body name is being built

  std::string (EquipmentResourceMgr::*g_getBodyPalName)(int, int, int, int, int, int);
  std::string (EquipmentResourceMgr::*g_getHeadPalName)(int, int, int, int);
  std::string (EquipmentResourceMgr::*g_getHeadName)(int, int, int, int*);
  std::string (EquipmentResourceMgr::*g_getAccessoryResourceName)(int, int, int, int, int);
  std::string (EquipmentResourceMgr::*g_getBodyResourceName)(int, int, int, int, int);
  std::string (EquipmentResourceMgr::*g_getRaceBodyResourceName)(int, int, int, int, int);
  bool (CSession::*g_isSecondJob)(int);
  int (CSession::*g_getSpriteJob)(int, int, bool);

  int ParseShare(const char* value) {
    const std::string text = value;
    return text == "MaleFemale" ? kMaleFemale : text == "Unisex" ? kUnisex : kDefault;
  }

  const Outfit* FindOutfit(int id) {
    for (const Outfit& outfit : g_outfits) {
      if (outfit.id == id) {
        return &outfit;
      }
    }
    return nullptr;
  }

  // Puts `outfit` in scope for the body name built inside it.
  struct OutfitScope {
    const Outfit* saved;
    explicit OutfitScope(const Outfit* outfit) : saved(g_scope) { g_scope = outfit; }
    ~OutfitScope() { g_scope = saved; }
  };

  // "...\costume_1\<job>_<sex>_1.<ext>" with the scoped outfit's costume number.
  void ApplyCostume(std::string& name) {
    if (!g_scope || g_scope->costume == 1) {
      return;
    }
    const std::string number = std::to_string(g_scope->costume);
    const std::size_t folder = name.find("\\costume_1\\");
    if (folder != std::string::npos) {
      name.replace(folder + 9, 1, number);
    }
    const std::size_t suffix = name.rfind("_1.");
    if (suffix != std::string::npos) {
      name.replace(suffix + 1, 1, number);
    }
  }

  const std::vector<const char*>& HairTable(bool doram, int sex) {
    if (doram) {
      return sex == 1 ? g_session.m_doramMaleHeadNames : g_session.m_doramFemaleHeadNames;
    }
    return sex == 1 ? g_session.m_humanMaleHeadNames : g_session.m_humanFemaleHeadNames;
  }

  // The style the game names, as the 46 / 11-entry clamps do, extended past the table by Allow65kHairs.
  int ClampHair(bool doram, int head) {
    const int size = static_cast<int>(HairTable(doram, 1).size());
    const int lowest = doram ? 1 : 0;
    if (head >= lowest && (head < size || (g_hairs && head <= g_maxHair))) {
      return head;
    }
    return doram ? 10 : 13;
  }

  // The name part of a hair resource: the table entry, or the style number past the table.
  std::string HairName(bool doram, int sex, int head) {
    if (sex != 0 && sex != 1) {
      return "";
    }
    const std::vector<const char*>& table = HairTable(doram, sex);
    if (head < static_cast<int>(table.size())) {
      return table[head];
    }
    return std::to_string(head);
  }

  struct ResourceHook : EquipmentResourceMgr {
    std::string GetBodyPalName(int job, int sex, int palette, int costumeJob, int costume, int unused) {
      const OutfitScope scope(FindOutfit(costumeJob));
      const int style = g_scope ? palette : g_session.GetBodyStyle(job, palette, costumeJob);
      const bool secondJob = g_session.IsSecondJob(costumeJob);
      int spriteJob = g_session.GetSpriteJob(job, costumeJob, false);
      if (spriteJob > 3950) {
        spriteJob -= 3950;
      }
      const int number = g_scope ? g_scope->costume : 1;
      const std::string sexName = g_session.GetSexName(sex);
      const char* const palJob = g_session.m_palJobNames[spriteJob];

      if (g_session.IsDoramJob(job)) {
        const std::string race = this->GetBodyRacePrefix(job);
        switch (g_body) {
        case kMaleFemale:
          return secondJob ? FormatOwnedString("%s\\costume_%d\\body_%s_%d.pal", race.c_str(), number, sexName.c_str(), style)
                           : FormatOwnedString("%s\\body_%s_%d.pal", race.c_str(), sexName.c_str(), style);
        case kUnisex:
          return secondJob ? FormatOwnedString("%s\\costume_%d\\body_%d.pal", race.c_str(), number, style)
                           : FormatOwnedString("%s\\body_%d.pal", race.c_str(), style);
        default:
          return secondJob ? FormatOwnedString("%s\\body\\costume_%d\\%s_%s_%d_%d.pal", race.c_str(), number, palJob, sexName.c_str(), style, number)
                           : FormatOwnedString("%s\\body\\%s_%s_%d.pal", race.c_str(), palJob, sexName.c_str(), style);
        }
      }

      std::string result;
      switch (g_body) {
      case kMaleFemale:
        result = secondJob ? FormatOwnedString("몸\\costume_%d\\body_%s_%d_%d.pal", number, sexName.c_str(), style, number)
                           : FormatOwnedString("몸\\body_%s_%d.pal", sexName.c_str(), style);
        break;
      case kUnisex:
        result = secondJob ? FormatOwnedString("몸\\costume_%d\\body_%d_%d.pal", number, style, number)
                           : FormatOwnedString("몸\\body_%d.pal", style);
        break;
      default:
        result = secondJob ? FormatOwnedString("몸\\costume_%d\\%s_%s_%d_%d.pal", number, palJob, sexName.c_str(), style, number)
                           : FormatOwnedString("몸\\%s_%s_%d.pal", palJob, sexName.c_str(), style);
        break;
      }

      if (costume == 2) {
        std::size_t found = result.rfind("마도기어_");
        if (found != std::string::npos) {
          result.replace(found, 9, "마도아머_", 9);
        }
        found = result.rfind("MEISTER_MADOGEAR1");
        if (found != std::string::npos) {
          result.replace(found, 17, "MEISTER_MADOGEAR2", 17);
        }
      }
      return result;
    }

    std::string GetHeadPalName(int head, int job, int sex, int palette) {
      const std::string sexName = g_session.GetSexName(sex);
      if (g_session.IsDoramJob(job)) {
        const std::string race = this->GetBodyRacePrefix(job);
        if (g_doramHead && g_head == kMaleFemale) {
          return FormatOwnedString("%s\\머리\\head_%s_%d.pal", race.c_str(), sexName.c_str(), palette);
        }
        if (g_doramHead && g_head == kUnisex) {
          return FormatOwnedString("%s\\머리\\head_%d.pal", race.c_str(), palette);
        }
        const std::string name = HairName(true, sex, ClampHair(true, head));
        return FormatOwnedString("%s\\머리\\머리%s_%s_%d.pal", race.c_str(), name.c_str(), sexName.c_str(), palette);
      }

      if (g_head == kMaleFemale) {
        return FormatOwnedString("머리\\head_%s_%d.pal", sexName.c_str(), palette);
      }
      if (g_head == kUnisex) {
        return FormatOwnedString("머리\\head_%d.pal", palette);
      }
      g_session.NormalizeHead(&head, job);
      const std::string name = HairName(false, sex, ClampHair(false, head));
      return FormatOwnedString("머리\\머리%s_%s_%d.pal", name.c_str(), sexName.c_str(), palette);
    }

    std::string GetHeadName(int type, int job, int sex, int* head) {
      if (job < 0 || (job > 30 && job < 1000) || (job > 1000 && job < 3999) || (job >= 20021 && job < 32000)) {
        job = 0;
      }
      if (sex != 0) {
        sex = 1;
      }
      const bool doram = g_session.IsDoramJob(job);
      *head = ClampHair(doram, *head);
      const std::string name = HairName(doram, sex, *head);
      const std::string sexName = g_session.GetSexName(sex);
      return this->GetBodyRacePrefix(job) +
             FormatOwnedString("\\머리통\\%s\\%s_%s.%s", sexName.c_str(), name.c_str(), sexName.c_str(), g_equipmentResourceExtensions[type]);
    }

    std::string GetAccessoryResourceName(int type, int job, int sex, int head, int accessory) {
      if (head < 0 || head >= static_cast<int>(g_session.m_humanMaleHeadNames.size())) {
        head = 13;  // the style only selects a name the result does not use
      }
      return (this->*g_getAccessoryResourceName)(type, job, sex, head, accessory);
    }

    std::string GetBodyResourceName(int type, int job, int sex, int costumeJob, int costume) {
      const OutfitScope scope(FindOutfit(costumeJob));
      std::string result = (this->*g_getBodyResourceName)(type, job, sex, costumeJob, costume);
      ApplyCostume(result);
      return result;
    }

    std::string GetRaceBodyResourceName(int type, int raceJob, int job, int sex, int costumeJob) {
      const OutfitScope scope(FindOutfit(costumeJob));
      std::string result = (this->*g_getRaceBodyResourceName)(type, raceJob, job, sex, costumeJob);
      ApplyCostume(result);
      return result;
    }
  };

  struct SessionHook : CSession {
    bool IsSecondJob(int job) {
      return (g_scope && job == g_scope->id) || (this->*g_isSecondJob)(job);
    }

    int GetSpriteJob(int job, int costumeJob, bool spriteIndex) {
      if (g_scope && costumeJob == g_scope->id) {
        costumeJob = g_scope->job;
      }
      return (this->*g_getSpriteJob)(job, costumeJob, spriteIndex);
    }
  };

  bool ReadOutfits(const RoHostApi* host) {
    const int count = host->SettingCount(kName, "ExtraOutfits");
    for (int index = 0; index < count; ++index) {
      const char* text = host->SettingItem(kName, "ExtraOutfits", index);
      Outfit outfit{};
      char extra = 0;
      if (sscanf_s(text, " %d = %d , %d %c", &outfit.id, &outfit.job, &outfit.costume, &extra, 1u) != 3 || outfit.id < 1 ||
          outfit.job < 0 || outfit.costume < 1 || outfit.costume > 99 || FindOutfit(outfit.id)) {
        host->Log("%s: ExtraOutfits entry \"%s\" is not 'outfit id = job id, costume' (costume 1-99, ids unique)", kName, text);
        return false;
      }
      g_outfits.push_back(outfit);
    }
    return true;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_body = ParseShare(host->SettingString(kName, "BodyPalettes"));
  g_head = ParseShare(host->SettingString(kName, "HeadPalettes"));
  g_doramHead = host->SettingBool(kName, "DoramHeadPalettes");
  g_hairs = host->SettingBool(kName, "Allow65kHairs");
  g_maxHair = static_cast<int>(host->SettingInt(kName, "MaxHairStyle"));
  if (!ReadOutfits(host)) {
    return false;
  }

  bool ok = true;
  if (g_body != kDefault || !g_outfits.empty()) {
    ok = ok && ro::Hook(host, &EquipmentResourceMgr::GetBodyPalName, &ResourceHook::GetBodyPalName, g_getBodyPalName, kName);
  }
  if (g_head != kDefault || g_hairs) {
    ok = ok && ro::Hook(host, &EquipmentResourceMgr::GetHeadPalName, &ResourceHook::GetHeadPalName, g_getHeadPalName, kName);
  }
  if (g_hairs) {
    ok = ok && ro::Hook(host, &EquipmentResourceMgr::GetHeadName, &ResourceHook::GetHeadName, g_getHeadName, kName) &&
         ro::Hook(host, &EquipmentResourceMgr::GetAccessoryResourceName, &ResourceHook::GetAccessoryResourceName, g_getAccessoryResourceName, kName);
  }
  if (!g_outfits.empty()) {
    ok = ok && ro::Hook(host, &EquipmentResourceMgr::GetBodyResourceName, &ResourceHook::GetBodyResourceName, g_getBodyResourceName, kName) &&
         ro::Hook(host, &EquipmentResourceMgr::GetRaceBodyResourceName, &ResourceHook::GetRaceBodyResourceName, g_getRaceBodyResourceName, kName) &&
         ro::Hook(host, &CSession::IsSecondJob, &SessionHook::IsSecondJob, g_isSecondJob, kName) &&
         ro::Hook(host, &CSession::GetSpriteJob, &SessionHook::GetSpriteJob, g_getSpriteJob, kName);
  }
  return ok;
}
