// Hooks UINewSelectCharWnd::Paint to scope a TextDrawHooks rewriter, which then skips the "Premium " DrawTextAt and the
// "Service" DrawTextAt that follows it, like the original patch's jump over that branch. Premium slots still count for the Billing
// slot range, so g_session.m_premiumSlotCount is left alone.
#include "RoPlugin.h"
#include "TextDrawHooksApi.h"

#include "UI/Login/UINewSelectCharWnd.h"

#include <cstring>

namespace {
  constexpr const char* kName = "RemovePremiumService";

  void (UINewSelectCharWnd::*g_paint)();

  bool g_inPaint;
  bool g_skipService;

  void Rewrite(UIWindow*, bool right, int&, int&, const char*& text, void*) {
    if (!g_inPaint || right) {
      return;
    }
    if (std::strcmp(text, "Premium ") == 0) {
      g_skipService = true;
      text = nullptr;
      return;
    }
    if (g_skipService) {
      g_skipService = false;
      if (std::strcmp(text, "Service") == 0) {
        text = nullptr;
      }
    }
  }

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
  return TextDrawHooks_AddRewriter(&Rewrite, nullptr) &&
         ro::HookVirtual(host, "?Paint@UINewSelectCharWnd@@UAEXXZ", &SelectCharHook::PaintDetour, g_paint, kName);
}
