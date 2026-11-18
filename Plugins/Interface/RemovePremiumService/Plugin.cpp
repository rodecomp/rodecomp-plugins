// Hooks UINewSelectCharWnd::Paint to scope UIWindow::DrawTextAt, which then skips the "Premium " draw and the "Service"
// draw that follows it, like the original patch's jump over that branch. Premium slots still count for the Billing
// slot range, so g_session.m_premiumSlotCount is left alone.
#include "RoPlugin.h"

#include "UI/Login/UINewSelectCharWnd.h"

#include <cstring>

namespace {
  constexpr const char* kName = "RemovePremiumService";

  void (UINewSelectCharWnd::*g_paint)();
  void (UIWindow::*g_drawTextAt)(int, int, const char*, int, int, int, std::uint32_t, unsigned char, unsigned char);

  bool g_inPaint;
  bool g_skipService;

  struct WindowHook : UIWindow {
    void DrawTextAtDetour(int x, int y, const char* text, int bytes, int family, int height, std::uint32_t color,
                          unsigned char bold, unsigned char italic) {
      if (g_inPaint && text != nullptr) {
        if (std::strcmp(text, "Premium ") == 0) {
          g_skipService = true;
          return;
        }
        if (g_skipService) {
          g_skipService = false;
          if (std::strcmp(text, "Service") == 0) {
            return;
          }
        }
      }
      (this->*g_drawTextAt)(x, y, text, bytes, family, height, color, bold, italic);
    }
  };

  struct SelectCharHook : UINewSelectCharWnd {
    void PaintDetour() {
      g_inPaint = true;
      g_skipService = false;
      (this->*g_paint)();
      g_inPaint = false;
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &UIWindow::DrawTextAt, &WindowHook::DrawTextAtDetour, g_drawTextAt, kName) &&
         ro::HookVirtual(host, "?Paint@UINewSelectCharWnd@@UAEXXZ", &SelectCharHook::PaintDetour, g_paint, kName);
}
