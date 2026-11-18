// Hooks UIWindow::DrawTextAt and UIWindow::DrawRightAlignedText once and runs the rewriters dependents register; a
// rewriter that sets the text to nullptr ends the chain and the draw is skipped.
#include "RoPlugin.h"
#include "TextDrawHooksApi.h"

#include "UI/Window/UIWindow.h"

#include <cstdint>

namespace {
  constexpr const char* kName = "TextDrawHooks";
  constexpr int kMax = 16;

  struct Entry {
    TextDrawRewriter rewriter;
    void* user;
  };
  Entry g_entries[kMax];
  int g_count;

  void (UIWindow::*g_drawTextAt)(int, int, const char*, int, int, int, std::uint32_t, unsigned char, unsigned char);
  void (UIWindow::*g_drawRight)(int, int, const char*, int, int, int, std::uint32_t, unsigned char);

  // False when a rewriter skipped the draw.
  bool Rewrite(UIWindow* window, bool right, int& x, int& y, const char*& text, int& bytes) {
    const char* before = text;
    for (int i = 0; i < g_count && text != nullptr; ++i) {
      g_entries[i].rewriter(window, right, x, y, text, g_entries[i].user);
    }
    if (text != before) {
      bytes = 0;
    }
    return text != nullptr;
  }

  struct WindowHook : UIWindow {
    void DrawTextAtDetour(int x, int y, const char* text, int bytes, int family, int height, std::uint32_t color, unsigned char bold,
                          unsigned char italic) {
      if (g_count != 0 && text != nullptr && !Rewrite(this, false, x, y, text, bytes)) {
        return;
      }
      (this->*g_drawTextAt)(x, y, text, bytes, family, height, color, bold, italic);
    }
    void DrawRightAlignedTextDetour(int right, int y, const char* text, int bytes, int family, int height, std::uint32_t color, unsigned char bold) {
      if (g_count != 0 && text != nullptr && !Rewrite(this, true, right, y, text, bytes)) {
        return;
      }
      (this->*g_drawRight)(right, y, text, bytes, family, height, color, bold);
    }
  };
}

TEXTDRAWHOOKS_API bool TextDrawHooks_AddRewriter(TextDrawRewriter rewriter, void* user) {
  if (rewriter == nullptr || g_count == kMax) {
    return false;
  }
  g_entries[g_count++] = {rewriter, user};
  return true;
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &UIWindow::DrawTextAt, &WindowHook::DrawTextAtDetour, g_drawTextAt, kName) &&
         ro::Hook(host, &UIWindow::DrawRightAlignedText, &WindowHook::DrawRightAlignedTextDetour, g_drawRight, kName);
}
