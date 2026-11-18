// - Colours: reimplements the virtual DrawTransBox of UINewChatWnd, UISubChatHisWnd and UIWhisperWnd (ScreenLine.cpp,
//   WindowOverlay.cpp) with the configured ARGB in place of 0x66000000. Inactive main tabs keep 0x99000000 and the
//   whisper title strip keeps 0x449EB6E4. With every colour at its default the hooks are not installed.
// - KeepFocus: CGameMode::ProcessInput calls g_windowMgr.SetFocus(nullptr) when a world click is not handled; inside
//   ProcessInput a SetFocus(nullptr) is dropped.
#include "RoPlugin.h"

#include "Client/Mode/CGameMode.h"
#include "Render/Primitives/AlphaRectangle.h"
#include "UI/Chat/UINewChatWnd.h"
#include "UI/Chat/UISubChatHisWnd.h"
#include "UI/Chat/UIWhisperWnd.h"
#include "UI/Lists/UITabControl.h"
#include "UI/Window/UIWindowMgr.h"

#include <cstdint>
#include <cstdlib>

// UISubChatHisWnd's layout fields are private: reached through ro::PrivateField (RoPlugin.h) rather than offsets.
struct SubChatState : ro::PrivateField<SubChatState, SubChatHistoryState UISubChatHisWnd::*> {};
template struct ro::GrantPrivate<SubChatState, &UISubChatHisWnd::m_state>;
struct SubChatInputHeight : ro::PrivateField<SubChatInputHeight, int UISubChatHisWnd::*> {};
template struct ro::GrantPrivate<SubChatInputHeight, &UISubChatHisWnd::m_inputHeight>;
struct SubChatInputWidth : ro::PrivateField<SubChatInputWidth, int UISubChatHisWnd::*> {};
template struct ro::GrantPrivate<SubChatInputWidth, &UISubChatHisWnd::m_inputWidth>;

namespace {
  constexpr const char* kName = "ChatWindowStyle";
  constexpr std::uint32_t kDefault = 0x66000000;

  std::uint32_t g_main;
  std::uint32_t g_detached;
  std::uint32_t g_whisper;
  bool g_inProcessInput;

  void (UINewChatWnd::*g_mainDraw)();
  void (UISubChatHisWnd::*g_detachedDraw)();
  void (UIWhisperWnd::*g_whisperDraw)();
  void (CGameMode::*g_processInput)();
  void (UIWindowMgr::*g_setFocus)(UIWindow*);

  std::uint32_t Color(const RoHostApi* host, const char* colorKey, const char* alphaKey) {
    const std::uint32_t rgb = static_cast<std::uint32_t>(std::strtoul(host->SettingString(kName, colorKey), nullptr, 16)) & 0xFFFFFF;
    const std::uint32_t alpha = static_cast<std::uint32_t>(host->SettingInt(kName, alphaKey)) & 0xFF;
    return (alpha << 24) | rgb;
  }
}

struct ChatWindowStyleMainHook : UINewChatWnd {
  void DrawTransBoxDetour() {
    int x = 3;
    int y = 0;
    this->ToScreen(x, y);
    if (this->geometry.state == 0) {
      return;
    }
    for (int i = 0; i < static_cast<int>(this->m_tabControl->labels.size()); ++i) {
      const int inactive = i != this->m_currentTab;
      DrawAlphaRectangle(x + 1 + this->m_tabWidth * i, y + 1 + inactive, this->m_tabWidth - 1, this->m_inputOffsetY - inactive,
                         i == this->m_currentTab ? g_main : 0x99000000);
    }
    DrawAlphaRectangle(this->m_inputX + 1, this->m_inputY + 1, this->m_listWidth - 2, this->m_listHeight - 1,
                       this->m_listColor == kDefault ? g_main : this->m_listColor);
  }

};

namespace {
  struct DetachedHook : UISubChatHisWnd {
    void DrawTransBoxDetour() {
      const SubChatHistoryState& state = ro::Field<SubChatState>(*this);
      const int inputHeight = ro::Field<SubChatInputHeight>(*this);
      const int inputWidth = ro::Field<SubChatInputWidth>(*this);
      int x = 0;
      int y = 0;
      this->ToScreen(x, y);
      DrawAlphaRectangle(x + 1, y + 1, inputWidth - 2, inputHeight, g_detached);
      const auto background = static_cast<std::uint32_t>(state.backgroundColor);
      DrawAlphaRectangle(state.x + 1, state.y + 1, state.width - 2, state.height - 2, background == kDefault ? g_detached : background);
    }
  };

  struct WhisperHook : UIWhisperWnd {
    void DrawTransBoxDetour() {
      int x = 0;
      int y = 0;
      this->ToScreen(x, y);
      DrawAlphaRectangle(x + 1, y + 1, this->geometry.width - 2, this->geometry.height, g_whisper);
      DrawAlphaRectangle(x + 1, y + 1, this->geometry.width - 2, 17, 0x449EB6E4);
    }
  };

  struct GameModeHook : CGameMode {
    void ProcessInputDetour() {
      g_inProcessInput = true;
      (this->*g_processInput)();
      g_inProcessInput = false;
    }
  };

  struct WindowMgrHook : UIWindowMgr {
    void SetFocusDetour(UIWindow* window) {
      if (window == nullptr && g_inProcessInput) {
        return;
      }
      (this->*g_setFocus)(window);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_main = Color(host, "MainColor", "MainAlpha");
  g_detached = Color(host, "DetachedColor", "DetachedAlpha");
  g_whisper = Color(host, "WhisperColor", "WhisperAlpha");
  const bool keepFocus = host->SettingBool(kName, "KeepFocus");
  return (g_main == kDefault ||
          ro::HookVirtual(host, "?DrawTransBox@UINewChatWnd@@UAEXXZ", &ChatWindowStyleMainHook::DrawTransBoxDetour, g_mainDraw, kName)) &&
         (g_detached == kDefault ||
          ro::HookVirtual(host, "?DrawTransBox@UISubChatHisWnd@@UAEXXZ", &DetachedHook::DrawTransBoxDetour, g_detachedDraw, kName)) &&
         (g_whisper == kDefault ||
          ro::HookVirtual(host, "?DrawTransBox@UIWhisperWnd@@UAEXXZ", &WhisperHook::DrawTransBoxDetour, g_whisperDraw, kName)) &&
         (!keepFocus || (ro::Hook(host, &CGameMode::ProcessInput, &GameModeHook::ProcessInputDetour, g_processInput, kName) &&
                         ro::Hook(host, &UIWindowMgr::SetFocus, &WindowMgrHook::SetFocusDetour, g_setFocus, kName)));
}
