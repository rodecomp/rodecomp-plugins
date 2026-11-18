// The exe calls MessageBoxA directly from 160+ sites, so like WARP (which redirects every import reference) this hooks
// user32!MessageBoxA through HookSystem and acts only on calls whose return address is in the exe; other modules'
// boxes pass through.
// - Beep: MessageBeep(Sound) and IDOK. None: IDOK. Icon: the MB_ICONMASK bits of the type are replaced by Icon.
#include "RoPlugin.h"

#include <windows.h>

#include <cstring>
#include <intrin.h>

namespace {
  constexpr const char* kName = "MessageBoxes";

  enum class Mode { Default, Beep, None, Icon };

  struct Named {
    const char* name;
    UINT value;
  };
  constexpr Named kTypes[] = {
    {"OK_DEFAULT", MB_OK},           {"ERROR_STOP", MB_ICONHAND},       {"QUESTION", MB_ICONQUESTION},
    {"WARN_EXCLAMATION", MB_ICONEXCLAMATION}, {"INFO_ASTERISK", MB_ICONASTERISK}, {"SIMPLE_BEEP", 0xFFFFFFFF},
  };

  Mode g_mode;
  UINT g_value;
  decltype(&::MessageBoxA) g_original;

  UINT TypeValue(const char* name) {
    for (const Named& type : kTypes) {
      if (std::strcmp(name, type.name) == 0) {
        return type.value;
      }
    }
    return MB_OK;
  }

  bool CalledByExe(const void* returnAddress) {
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<const char*>(returnAddress), &module) &&
           module == GetModuleHandleA(nullptr);
  }

  int WINAPI MessageBoxDetour(HWND window, LPCSTR text, LPCSTR caption, UINT type) {
    if (!CalledByExe(_ReturnAddress())) {
      return g_original(window, text, caption, type);
    }
    switch (g_mode) {
    case Mode::Beep:
      MessageBeep(g_value);
      return IDOK;
    case Mode::None:
      return IDOK;
    case Mode::Icon:
      return g_original(window, text, caption, (type & ~MB_ICONMASK) | g_value);
    default:
      return g_original(window, text, caption, type);
    }
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const char* mode = host->SettingString(kName, "Mode");
  if (std::strcmp(mode, "Beep") == 0) {
    g_mode = Mode::Beep;
    g_value = TypeValue(host->SettingString(kName, "Sound"));
  } else if (std::strcmp(mode, "None") == 0) {
    g_mode = Mode::None;
  } else if (std::strcmp(mode, "Icon") == 0) {
    g_mode = Mode::Icon;
    g_value = TypeValue(host->SettingString(kName, "Icon"));
  } else {
    host->Log("%s: Mode is Default, nothing to do", kName);
    return true;
  }
  return ro::HookSystem(host, "user32.dll", "MessageBoxA", &MessageBoxDetour, g_original, kName);
}
