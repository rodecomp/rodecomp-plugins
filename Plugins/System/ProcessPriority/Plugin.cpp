// Sets the process to the Priority setting's class (Normal or High) where the game applies g_session.m_priorityClass:
// the startup SetPriorityClass is inline in WinMain, so CModeMgr::Run (its next call, run once) sets it first;
// SetApplicationActive is hooked to set it again after the original when the window becomes active.
#include "RoPlugin.h"

#include "Client/Application/ApplicationActivation.h"
#include "Client/Mode/ModeMgr.h"

#include <cstring>
#include <windows.h>

namespace {
  constexpr const char* kName = "ProcessPriority";

  DWORD g_priorityClass;

  void (CModeMgr::*g_originalRun)(int, const char*);
  decltype(&SetApplicationActive) g_originalSetApplicationActive;

  struct ModeMgrHook : CModeMgr {
    void Run(int mode, const char* name) {
      SetPriorityClass(GetCurrentProcess(), g_priorityClass);
      (this->*g_originalRun)(mode, name);
    }
  };

  void SetApplicationActiveHook(int value) {
    g_originalSetApplicationActive(value);
    if (value != 0) {
      SetPriorityClass(GetCurrentProcess(), g_priorityClass);
    }
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  if (host->version < 6) {
    host->Log("%s: needs RoPluginHost API 6 for settings, found %u", kName, host->version);
    return false;
  }
  const char* priority = host->SettingString(kName, "Priority");
  g_priorityClass = std::strcmp(priority, "Normal") == 0 ? NORMAL_PRIORITY_CLASS : HIGH_PRIORITY_CLASS;
  host->Log("%s: %s priority", kName, priority);
  return ro::Hook(host, &CModeMgr::Run, &ModeMgrHook::Run, g_originalRun, kName) &&
         ro::Hook(host, &SetApplicationActive, &SetApplicationActiveHook, g_originalSetApplicationActive, kName);
}
