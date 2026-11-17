// Quest "not found" popups go to OutputDebugStringA. Four sites (CUIOngoingQuestInfo, CUIRecommendedQuestInfo,
// UIRenewQuestUI and UIRenewQuestWnd::SendMsg) call UIWindowMgr::Confirm, shared with every other confirm box, so its
// detour filters by text. UIRenewDetailQuestInfoWnd::SetQuest calls MessageBoxA directly and is reimplemented.
#include "RoPlugin.h"

#include "Contents/Quest/RenewQuestMgr.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/MSGSTRINGID.h"
#include "UI/Quest/UIRenewQuestWnd.h"
#include "UI/Window/UIWindowMgr.h"
#include "Utility/Text/FormatString.h"

#include <cstring>
#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_IgnoreQuestErrors";
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
          const std::string timeText = FormatOwnedString("%s [%02d/%02d/%02d %02d:%02d]", LookupMessage(MSI_NAVIGATION_ROUTEINFO), quest.m_year,
                                                         quest.m_month, quest.m_day, quest.m_hour, quest.m_minute);
          this->m_timeText = timeText;
        }
      }

      this->Invalidate();
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &UIWindowMgr::Confirm, &ConfirmHook::Confirm, g_originalConfirm, kName) &&
         ro::Hook(host, &UIRenewDetailQuestInfoWnd::SetQuest, &DetailQuestHook::SetQuest, g_originalSetQuest, kName);
}
