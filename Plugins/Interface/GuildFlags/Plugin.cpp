// The launcher preloads model\3dmob\guildflag90_1.gr2 with GrannyAnimationLibrary::Load(name, 1, 0); WARP changes the
// PUSH 5 of the original client's call to 1. That load is hooked to pass job 1 for that model on any other value.
#include "RoPlugin.h"

#include "Resource/Models/GrannyModelResource.h"

#include <cstring>

namespace {
  constexpr const char* kName = "GuildFlags";

  bool (GrannyAnimationLibrary::*g_load)(const char*, int, int);

  struct LibraryHook : GrannyAnimationLibrary {
    bool LoadDetour(const char* name, int job, int action) {
      if (name != nullptr && _stricmp(name, "model\\3dmob\\guildflag90_1.gr2") == 0) {
        job = 1;
      }
      return (this->*g_load)(name, job, action);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &GrannyAnimationLibrary::Load, &LibraryHook::LoadDetour, g_load, kName);
}
