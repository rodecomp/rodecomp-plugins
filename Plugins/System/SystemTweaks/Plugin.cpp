// The registry calls (RegOpenKeyExA/RegCreateKeyExA with HKEY_LOCAL_MACHINE, ~40 sites across the exe, many inlined)
// and the IsDebuggerPresent call (inside CCheckGDISchedule::Process, between checks that must keep running) have no
// common exe function to hook, so, like AllowMultipleInstances, this plugin hooks the system functions and filters on
// callers inside the exe; every other module passes through untouched:
// - HKLMtoHKCU: advapi32!RegOpenKeyExA and advapi32!RegCreateKeyExA get HKEY_CURRENT_USER for HKEY_LOCAL_MACHINE.
//   A key HKCU lacks is first copied from HKLM, where Setup.exe (a separate elevated process) writes its settings;
//   WinMain starts Setup.exe until ReadSetup finds Software\Gravity Soft\RenewSetup.
// - IgnoreDebugger: kernel32!IsDebuggerPresent returns FALSE, as WARP's replacement import did.
// - No1and1Arg: WinMain requires a "1rag1" (or 1sak1, 1gld1, 2rag2, 1eve1, 1pk1) command-line argument and otherwise
//   shows g_runPatcherMessage (an empty "Error" box) and quits. The check is inline in WinMain and its first test is
//   strstr(commandLine, "1rag1") with the decrypted name; "1rag1" is searched nowhere else. vcruntime140!strstr
//   called by the exe with that needle answers as if the argument were there, so the whole chain is skipped, as
//   WARP's jump over it did.
#include "RoPlugin.h"

#include <windows.h>

#include <cstring>

#include <intrin.h>

namespace {
  constexpr const char* kName = "SystemTweaks";

  decltype(&::RegOpenKeyExA) g_regOpenKeyEx;
  decltype(&::RegCreateKeyExA) g_regCreateKeyEx;
  decltype(&::IsDebuggerPresent) g_isDebuggerPresent;
  using Strstr = const char* (__cdecl*)(const char*, const char*);
  Strstr g_strstr;

  bool CalledByExe(const void* returnAddress) {
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<const char*>(returnAddress), &module) &&
           module == GetModuleHandleA(nullptr);
  }

  /// HKLM\subKey into HKCU\subKey when HKCU has no such key yet and HKLM does. The original advapi32 functions are
  /// used, so this doesn't go through the detours.
  void SeedFromMachine(LPCSTR subKey) {
    if (subKey == nullptr || subKey[0] == '\0') {
      return;
    }
    HKEY user = nullptr;
    if (g_regOpenKeyEx(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &user) == ERROR_SUCCESS) {
      RegCloseKey(user);
      return;
    }
    HKEY machine = nullptr;
    if (g_regOpenKeyEx(HKEY_LOCAL_MACHINE, subKey, 0, KEY_READ, &machine) != ERROR_SUCCESS) {
      return;
    }
    if (g_regCreateKeyEx(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &user, nullptr) == ERROR_SUCCESS) {
      RegCopyTreeA(machine, nullptr, user);
      RegCloseKey(user);
    }
    RegCloseKey(machine);
  }

  HKEY UserRoot(HKEY root, LPCSTR subKey, const void* caller) {
    if (root != HKEY_LOCAL_MACHINE || !CalledByExe(caller)) {
      return root;
    }
    SeedFromMachine(subKey);
    return HKEY_CURRENT_USER;
  }

  LSTATUS WINAPI RegOpenKeyExDetour(HKEY root, LPCSTR subKey, DWORD options, REGSAM access, PHKEY result) {
    return g_regOpenKeyEx(UserRoot(root, subKey, _ReturnAddress()), subKey, options, access, result);
  }

  LSTATUS WINAPI RegCreateKeyExDetour(HKEY root, LPCSTR subKey, DWORD reserved, LPSTR className, DWORD options,
                                      REGSAM access, const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition) {
    return g_regCreateKeyEx(UserRoot(root, subKey, _ReturnAddress()), subKey, reserved, className, options, access, security, result, disposition);
  }

  BOOL WINAPI IsDebuggerPresentDetour() {
    return CalledByExe(_ReturnAddress()) ? FALSE : g_isDebuggerPresent();
  }

  const char* __cdecl StrstrDetour(const char* text, const char* needle) {
    const char* found = g_strstr(text, needle);
    if (found == nullptr && needle != nullptr && std::strcmp(needle, "1rag1") == 0 && CalledByExe(_ReturnAddress())) {
      return text;
    }
    return found;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const bool hklmToHkcu = host->SettingBool(kName, "HKLMtoHKCU");
  const bool ignoreDebugger = host->SettingBool(kName, "IgnoreDebugger");
  const bool no1and1Arg = host->SettingBool(kName, "No1and1Arg");
  if (!hklmToHkcu && !ignoreDebugger && !no1and1Arg) {
    host->Log("%s: every setting is off, nothing to do", kName);
    return true;
  }
  return (!hklmToHkcu ||
          (ro::HookSystem(host, "advapi32.dll", "RegOpenKeyExA", &RegOpenKeyExDetour, g_regOpenKeyEx, kName) &&
           ro::HookSystem(host, "advapi32.dll", "RegCreateKeyExA", &RegCreateKeyExDetour, g_regCreateKeyEx, kName))) &&
         (!ignoreDebugger || ro::HookSystem(host, "kernel32.dll", "IsDebuggerPresent", &IsDebuggerPresentDetour, g_isDebuggerPresent, kName)) &&
         (!no1and1Arg || ro::HookSystem(host, "vcruntime140.dll", "strstr", &StrstrDetour, g_strstr, kName));
}
