// Hooks per setting:
// - Limits: CSession::GetMaxBaseLevel and GetMaxJobLevel, whose only callers are UIBasicInfoWnd's bar layout and level
//   text, return the first matching entry's level for the job and the original otherwise.
// - ShowNumbers: after UIBasicInfoWnd::Paint, in the expanded non-4th-job layout, "current / required" is drawn on each
//   experience bar line (where WARP's code adds its TextOutA), skipped when both values are 0.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "UI/Status/UIBasicInfoWnd.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
  constexpr const char* kName = "ExpBars";

  struct Limit {
    int firstJob;
    int lastJob;
    int maxBase;
    int maxJob;
  };

  std::vector<Limit> g_limits;
  std::uint32_t g_color;

  int (CSession::*g_getMaxBaseLevel)(int) const;
  int (CSession::*g_getMaxJobLevel)(int) const;
  void (UIBasicInfoWnd::*g_paint)();

  const Limit* Find(int job) {
    for (const Limit& limit : g_limits) {
      if (job >= limit.firstJob && job <= limit.lastJob) {
        return &limit;
      }
    }
    return nullptr;
  }

  struct SessionHook : CSession {
    int GetMaxBaseLevelDetour(int job) const {
      const Limit* limit = Find(job);
      return limit ? limit->maxBase : (this->*g_getMaxBaseLevel)(job);
    }

    int GetMaxJobLevelDetour(int job) const {
      const Limit* limit = Find(job);
      return limit ? limit->maxJob : (this->*g_getMaxJobLevel)(job);
    }
  };

  struct BasicInfoHook : UIBasicInfoWnd {
    void DrawExp(int y, std::int64_t current, std::int64_t required) {
      if (current == 0 && required == 0) {
        return;
      }
      char text[64];
      std::sprintf(text, "%lld / %lld", static_cast<long long>(current), static_cast<long long>(required));
      this->DrawTextAt(86, y, text, 0, 1, 13, g_color, 0, 0);
    }

    void PaintDetour() {
      (this->*g_paint)();
      if (this->geometry.height != this->expandedHeight || g_session.IsTreeGroupThree(g_session.GetDisplayJob())) {
        return;
      }
      this->DrawExp(this->layoutOffset + 71, g_session.m_exp, g_session.m_nextExp);
      this->DrawExp(this->layoutOffset + 83, g_session.m_jobExp, g_session.m_nextJobExp);
    }
  };

  bool ParseLimit(const char* entry, Limit& limit) {
    char* end;
    limit.firstJob = static_cast<int>(std::strtol(entry, &end, 10));
    limit.lastJob = limit.firstJob;
    if (*end == '-') {
      limit.lastJob = static_cast<int>(std::strtol(end + 1, &end, 10));
    }
    if (*end != '=') {
      return false;
    }
    limit.maxBase = static_cast<int>(std::strtol(end + 1, &end, 10));
    if (*end != ',') {
      return false;
    }
    limit.maxJob = static_cast<int>(std::strtol(end + 1, &end, 10));
    return *end == '\0' && limit.firstJob <= limit.lastJob;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  for (int index = 0, count = host->SettingCount(kName, "Limits"); index < count; ++index) {
    const char* entry = host->SettingItem(kName, "Limits", index);
    Limit limit;
    if (ParseLimit(entry, limit)) {
      g_limits.push_back(limit);
    } else {
      host->Log("%s: Limits: cannot read %s (expected firstJob-lastJob=maxBase,maxJob), ignored", kName, entry);
    }
  }
  const bool numbers = host->SettingBool(kName, "ShowNumbers");
  g_color = static_cast<std::uint32_t>(host->SettingInt(kName, "NumberColor"));
  if (g_limits.empty() && !numbers) {
    host->Log("%s: Limits is empty and ShowNumbers is off, nothing to do", kName);
    return true;
  }
  return (g_limits.empty() ||
          (ro::Hook(host, &CSession::GetMaxBaseLevel, &SessionHook::GetMaxBaseLevelDetour, g_getMaxBaseLevel, kName) &&
           ro::Hook(host, &CSession::GetMaxJobLevel, &SessionHook::GetMaxJobLevelDetour, g_getMaxJobLevel, kName))) &&
         (!numbers || ro::HookVirtual(host, "?Paint@UIBasicInfoWnd@@UAEXXZ", &BasicInfoHook::PaintDetour, g_paint, kName));
}
