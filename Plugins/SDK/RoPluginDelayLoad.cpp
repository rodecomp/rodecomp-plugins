// Compiled into every plugin by scripts/build.py, which links plugins with /DELAYLOAD:Ragexe.exe delayimp.lib.
//
// Ragexe.lib names the module "Ragexe.exe", but the patched exe may be installed under any name
// (build.py --exe-name). The hook binds that import to the running exe whatever its file name.
//
// All exe imports are then bound while the DLL loads (static initialisation, before RoPlugin_Init), so the
// import thunks of Ragexe.lib jump straight into the exe: ro::Address() and RoHostApi::Replace() follow them to the
// exe function, not to a delay-load stub. Binding only uses GetProcAddress on the exe; no exe code runs.
#include <windows.h>

#include <delayimp.h>

namespace {
  FARPROC WINAPI Notify(unsigned reason, PDelayLoadInfo info) {
    if (reason == dliNotePreLoadLibrary && _stricmp(info->szDll, "Ragexe.exe") == 0) {
      return reinterpret_cast<FARPROC>(GetModuleHandleA(nullptr));
    }
    return nullptr;
  }

  const HRESULT g_bound = __HrLoadAllImportsForDll("Ragexe.exe");
}

ExternC const PfnDliHook __pfnDliNotifyHook2 = Notify;
