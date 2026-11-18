// After UISeekPartyListWnd::InitJobFilter, the m_jobOptions entries built from message 0x65D + k (categories 2..8, in
// order) whose ID is listed become empty strings. They are blanked rather than removed: the window turns the combo
// index into the job it sends (jobOffsets[category] - 1 + index), so removing an entry would shift every later job.
#include "RoPlugin.h"

#include "UI/Party/UISeekPartyListWnd.h"

#include <vector>

namespace {
  constexpr const char* kName = "BookingJobFilter";
  constexpr int kFirstMessage = 0x65D;

  std::vector<int> g_ids;

  void (UISeekPartyListWnd::*g_initJobFilter)();

  struct SeekPartyHook : UISeekPartyListWnd {
    void InitJobFilterDetour() {
      (this->*g_initJobFilter)();
      int message = kFirstMessage;
      for (int category = 2; category < 9; ++category) {
        for (std::string& option : this->m_jobOptions[category]) {
          for (const int id : g_ids) {
            if (id == message) {
              option.clear();
            }
          }
          ++message;
        }
      }
      this->SelectJobCategory(this->m_jobCategory);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const int count = host->SettingCount(kName, "JobMessageIds");
  for (int i = 0; i < count; ++i) {
    g_ids.push_back(static_cast<int>(host->SettingItemInt(kName, "JobMessageIds", i)));
  }
  if (g_ids.empty()) {
    return true;
  }
  return ro::Hook(host, &UISeekPartyListWnd::InitJobFilter, &SeekPartyHook::InitJobFilterDetour, g_initJobFilter, kName);
}
