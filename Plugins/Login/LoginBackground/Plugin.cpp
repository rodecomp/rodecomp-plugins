// CLoginMode::Update and OnUpdateEx call UIWindowMgr::DrawLoginOverlay (then DrawCenteredBackground only in state 0)
// on the official service types, and only DrawCenteredBackground elsewhere. On the other service types, a
// DrawCenteredBackground inside those two functions becomes DrawLoginOverlay, plus the background in state 0.
#include "RoPlugin.h"

#include "Client/Mode/CLoginMode.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "UI/Window/UIWindowMgr.h"

namespace {
  constexpr const char* kName = "LoginBackground";

  CLoginMode* g_mode;

  void (CLoginMode::*g_update)();
  void (CLoginMode::*g_updateEx)(int);
  void (UIWindowMgr::*g_drawCentered)();

  bool Official() {
    switch (g_serviceType) {
    case ServiceKorea: case ServiceTaiwan: case ServiceMalaysia: case ServiceSingapore: case ServiceChina: case ServiceGermany:
    case ServiceAmerica: case ServiceIndia: case ServiceBrazil: case ServiceThai: case ServicePhilippine: case ServiceRussia:
    case ServiceEu: case ServiceVietnam:
      return true;
    default:
      return false;
    }
  }

  struct LoginModeHook : CLoginMode {
    void UpdateDetour() {
      g_mode = this;
      (this->*g_update)();
      g_mode = nullptr;
    }
    void UpdateExDetour(int value) {
      g_mode = this;
      (this->*g_updateEx)(value);
      g_mode = nullptr;
    }
  };

  struct WindowMgrHook : UIWindowMgr {
    void DrawCenteredDetour() {
      if (g_mode == nullptr || Official()) {
        (this->*g_drawCentered)();
        return;
      }
      CLoginMode* mode = g_mode;
      g_mode = nullptr;
      this->DrawLoginOverlay();
      if (mode->cursorType == 0) {
        (this->*g_drawCentered)();
      }
      g_mode = mode;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::HookVirtual(host, "?Update@CLoginMode@@UAEXXZ", &LoginModeHook::UpdateDetour, g_update, kName) &&
         ro::HookVirtual(host, "?OnUpdateEx@CLoginMode@@UAEXH@Z", &LoginModeHook::UpdateExDetour, g_updateEx, kName) &&
         ro::Hook(host, &UIWindowMgr::DrawCenteredBackground, &WindowMgrHook::DrawCenteredDetour, g_drawCentered, kName);
}
