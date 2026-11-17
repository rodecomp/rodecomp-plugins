// Ragexe plugin SDK.
//
// A plugin is a DLL in <client>\plugins\ built by scripts/build.py against Ragexe.lib (every function of the
// exe, exported when scripts/plugin_exe.py links it) and RagexeGlobals.obj (every global of the exe at its fixed
// address). It includes Source/ headers and calls exe functions and globals like its own.
//
// It exports
//     extern "C" __declspec(dllexport) bool RoPlugin_Init(const RoHostApi* host);
// which runs from the exe's entry point, before the CRT and the exe's static constructors: install replacements
// there, touch no exe global. Returning false disables the plugin and every plugin that depends on it.
//
// Manifests and dependencies. Every plugin has Plugins/<Category>/<Name>/plugin.json:
//     {"name": "MyEffects", "category": "Graphics", "version": "0.1.0", "description": "...",
//      "dependencies": {"RendererDX12": "^1.1.0"}, "libs": ["d3d12.lib"]}
// name equals the directory name (unique across categories), category is required and equals the parent directory,
// version is semver (MAJOR.MINOR.PATCH[-prerelease]) and dependencies map plugin
// names to npm-style ranges (exact, ^, ~, >= > < <= comparators joined by spaces, *, ||). The build resolves them
// against the local plugins, fails on a missing, out-of-range or cyclic dependency, and builds in dependency order.
// A plugin is compiled with /D<NAME>_EXPORTS (its name in upper case) and with /I on each dependency's directory,
// and links each dependency's import library: a dependency exports its API through a macro such as
//     #ifdef RENDERERDX12_EXPORTS
//     #define RENDERERDX12_API __declspec(dllexport)
//     #else
//     #define RENDERERDX12_API __declspec(dllimport)
//     #endif
// in a public header (Plugins/Graphics/RendererDX12/RendererDX12Api.h) that dependents include.
//
// The build installs <client>\plugins\<Name>.plugin next to <Name>.dll, one field per line:
//     name MyEffects
//     category Graphics
//     version 0.1.0
//     depends RendererDX12 >=1.1.0 <2.0.0-0
// with each range normalised to comparator sets (`op version` joined by spaces, sets joined by `||`). The host reads
// every .plugin first, refuses a DLL without one, disables a plugin whose dependency is missing, out of range, in a
// cycle or itself disabled (logging why), then loads the rest with LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH), so
// imports of sibling plugin DLLs resolve from plugins\, and calls RoPlugin_Init in dependency order.
//
// Plugins link Ragexe.lib with /DELAYLOAD:Ragexe.exe and Plugins/SDK/RoPluginDelayLoad.cpp, which binds those
// imports to the running exe while the DLL loads, so the patched exe may have any file name.
//
// User switches and runtime status (tools/plugin-manager (ro-plugins) reads and edits both):
//   plugins\disabled.txt  one plugin name per line, `#` comments: the plugin is `off` and not loaded; its dependents
//                         are disabled with "needs X, which is turned off".
//   plugins\status.txt    rewritten atomically after the load pass and after every RoPlugin_Init, tab-separated:
//       RoPluginHost <host version> api <N> pid <pid> started <YYYY-MM-DDTHH:MM:SS> exe <exe file name>
//       <name> <version> <state> <reason> <dependency>=<resolved version>[,...]
//     state: loaded | disabled | failed-init | failed-load | refused (DLL without manifest, version `-`) | off |
//     pending (resolved, not initialised yet); reason is the log wording, empty when loaded.
#ifndef RAGEXE_PLUGIN_SDK_ROPLUGIN_H
#define RAGEXE_PLUGIN_SDK_ROPLUGIN_H

#include <cstdint>
#include <cstring>

// Fields are only ever appended; check `version` before using one added after version 1.
struct RoHostApi {
  std::uint32_t version;
  // Address of an exe function or global by decorated name (the linker map name), or nullptr.
  void* (*Resolve)(const char* decorated);
  // Send every call of `target` (an exe function) to `replacement`, which takes it over: the original code never
  // runs. False (and a log line) when the function is shared with other symbols by /OPT:ICF, too small to patch,
  // already replaced by another plugin, or hooked (prefer Hook, which lets several plugins share a function).
  bool (*Replace)(void* target, const void* replacement, const char* plugin);
  // Append a line to RoPluginHost.log.
  void (*Log)(const char* format, ...);
  // Version 2. The manifest version of a loaded plugin (its DLL is loaded and not disabled), or nullptr.
  const char* (*PluginVersion)(const char* name);
  // Version 3. The current plugins\status.txt text (header and plugin lines, '\n'-terminated); valid until the next
  // plugin finishes initialising.
  const char* (*PluginStatus)();
  // Version 4. Chain `detour` in front of `target` (an exe function) and store in `*original` what the detour calls
  // to continue: the previous plugin's detour, or the original function. Several plugins may hook one function; the
  // last hooked runs first. A detour that does not call `*original` skips every detour hooked before it and the
  // original. False (and a log line) under the same conditions as Replace, or when `target` is replaced.
  bool (*Hook)(void* target, const void* detour, void** original, const char* plugin);
  // Version 5. Hook, for `function` exported by the loaded system DLL `module` (e.g. "kernel32.dll",
  // "CreateMutexA"). The exception to hooking exe functions: only for behaviour the exe inlines where no exe hook
  // reaches it. It applies to every caller in the process, so the detour must filter for the game's call.
  bool (*HookSystem)(const char* module, const char* function, const void* detour, void** original, const char* plugin);
};

constexpr std::uint32_t RoHostApiVersion = 5;

namespace ro {
  // Address of a member function, e.g. ro::Address(&RenderSystemHolder::Initialize): the import thunk of
  // Ragexe.lib, which Replace() and Hook() follow to the function in the exe.
  template <class Member>
  void* Address(Member member) {
    static_assert(sizeof(Member) == sizeof(void*), "only single-inheritance member functions");
    void* address;
    std::memcpy(&address, &member, sizeof(address));
    return address;
  }

  // Typed Hook (API v4). `Target` is the exe function's type, e.g. bool (CTownInfoMgr::*)(). The detour must convert
  // to it: a free function of the same signature, or a member of a class derived from the target's class with the
  // same signature (`struct TownInfoHook : CTownInfoMgr { bool LoadScript(); };`). A signature change in rodecomp
  // stops the plugin from compiling. `original` receives what the detour calls to continue: (this->*original)().
  template <class Target, class Detour>
  bool Hook(const RoHostApi* host, Target target, Detour detour, Target& original, const char* plugin) {
    static_assert(sizeof(Target) == sizeof(void*), "only single-inheritance member functions");
    if (host->version < 4) {
      host->Log("%s: needs RoPluginHost API 4 for Hook, found %u", plugin, host->version);
      return false;
    }
    void* next = nullptr;
    if (!host->Hook(Address(target), Address(static_cast<Target>(detour)), &next, plugin)) {
      return false;
    }
    std::memcpy(&original, &next, sizeof(next));
    return true;
  }

  // Typed Hook of a virtual function. &Class::Virtual is a vcall thunk in the plugin, not the exe function, so the
  // target is found by its decorated name (Ragexe.map), e.g. "?SendMsg@CGameMode@@UAEHHHHHH@Z". `Target` still
  // checks the detour's signature at compile time; a wrong or renamed name fails at run time (logged, false).
  template <class Target, class Detour>
  bool HookVirtual(const RoHostApi* host, const char* decorated, Detour detour, Target& original, const char* plugin) {
    static_assert(sizeof(Target) == sizeof(void*), "only single-inheritance member functions");
    if (host->version < 4) {
      host->Log("%s: needs RoPluginHost API 4 for Hook, found %u", plugin, host->version);
      return false;
    }
    void* target = host->Resolve(decorated);
    if (!target) {
      host->Log("%s: no exe function named %s", plugin, decorated);
      return false;
    }
    void* next = nullptr;
    if (!host->Hook(target, Address(static_cast<Target>(detour)), &next, plugin)) {
      return false;
    }
    std::memcpy(&original, &next, sizeof(next));
    return true;
  }

  // Typed HookSystem (API v5): `detour` must have the system function's exact type, which the declaration from
  // <windows.h> fixes, e.g. ro::HookSystem(host, "kernel32.dll", "CreateMutexA", &CreateInstanceMutex, g_original, kName)
  // with `decltype(&::CreateMutexA) g_original`.
  template <class Function>
  bool HookSystem(const RoHostApi* host, const char* module, const char* function, Function detour, Function& original,
                  const char* plugin) {
    if (host->version < 5) {
      host->Log("%s: needs RoPluginHost API 5 for HookSystem, found %u", plugin, host->version);
      return false;
    }
    void* next = nullptr;
    if (!host->HookSystem(module, function, reinterpret_cast<const void*>(detour), &next, plugin)) {
      return false;
    }
    original = reinterpret_cast<Function>(next);
    return true;
  }
}

#define RO_PLUGIN_EXPORT extern "C" __declspec(dllexport)

#endif
