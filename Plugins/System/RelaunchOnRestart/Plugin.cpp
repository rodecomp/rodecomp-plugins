// At the end of WinMain, g_restartClient (set when Game Settings applies a change that needs a restart) makes the
// client ShellExecute a web address. CleanupUnhandledExceptionHandler is WinMain's last call before that check: after
// the original, a set flag starts this exe again with WinMain's own arguments in the current folder and is cleared,
// so the address is not opened.
#include "RoPlugin.h"

#include "Client/Application/UnhandledExceptionHandler.h"
#include "UI/GameSettings/GameSettingsUI.h"

#include <shellapi.h>
#include <windows.h>

namespace {
  constexpr const char* kName = "RelaunchOnRestart";

  decltype(&CleanupUnhandledExceptionHandler) g_original;

  // GetCommandLineA without the program name, as WinMain's lpCmdLine.
  const char* Arguments() {
    const char* line = GetCommandLineA();
    if (*line == '"') {
      ++line;
      while (*line != '\0' && *line != '"') {
        ++line;
      }
      if (*line == '"') {
        ++line;
      }
    } else {
      while (*line != '\0' && *line != ' ' && *line != '\t') {
        ++line;
      }
    }
    while (*line == ' ' || *line == '\t') {
      ++line;
    }
    return line;
  }

  void CleanupHook() {
    g_original();
    if (!g_restartClient) {
      return;
    }
    g_restartClient = false;
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) != 0) {
      ShellExecuteA(nullptr, "open", path, Arguments(), nullptr, SW_SHOWNORMAL);
    }
  }
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CleanupUnhandledExceptionHandler, &CleanupHook, g_original, kName);
}
