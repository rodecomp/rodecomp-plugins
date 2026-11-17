// Entry point of the plugin build of Ragexe.exe (scripts/build.py links it with /ENTRY:RoLoaderEntry). It runs before
// the CRT and the exe's static constructors: it loads RoPluginHost.dll next to the exe, if there is one, lets it load
// the plugins, then starts the game through the CRT's own entry point. Without RoPluginHost.dll the exe runs as usual.
// Compiled with /GS-: the security cookie is not initialised yet.
#include <windows.h>

extern "C" int WinMainCRTStartup();

extern "C" int RoLoaderEntry() {
  if (const HMODULE host = LoadLibraryA("RoPluginHost.dll")) {
    if (const auto start = reinterpret_cast<void (*)()>(GetProcAddress(host, "RoHost_Start"))) {
      start();
    }
  }
  return WinMainCRTStartup();
}
