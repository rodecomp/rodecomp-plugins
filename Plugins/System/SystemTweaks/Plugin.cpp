// The registry calls (RegOpenKeyExA/RegCreateKeyExA with HKEY_LOCAL_MACHINE, ~40 sites across the exe, many inlined)
// and the IsDebuggerPresent call (inside CCheckGDISchedule::Process, between checks that must keep running) have no
// common exe function to hook, so, like AllowMultipleInstances, this plugin hooks the system functions and filters on
// callers inside the exe; every other module passes through untouched:
// - HKLMtoHKCU: advapi32!RegOpenKeyExA and advapi32!RegCreateKeyExA get HKEY_CURRENT_USER for HKEY_LOCAL_MACHINE,
//   as WARP rewrote every PUSH HKEY_LOCAL_MACHINE.
// - IgnoreDebugger: kernel32!IsDebuggerPresent returns FALSE, as WARP's replacement import did.
#include "RoPlugin.h"

#include <windows.h>

#include <intrin.h>

namespace {
  constexpr const char* kName = "SystemTweaks";

  decltype(&::RegOpenKeyExA) g_regOpenKeyEx;
  decltype(&::RegCreateKeyExA) g_regCreateKeyEx;
  decltype(&::IsDebuggerPresent) g_isDebuggerPresent;

  bool CalledByExe(const void* returnAddress) {
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<const char*>(returnAddress), &module) &&
           module == GetModuleHandleA(nullptr);
  }

  HKEY UserRoot(HKEY root, const void* caller) {
    return root == HKEY_LOCAL_MACHINE && CalledByExe(caller) ? HKEY_CURRENT_USER : root;
  }

  LSTATUS WINAPI RegOpenKeyExDetour(HKEY root, LPCSTR subKey, DWORD options, REGSAM access, PHKEY result) {
    return g_regOpenKeyEx(UserRoot(root, _ReturnAddress()), subKey, options, access, result);
  }

  LSTATUS WINAPI RegCreateKeyExDetour(HKEY root, LPCSTR subKey, DWORD reserved, LPSTR className, DWORD options,
                                      REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition) {
    return g_regCreateKeyEx(UserRoot(root, _ReturnAddress()), subKey, reserved, className, options, access, security, result, disposition);
  }

  BOOL WINAPI IsDebuggerPresentDetour() {
    return CalledByExe(_ReturnAddress()) ? FALSE : g_isDebuggerPresent();
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool hklmToHkcu = host->SettingBool(kName, "HKLMtoHKCU");
  const bool ignoreDebugger = host->SettingBool(kName, "IgnoreDebugger");
  if (!hklmToHkcu && !ignoreDebugger) {
    host->Log("%s: HKLMtoHKCU and IgnoreDebugger are off, nothing to do", kName);
    return true;
  }
  return (!hklmToHkcu ||
          (ro::HookSystem(host, "advapi32.dll", "RegOpenKeyExA", &RegOpenKeyExDetour, g_regOpenKeyEx, kName) &&
           ro::HookSystem(host, "advapi32.dll", "RegCreateKeyExA", &RegCreateKeyExDetour, g_regCreateKeyEx, kName))) &&
         (!ignoreDebugger || ro::HookSystem(host, "kernel32.dll", "IsDebuggerPresent", &IsDebuggerPresentDetour, g_isDebuggerPresent, kName));
}
