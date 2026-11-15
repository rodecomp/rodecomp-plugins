// RoPluginHost.dll: loaded by the exe's entry stub (scripts/exe/patches/0100_plugin_loader.py) before the CRT
// starts. Loads <exe dir>\plugins\*.dll in name order and gives each the RoHostApi.
#include "RoPlugin.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {
  HMODULE g_exe;
  std::string g_directory;
  std::vector<std::uint32_t> g_exportRvas;  // sorted, one entry per exported name
  std::map<void*, std::string> g_replaced;  // target -> plugin

  void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (FILE* file = std::fopen((g_directory + "RoPluginHost.log").c_str(), "a")) {
      std::fprintf(file, "%s\n", line);
      std::fclose(file);
    }
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
  }

  const IMAGE_NT_HEADERS* Headers() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe);
    return reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(g_exe) + dos->e_lfanew);
  }

  void IndexExports() {
    const auto& directory = Headers()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) {
      return;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(g_exe);
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    g_exportRvas.assign(functions, functions + exports->NumberOfFunctions);
    std::sort(g_exportRvas.begin(), g_exportRvas.end());
  }

  bool InExe(const void* address) {
    const auto offset = reinterpret_cast<std::uintptr_t>(address) - reinterpret_cast<std::uintptr_t>(g_exe);
    return offset < Headers()->OptionalHeader.SizeOfImage;
  }

  // An import thunk (jmp [__imp_X]) resolves to X.
  void* FollowThunk(void* address) {
    const auto* code = static_cast<const std::uint8_t*>(address);
    if (code[0] == 0xFF && code[1] == 0x25) {
      return **reinterpret_cast<void** const*>(code + 2);
    }
    return address;
  }

  void* Resolve(const char* decorated) {
    return reinterpret_cast<void*>(GetProcAddress(g_exe, decorated));
  }

  bool Replace(void* target, const void* replacement, const char* plugin) {
    target = FollowThunk(target);
    if (!InExe(target)) {
      Log("%s: Replace(%p): not a function of the exe", plugin, target);
      return false;
    }
    const auto rva = static_cast<std::uint32_t>(static_cast<std::uint8_t*>(target) - reinterpret_cast<std::uint8_t*>(g_exe));
    const auto [first, last] = std::equal_range(g_exportRvas.begin(), g_exportRvas.end(), rva);
    if (last - first > 1) {
      Log("%s: Replace(%p): %d exported symbols share this code (/OPT:ICF); replace their callers instead", plugin, target,
          static_cast<int>(last - first));
      return false;
    }
    if (last != g_exportRvas.end() && *last - rva < 5) {
      Log("%s: Replace(%p): the function is %u bytes, too small for a jmp", plugin, target, *last - rva);
      return false;
    }
    if (const auto owner = g_replaced.find(target); owner != g_replaced.end()) {
      Log("%s: Replace(%p): already replaced by %s", plugin, target, owner->second.c_str());
      return false;
    }
    std::uint8_t jump[5] = {0xE9};
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(replacement) - (reinterpret_cast<std::uintptr_t>(target) + 5));
    std::memcpy(jump + 1, &relative, sizeof(relative));
    DWORD protection;
    if (!VirtualProtect(target, sizeof(jump), PAGE_EXECUTE_READWRITE, &protection)) {
      Log("%s: Replace(%p): VirtualProtect failed (%lu)", plugin, target, GetLastError());
      return false;
    }
    std::memcpy(target, jump, sizeof(jump));
    VirtualProtect(target, sizeof(jump), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(jump));
    g_replaced[target] = plugin;
    Log("%s: replaced %p", plugin, target);
    return true;
  }

  const RoHostApi g_api = {RoHostApiVersion, Resolve, Replace, Log};
}

extern "C" __declspec(dllexport) void RoHost_Start() {
  g_exe = GetModuleHandleA(nullptr);
  char path[MAX_PATH];
  GetModuleFileNameA(g_exe, path, MAX_PATH);
  g_directory = path;
  g_directory.resize(g_directory.find_last_of("\\/") + 1);
  DeleteFileA((g_directory + "RoPluginHost.log").c_str());
  IndexExports();
  Log("RoPluginHost: %zu exported symbols", g_exportRvas.size());

  std::vector<std::string> names;
  WIN32_FIND_DATAA found;
  const HANDLE search = FindFirstFileA((g_directory + "plugins\\*.dll").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      names.push_back(found.cFileName);
    } while (FindNextFileA(search, &found));
    FindClose(search);
  }
  std::sort(names.begin(), names.end());

  for (const std::string& name : names) {
    const HMODULE plugin = LoadLibraryA((g_directory + "plugins\\" + name).c_str());
    if (!plugin) {
      Log("%s: LoadLibrary failed (%lu)", name.c_str(), GetLastError());
      continue;
    }
    using Init = bool (*)(const RoHostApi*);
    const auto init = reinterpret_cast<Init>(GetProcAddress(plugin, "RoPlugin_Init"));
    if (!init) {
      Log("%s: no RoPlugin_Init export", name.c_str());
      continue;
    }
    Log("%s: %s", name.c_str(), init(&g_api) ? "loaded" : "RoPlugin_Init failed");
  }
}
