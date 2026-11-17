// The two standalone ShowErrorDialog overloads of the exe prepare the render system for a dialog as before, then send
// the text to OutputDebugStringA instead of MessageBoxA and return 0. The other two are inlined into their callers and
// keep their message box: ShowErrorDialog(int) in C3dAttr::CellType, and ShowErrorDialog(CRenderSystemBase*, ...) in
// CFile::OpenWrite and ImportWorldGround, which nothing calls in this source.
#include "RoPlugin.h"

#include "Client/Application/ErrorDialog.h"
#include "Render/DeviceContext/DCSurface.h"
#include "Render/RenderSystemBase.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_IgnoreResourceErrors";

  using StringOverload = int (*)(const std::string&, const UINT&);
  using TextOverload = int (*)(const char*, UINT);

  StringOverload g_originalString;
  TextOverload g_originalText;
  // GetRenderSystemHolder() is inline with a local static: call the exe's copy.
  RenderSystemHolder* (*g_getRenderSystemHolder)();

  int ShowStringDetour(const std::string& text, const UINT&) {
    g_getRenderSystemHolder()->PrepareErrorDialog();
    OutputDebugStringA(text.c_str());
    return 0;
  }

  int ShowTextDetour(const char* text, UINT) {
    g_getRenderSystemHolder()->PrepareErrorDialog();
    OutputDebugStringA(text);
    return 0;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  g_getRenderSystemHolder = reinterpret_cast<RenderSystemHolder* (*)()>(host->Resolve("?GetRenderSystemHolder@@YAPAURenderSystemHolder@@XZ"));
  if (!g_getRenderSystemHolder) {
    host->Log("%s: no exe function GetRenderSystemHolder", kName);
    return false;
  }
  return ro::Hook(host, static_cast<StringOverload>(&ShowErrorDialog), &ShowStringDetour, g_originalString, kName) &&
         ro::Hook(host, static_cast<TextOverload>(&ShowErrorDialog), &ShowTextDetour, g_originalText, kName);
}
