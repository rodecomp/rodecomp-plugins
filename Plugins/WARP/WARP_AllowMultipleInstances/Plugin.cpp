// WinMain refuses to start when the "MutexInit" mutex already exists. That check (AcquireInstanceMutex) is inlined
// into WinMain, which no exe hook can reach without copying all of WinMain, so this plugin is the one exception that
// hooks a system function: kernel32!CreateMutexA. For the exe's own "MutexInit" call it clears ERROR_ALREADY_EXISTS,
// the error WinMain reads right after; every other caller and mutex name passes through untouched.
#include "RoPlugin.h"

#include <windows.h>

#include <cstring>
#include <intrin.h>

namespace {
  constexpr const char* kName = "WARP_AllowMultipleInstances";
  constexpr const char* kMutexName = "MutexInit";

  decltype(&::CreateMutexA) g_original;

  bool CalledByExe(const void* returnAddress) {
    HMODULE module = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<const char*>(returnAddress), &module) &&
           module == GetModuleHandleA(nullptr);
  }

  HANDLE WINAPI CreateInstanceMutex(LPSECURITY_ATTRIBUTES attributes, BOOL initialOwner, LPCSTR name) {
    const void* caller = _ReturnAddress();
    HANDLE mutex = g_original(attributes, initialOwner, name);
    if (name && std::strcmp(name, kMutexName) == 0 && GetLastError() == ERROR_ALREADY_EXISTS && CalledByExe(caller)) {
      SetLastError(ERROR_SUCCESS);
    }
    return mutex;
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::HookSystem(host, "kernel32.dll", "CreateMutexA", &CreateInstanceMutex, g_original, kName);
}
