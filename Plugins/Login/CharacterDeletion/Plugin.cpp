// - UseEmail: CLoginMode::OnChangeState(11) sends CH_DELETE_CHAR2 with the typed text only on America, Germany and
//   Brazil, and UIEmailAddressWnd::Paint labels the prompt "birth date" on Korea. Both run with g_serviceType set to
//   ServiceAmerica when it is not one of those.
// - Countdown: inside UISlotBmpButton::Paint, Timestamp::ToSqlTimestamp returns the time left (hours may pass 24) and
//   the date line (drawn at y 0x28 through TextDrawHooks) moves out of the button, as WARP moves it.
#include "RoPlugin.h"
#include "TextDrawHooksApi.h"

#include "Client/Mode/CLoginMode.h"
#include "Contents/Configuration/ServiceConfiguration.h"
#include "Contents/Configuration/ServiceEnums.h"
#include "UI/Buttons/UISlotBmpButton.h"
#include "UI/Login/UIEmailAddressWnd.h"
#include "Utility/Timestamp.h"

#include <ctime>

namespace {
  constexpr const char* kName = "CharacterDeletion";

  bool g_inSlotPaint;

  void (CLoginMode::*g_onChangeState)(int);
  void (UIEmailAddressWnd::*g_emailPaint)();
  void (UISlotBmpButton::*g_slotPaint)();
  TIMESTAMP_STRUCT (Timestamp::*g_toSql)();

  bool UsesEmail(int type) {
    return type == ServiceAmerica || type == ServiceGermany || type == ServiceBrazil;
  }

  struct ServiceTypeScope {
    int saved = g_serviceType;
    ServiceTypeScope() {
      if (!UsesEmail(saved)) {
        g_serviceType = ServiceAmerica;
      }
    }
    ~ServiceTypeScope() { g_serviceType = saved; }
  };

  struct LoginModeHook : CLoginMode {
    void OnChangeStateDetour(int value) {
      if (value != 11) {
        (this->*g_onChangeState)(value);
        return;
      }
      ServiceTypeScope scope;
      (this->*g_onChangeState)(value);
    }
  };

  struct EmailHook : UIEmailAddressWnd {
    void PaintDetour() {
      ServiceTypeScope scope;
      (this->*g_emailPaint)();
    }
  };

  struct SlotHook : UISlotBmpButton {
    void PaintDetour() {
      g_inSlotPaint = true;
      (this->*g_slotPaint)();
      g_inSlotPaint = false;
    }
  };

  struct TimestampHook : Timestamp {
    TIMESTAMP_STRUCT ToSqlDetour() {
      if (!g_inSlotPaint) {
        return (this->*g_toSql)();
      }
      TIMESTAMP_STRUCT result{};
      const long long left = static_cast<long long>(this->seconds) - static_cast<long long>(_time32(nullptr));
      if (left > 0) {
        result.hour = static_cast<SQLUSMALLINT>(left / 3600);
        result.minute = static_cast<SQLUSMALLINT>(left / 60 % 60);
        result.second = static_cast<SQLUSMALLINT>(left % 60);
      }
      return result;
    }
  };

  void HideDateLine(UIWindow*, bool right, int&, int& y, const char*&, void*) {
    if (g_inSlotPaint && !right && y == 0x28) {
      y = -100;
    }
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool email = host->SettingBool(kName, "UseEmail");
  const bool countdown = host->SettingBool(kName, "Countdown");
  return (!email || (ro::HookVirtual(host, "?OnChangeState@CLoginMode@@UAEXH@Z", &LoginModeHook::OnChangeStateDetour, g_onChangeState, kName) &&
                     ro::HookVirtual(host, "?Paint@UIEmailAddressWnd@@UAEXXZ", &EmailHook::PaintDetour, g_emailPaint, kName))) &&
         (!countdown || (ro::HookVirtual(host, "?Paint@UISlotBmpButton@@UAEXXZ", &SlotHook::PaintDetour, g_slotPaint, kName) &&
                         ro::Hook(host, &Timestamp::ToSqlTimestamp, &TimestampHook::ToSqlDetour, g_toSql, kName) &&
                         TextDrawHooks_AddRewriter(&HideDateLine, nullptr)));
}
