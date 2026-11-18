// PakRegistry::Read (every resource read, from CFile::Open) tries the loose file first; on a miss the original tries
// the archives, then the loose file again. Exists is not hooked: its result does not depend on the order.
#include "RoPlugin.h"

#include "Archive/PakRegistry.h"

namespace {
  constexpr const char* kName = "ReadDataFolderFirst";

  void* (PakRegistry::*g_original)(const char*, std::uint32_t*, int);

  struct PakRegistryHook : PakRegistry {
    void* Read(const char* filename, std::uint32_t* size, int option) {
      if (void* loose = this->ReadLooseResource(filename, size)) {
        return loose;
      }
      return (this->*g_original)(filename, size, option);
    }
  };
}

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &PakRegistry::Read, &PakRegistryHook::Read, g_original, kName);
}
