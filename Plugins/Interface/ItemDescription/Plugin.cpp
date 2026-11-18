// UIItemCollectionWnd::SendMsg(UM_ITEMEXPLANATION) creates the preview button (m_withdrawButton, id 354) and
// re-enables m_helpButton; OnCreate creates m_helpButton. After either, the preview button is removed and the help
// button made invisible (kept as a child: SendMsg dereferences it).
#include "RoPlugin.h"

#include "UI/Buttons/UIBasicButton.h"
#include "UI/Buttons/UIBitmapButton.h"
#include "UI/Items/UIItemCollectionWnd.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "ItemDescription";

  bool g_noPreview;
  bool g_hideHelp;

  void (UIItemCollectionWnd::*g_onCreate)(int, int);
  int (UIItemCollectionWnd::*g_sendMsg)(UIWindow*, int, std::intptr_t, std::intptr_t, std::intptr_t, std::intptr_t);

  struct CollectionHook : UIItemCollectionWnd {
    void Strip() {
      if (g_noPreview && this->m_withdrawButton != nullptr) {
        this->RemoveChild(this->m_withdrawButton, 1);
        this->m_withdrawButton = nullptr;
      }
      if (g_hideHelp && this->m_helpButton != nullptr) {
        this->m_helpButton->geometry.visible = 0;
      }
    }
    void OnCreateDetour(int width, int height) {
      (this->*g_onCreate)(width, height);
      this->Strip();
    }
    int SendMsgDetour(UIWindow* sender, int message, std::intptr_t a, std::intptr_t b, std::intptr_t c, std::intptr_t d) {
      const int result = (this->*g_sendMsg)(sender, message, a, b, c, d);
      this->Strip();
      return result;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_noPreview = host->SettingBool(kName, "NoEquipPreview");
  g_hideHelp = host->SettingBool(kName, "HideProbability");
  if (!g_noPreview && !g_hideHelp) {
    host->Log("%s: every setting is off, nothing to do", kName);
    return true;
  }
  return ro::HookVirtual(host, "?OnCreate@UIItemCollectionWnd@@UAEXHH@Z", &CollectionHook::OnCreateDetour, g_onCreate, kName) &&
         ro::HookVirtual(host, "?SendMsg@UIItemCollectionWnd@@UAEHPAVUIWindow@@HHHHH@Z", &CollectionHook::SendMsgDetour, g_sendMsg, kName);
}
