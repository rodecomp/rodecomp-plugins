// Reimplements UIMileageWnd::Paint, OnProcess and OnCreate (UIMileageWnd.cpp) with the configured cap in place of 300.
#include "RoPlugin.h"

#include "Client/Session/Session.h"
#include "Contents/Tables/MessageTable.h"
#include "Resource/Bitmap/BitmapResource.h"
#include "Resource/Bitmap/TextureNames.h"
#include "Resource/MSGSTRINGID.h"
#include "Resource/ResourceMgr.h"
#include "UI/CashShop/UIMileageWnd.h"
#include "Utility/Text/FormatString.h"

#include <string>
#include <timeapi.h>

namespace {
  constexpr const char* kName = "GoldPcPoints";

  int g_max;

  void (UIMileageWnd::*g_paint)();
  void (UIMileageWnd::*g_onProcess)();
  void (UIMileageWnd::*g_onCreate)(int, int);

  struct MileageHook : UIMileageWnd {
    void PaintDetour() {
      this->ClearSurface(0xFF00FF);
      std::string text = FormatOwnedString("유저인터페이스\\basic_interface\\mileage_bg%d.bmp", this->m_bonus);
      this->DrawBitmap(0, 0, static_cast<CBitmapRes*>(ResourceMgr::Instance().Load(Resolve(text.c_str()))), true);
      if (this->m_point >= g_max) {
        text = "-- : --";
      } else {
        text = FormatOwnedString("%02d : %02d", this->m_remaining / 60, this->m_remaining % 60);
      }
      int width = this->TextWidth(text.c_str(), 0, 0, 12, 0, 0);
      this->DrawOutlinedText((this->geometry.width - width) >> 1, 15, text.c_str(), 0, 0xFFFFFF, 0, 0, 12, 0, 0);
      text = FormatOwnedString("%d", this->m_point);
      width = this->TextWidth(text.c_str(), 0, 0, 12, 0, 0);
      this->DrawOutlinedText(this->geometry.width - width - 9, this->geometry.height - 16, text.c_str(), 0, 0xFFFFFF, 0, 0, 12, 0, 0);
    }

    void OnProcessDetour() {
      if (!this->geometry.visible || this->m_point >= g_max || this->m_remaining == 0) {
        return;
      }
      const std::uint32_t now = timeGetTime();
      const std::uint32_t elapsed = now - this->m_startTime;
      if (elapsed < 1000) {
        return;
      }
      const std::uint32_t seconds = elapsed / 1000;
      this->m_remaining -= seconds;
      if (this->m_remaining < 0) {
        this->m_remaining = 0;
      }
      this->m_startTime = seconds * 1000 - elapsed + now;
      this->Invalidate();
      g_session.m_mileageWndElapsed = 3600 - this->m_remaining;
    }

    void OnCreateDetour(int, int) {
      this->m_tooltip = FormatOwnedString(LookupMessage(MSI_MILEAGE_HELP), g_max);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_max = static_cast<int>(host->SettingInt(kName, "MaxPoints"));
  if (g_max == 300) {
    return true;
  }
  return ro::HookVirtual(host, "?Paint@UIMileageWnd@@UAEXXZ", &MileageHook::PaintDetour, g_paint, kName) &&
         ro::HookVirtual(host, "?OnProcess@UIMileageWnd@@UAEXXZ", &MileageHook::OnProcessDetour, g_onProcess, kName) &&
         ro::HookVirtual(host, "?OnCreate@UIMileageWnd@@UAEXHH@Z", &MileageHook::OnCreateDetour, g_onCreate, kName);
}
