// TextDrawHooks' API for dependent plugins (dependencies: TextDrawHooks = "^1.1.0" for the skip). Rewriters run for every
// UIWindow::DrawTextAt and UIWindow::DrawRightAlignedText call, in registration order; a dependent normally scopes them
// by hooking the Paint it cares about and checking a flag. A rewriter may replace `text` with a pointer it owns (valid
// until the draw returns) and move `x`/`y`; `right` is true for DrawRightAlignedText, where `x` is the right edge.
// Setting `text = nullptr` skips the draw: the later rewriters do not run and the original is not called. `text` is
// never null when a rewriter is called.
#ifndef RAGEXE_PLUGIN_TEXTDRAWHOOKS_TEXTDRAWHOOKSAPI_H
#define RAGEXE_PLUGIN_TEXTDRAWHOOKS_TEXTDRAWHOOKSAPI_H

#ifdef TEXTDRAWHOOKS_EXPORTS
#define TEXTDRAWHOOKS_API __declspec(dllexport)
#else
#define TEXTDRAWHOOKS_API __declspec(dllimport)
#endif

class UIWindow;

using TextDrawRewriter = void (*)(UIWindow* window, bool right, int& x, int& y, const char*& text, void* user);

// Returns false for a null rewriter or a full table (16 entries).
TEXTDRAWHOOKS_API bool TextDrawHooks_AddRewriter(TextDrawRewriter rewriter, void* user);

#endif
