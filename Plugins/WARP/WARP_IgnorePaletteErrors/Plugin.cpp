// ShowErrorDialog(const char*, UINT), the overload CPaletteRes::Load calls when a palette file is missing: that one
// message goes to OutputDebugStringA instead of the dialog; every other text reaches the original.
#include "RoPlugin.h"

#include "Client/Application/ErrorDialog.h"

#include <cstring>
#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_IgnorePaletteErrors";
  constexpr char kPrefix[] = "CPaletteRes :: Cannot find File : ";

  using TextOverload = int (*)(const char*, UINT);

  TextOverload g_original;

  int ShowErrorDialogDetour(const char* text, UINT type) {
    if (text && std::strncmp(text, kPrefix, sizeof(kPrefix) - 1) == 0) {
      OutputDebugStringA(text);
      return 0;
    }
    return g_original(text, type);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, static_cast<TextOverload>(&ShowErrorDialog), &ShowErrorDialogDetour, g_original, kName);
}
