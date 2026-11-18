// WARP's CustomSleep points every exe call of kernel32!Sleep at a stub that maps Sleep(0) to InputDelay
// (AddInpDelay) and Sleep(1) to GameLoopDelay (CustomGameLoopDelay). The calls are inline in many exe functions
// (Sleep(1) ends each CGameMode::Run frame; Sleep(0) yields in world model loading), so this hooks kernel32!Sleep
// itself and maps those two values for callers inside the exe only; every other caller and value passes through.
// Installed only when a delay differs from the game's.
#include "RoPlugin.h"

#include <windows.h>

#include <intrin.h>

namespace {
  constexpr const char* kName = "SleepDelays";

  DWORD g_inputDelay;
  DWORD g_gameLoopDelay;

  decltype(&::Sleep) g_original;

  bool CalledByExe(const void* returnAddress) {
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<const char*>(returnAddress), &module) &&
           module == GetModuleHandleA(nullptr);
  }

  void WINAPI SleepDetour(DWORD milliseconds) {
    if (milliseconds <= 1 && CalledByExe(_ReturnAddress())) {
      milliseconds = milliseconds == 0 ? g_inputDelay : g_gameLoopDelay;
    }
    g_original(milliseconds);
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  g_inputDelay = static_cast<DWORD>(host->SettingInt(kName, "InputDelay"));
  g_gameLoopDelay = static_cast<DWORD>(host->SettingInt(kName, "GameLoopDelay"));
  if (g_inputDelay == 0 && g_gameLoopDelay == 1) {
    host->Log("%s: both delays are the game's, nothing to do", kName);
    return true;
  }
  host->Log("%s: Sleep(0) -> %lu ms, Sleep(1) -> %lu ms", kName, g_inputDelay, g_gameLoopDelay);
  return ro::HookSystem(host, "kernel32.dll", "Sleep", &SleepDetour, g_original, kName);
}
