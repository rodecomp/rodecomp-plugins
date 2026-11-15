// Ragexe plugin SDK.
//
// A plugin is a DLL in <client>\plugins\ built by scripts/build.py against Ragexe.lib (every C++ function of the
// exe, exported by patches/0100_plugin_loader.py) and RagexeGlobals.obj (every global of the exe at its fixed
// address). It includes Source/ headers and calls exe functions and globals like its own.
//
// It exports
//     extern "C" __declspec(dllexport) bool RoPlugin_Init(const RoHostApi* host);
// which runs from the exe's entry point, before the CRT and the exe's static constructors: install replacements
// there, touch no exe global. Returning false disables the plugin and every plugin that depends on it.
//
// Manifests and dependencies. Every plugin has Plugins/<Name>/plugin.json:
//     {"name": "DX12Effects", "version": "0.1.0", "description": "...",
//      "dependencies": {"RendererDX12": "^1.0.0"}, "libs": ["d3d12.lib"]}
// name equals the directory name, version is semver (MAJOR.MINOR.PATCH[-prerelease]) and dependencies map plugin
// names to npm-style ranges (exact, ^, ~, >= > < <= comparators joined by spaces, *, ||). The build resolves them
// against the local plugins, fails on a missing, out-of-range or cyclic dependency, and builds in dependency order.
// A plugin is compiled with /D<NAME>_EXPORTS (its name in upper case) and with /I on each dependency's directory,
// and links each dependency's import library: a dependency exports its API through a macro such as
//     #ifdef RENDERERDX12_EXPORTS
//     #define RENDERERDX12_API __declspec(dllexport)
//     #else
//     #define RENDERERDX12_API __declspec(dllimport)
//     #endif
// in a public header (Plugins/RendererDX12/RendererDX12Api.h) that dependents include.
//
// The build installs <client>\plugins\<Name>.plugin next to <Name>.dll, one field per line:
//     name DX12Effects
//     version 0.1.0
//     depends RendererDX12 >=1.0.0 <2.0.0-0
// with each range normalised to comparator sets (`op version` joined by spaces, sets joined by `||`). The host reads
// every .plugin first, refuses a DLL without one, disables a plugin whose dependency is missing, out of range, in a
// cycle or itself disabled (logging why), then loads the rest with LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH), so
// imports of sibling plugin DLLs resolve from plugins\, and calls RoPlugin_Init in dependency order.
#ifndef RAGEXE_PLUGIN_SDK_ROPLUGIN_H
#define RAGEXE_PLUGIN_SDK_ROPLUGIN_H

#include <cstdint>
#include <cstring>

// Fields are only ever appended; check `version` before using one added after version 1.
struct RoHostApi {
  std::uint32_t version;
  // Address of an exe function or global by decorated name (the linker map name), or nullptr.
  void* (*Resolve)(const char* decorated);
  // Send every call of `target` (an exe function) to `replacement`; false (and a log line) when the function is
  // shared with other symbols by /OPT:ICF, too small to patch, or already replaced by another plugin.
  bool (*Replace)(void* target, const void* replacement, const char* plugin);
  // Append a line to RoPluginHost.log.
  void (*Log)(const char* format, ...);
  // Version 2. The manifest version of a loaded plugin (its DLL is loaded and not disabled), or nullptr.
  const char* (*PluginVersion)(const char* name);
};

constexpr std::uint32_t RoHostApiVersion = 2;

namespace ro {
  // Address of a member function, e.g. ro::Address(&RenderSystemHolder::Initialize): the import thunk of
  // Ragexe.lib, which Replace() follows to the function in the exe.
  template <class Member>
  void* Address(Member member) {
    static_assert(sizeof(Member) == sizeof(void*), "only single-inheritance member functions");
    void* address;
    std::memcpy(&address, &member, sizeof(address));
    return address;
  }
}

#define RO_PLUGIN_EXPORT extern "C" __declspec(dllexport)

#endif
