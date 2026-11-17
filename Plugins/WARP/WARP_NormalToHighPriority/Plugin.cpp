// Sets the process to high priority where the game applies g_session.m_priorityClass: the startup SetPriorityClass is
// inline in WinMain, so CModeMgr::Run (its next call, run once) sets it first; SetApplicationActive is hooked to set it
// again after the original when the window becomes active.
#include "RoPlugin.h"

#include "Client/Application/ApplicationActivation.h"
#include "Client/Mode/ModeMgr.h"

#include <windows.h>

namespace {
  constexpr const char* kName = "WARP_NormalToHighPriority";

  void (CModeMgr::*g_originalRun)(int, const char*);
  decltype(&SetApplicationActive) g_originalSetApplicationActive;

  struct ModeMgrHook : CModeMgr {
    void Run(int mode, const char* name) {
      SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
      (this->*g_originalRun)(mode, name);
    }
  };

  void SetApplicationActiveHook(int value) {
    g_originalSetApplicationActive(value);
    if (value != 0) {
      SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CModeMgr::Run, &ModeMgrHook::Run, g_originalRun, kName) &&
         ro::Hook(host, &SetApplicationActive, &SetApplicationActiveHook, g_originalSetApplicationActive, kName);
}
