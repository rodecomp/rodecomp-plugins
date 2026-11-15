// Ragexe plugin SDK.
//
// A plugin is a DLL in <client>\plugins\ built by scripts/plugins/build_plugins.py against Ragexe.lib (every C++
// function of the exe, exported by scripts/exe/patches/0100_plugin_loader.py) and RagexeGlobals.obj (every global
// of the exe at its fixed address). It includes Source/ headers and calls exe functions and globals like its own.
//
// It exports
//     extern "C" __declspec(dllexport) bool RoPlugin_Init(const RoHostApi* host);
// which runs from the exe's entry point, before the CRT and the exe's static constructors: install replacements
// there, touch no exe global.
#ifndef RAGEXE_PLUGIN_SDK_ROPLUGIN_H
#define RAGEXE_PLUGIN_SDK_ROPLUGIN_H

#include <cstdint>
#include <cstring>

struct RoHostApi {
  std::uint32_t version;
  // Address of an exe function or global by decorated name (the linker map name), or nullptr.
  void* (*Resolve)(const char* decorated);
  // Send every call of `target` (an exe function) to `replacement`; false (and a log line) when the function is
  // shared with other symbols by /OPT:ICF, too small to patch, or already replaced by another plugin.
  bool (*Replace)(void* target, const void* replacement, const char* plugin);
  // Append a line to RoPluginHost.log.
  void (*Log)(const char* format, ...);
};

constexpr std::uint32_t RoHostApiVersion = 1;

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
