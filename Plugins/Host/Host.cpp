// RoPluginHost.dll: loaded by the exe's entry point (loader/RoLoaderEntry.cpp) before the CRT starts. Reads
// every <exe dir>\plugins\*.plugin manifest, disables plugins whose dependencies are not met, loads the rest in
// dependency order and gives each the RoHostApi (see RoPlugin.h for the manifest format).
#include "RoPlugin.h"

#include "minhook/include/MinHook.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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

  // The checks Replace and Hook share: an exe function, not folded by /OPT:ICF, room for a 5-byte jmp.
  bool CheckTarget(void* target, const char* what, const char* plugin) {
    if (!InExe(target)) {
      Log("%s: %s(%p): not a function of the exe", plugin, what, target);
      return false;
    }
    const auto rva = static_cast<std::uint32_t>(static_cast<std::uint8_t*>(target) - reinterpret_cast<std::uint8_t*>(g_exe));
    const auto [first, last] = std::equal_range(g_exportRvas.begin(), g_exportRvas.end(), rva);
    if (last - first > 1) {
      Log("%s: %s(%p): %d exported symbols share this code (/OPT:ICF); hook their callers instead", plugin, what, target,
          static_cast<int>(last - first));
      return false;
    }
    if (last != g_exportRvas.end() && *last - rva < 5) {
      Log("%s: %s(%p): the function is %u bytes, too small for a jmp", plugin, what, target, *last - rva);
      return false;
    }
    return true;
  }

  // One hooked exe function. MinHook patches it once, to `relay` (`jmp [head]`), and builds `trampoline`, which runs
  // the original code. Each Hook makes its detour the new head and hands back the previous one as `original`, so the
  // newest detour runs first and the exe is never patched again.
  struct HookChain {
    std::uint8_t* relay;
    const void* head;
    void* trampoline;
    std::vector<std::string> plugins;  // in hook order
  };

  std::map<void*, HookChain> g_hooks;  // by target
  std::uint8_t* g_relays;               // executable page of 6-byte `jmp [head]` stubs
  std::size_t g_relaysUsed;

  std::uint8_t* NewRelay(const void* const* head) {
    constexpr std::size_t size = 6;
    if (!g_relays || g_relaysUsed + size > 4096) {
      g_relays = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
      g_relaysUsed = 0;
      if (!g_relays) {
        return nullptr;
      }
    }
    std::uint8_t* relay = g_relays + g_relaysUsed;
    relay[0] = 0xFF;
    relay[1] = 0x25;
    std::memcpy(relay + 2, &head, sizeof(head));
    g_relaysUsed += size;
    return relay;
  }

  // Chains `detour` in front of `target`, which the caller has already validated.
  bool Chain(void* target, const void* detour, void** original, const char* plugin) {
    if (!detour || !original) {
      Log("%s: Hook(%p): needs a detour and somewhere to store the original", plugin, target);
      return false;
    }
    auto chain = g_hooks.find(target);
    if (chain == g_hooks.end()) {
      static const MH_STATUS initialized = MH_Initialize();
      if (initialized != MH_OK) {
        Log("%s: Hook(%p): MinHook failed to initialize: %s", plugin, target, MH_StatusToString(initialized));
        return false;
      }
      chain = g_hooks.emplace(target, HookChain{nullptr, detour, nullptr, {}}).first;
      chain->second.relay = NewRelay(&chain->second.head);
      MH_STATUS status = chain->second.relay ? MH_CreateHook(target, chain->second.relay, &chain->second.trampoline) : MH_ERROR_MEMORY_ALLOC;
      if (status == MH_OK) {
        status = MH_EnableHook(target);
      }
      if (status != MH_OK) {
        Log("%s: Hook(%p): %s", plugin, target, MH_StatusToString(status));
        MH_RemoveHook(target);
        g_hooks.erase(chain);
        return false;
      }
      *original = chain->second.trampoline;
    } else {
      *original = const_cast<void*>(chain->second.head);
      chain->second.head = detour;
    }
    chain->second.plugins.push_back(plugin);
    std::string order;
    for (auto it = chain->second.plugins.rbegin(); it != chain->second.plugins.rend(); ++it) {
      order += *it + " -> ";
    }
    Log("%s: hooked %p (calls run %soriginal)", plugin, target, order.c_str());
    return true;
  }

  bool Hook(void* target, const void* detour, void** original, const char* plugin) {
    target = FollowThunk(target);
    if (!CheckTarget(target, "Hook", plugin)) {
      return false;
    }
    if (const auto owner = g_replaced.find(target); owner != g_replaced.end()) {
      Log("%s: Hook(%p): already replaced by %s, which never calls the original", plugin, target, owner->second.c_str());
      return false;
    }
    return Chain(target, detour, original, plugin);
  }

  // The exception to "exe functions only": a function a system DLL exports, named explicitly, for behaviour that the
  // exe inlines into code no exe hook can reach. Every call in the process goes through it, not only the exe's.
  bool HookSystem(const char* module, const char* function, const void* detour, void** original, const char* plugin) {
    const HMODULE handle = module ? GetModuleHandleA(module) : nullptr;
    void* target = handle ? reinterpret_cast<void*>(GetProcAddress(handle, function)) : nullptr;
    if (!target) {
      Log("%s: HookSystem(%s!%s): not exported by a loaded module", plugin, module ? module : "(null)", function ? function : "(null)");
      return false;
    }
    Log("%s: HookSystem %s!%s", plugin, module, function);
    return Chain(target, detour, original, plugin);
  }

  bool Replace(void* target, const void* replacement, const char* plugin) {
    target = FollowThunk(target);
    if (!CheckTarget(target, "Replace", plugin)) {
      return false;
    }
    if (const auto owner = g_replaced.find(target); owner != g_replaced.end()) {
      Log("%s: Replace(%p): already replaced by %s", plugin, target, owner->second.c_str());
      return false;
    }
    if (const auto chain = g_hooks.find(target); chain != g_hooks.end()) {
      Log("%s: Replace(%p): already hooked by %s; use Hook to run alongside it", plugin, target, chain->second.plugins.front().c_str());
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

  // Semver precedence: MAJOR.MINOR.PATCH, then a release above its prereleases, identifiers compared numerically
  // when both are numeric, numeric below alphanumeric, and a shorter prerelease below a longer one it prefixes.
  struct Version {
    std::uint32_t core[3] = {};
    std::vector<std::string> prerelease;
  };

  bool IsNumber(const std::string& text) {
    return !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
  }

  bool ParseVersion(const std::string& text, Version& version) {
    const std::size_t dash = text.find('-');
    const std::string core = text.substr(0, dash);
    std::size_t start = 0;
    for (int i = 0; i < 3; ++i) {
      const std::size_t end = i < 2 ? core.find('.', start) : core.size();
      if (end == std::string::npos) {
        return false;
      }
      const std::string part = core.substr(start, end - start);
      if (!IsNumber(part) || (part.size() > 1 && part[0] == '0') || part.size() > 9) {
        return false;
      }
      version.core[i] = static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 10));
      start = end + 1;
    }
    version.prerelease.clear();
    if (dash != std::string::npos) {
      std::stringstream identifiers(text.substr(dash + 1));
      std::string identifier;
      while (std::getline(identifiers, identifier, '.')) {
        if (identifier.empty() || identifier.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-") != std::string::npos) {
          return false;
        }
        version.prerelease.push_back(identifier);
      }
      if (version.prerelease.empty() || text.back() == '.') {
        return false;
      }
    }
    return true;
  }

  int Compare(const Version& a, const Version& b) {
    for (int i = 0; i < 3; ++i) {
      if (a.core[i] != b.core[i]) {
        return a.core[i] < b.core[i] ? -1 : 1;
      }
    }
    if (a.prerelease.empty() || b.prerelease.empty()) {
      return a.prerelease.empty() == b.prerelease.empty() ? 0 : (a.prerelease.empty() ? 1 : -1);
    }
    for (std::size_t i = 0; i < a.prerelease.size() && i < b.prerelease.size(); ++i) {
      const std::string& x = a.prerelease[i];
      const std::string& y = b.prerelease[i];
      const bool xNumber = IsNumber(x);
      const bool yNumber = IsNumber(y);
      if (xNumber && yNumber) {
        if (x.size() != y.size()) {
          return x.size() < y.size() ? -1 : 1;
        }
      } else if (xNumber != yNumber) {
        return xNumber ? -1 : 1;
      }
      if (x != y) {
        return x < y ? -1 : 1;
      }
    }
    return a.prerelease.size() == b.prerelease.size() ? 0 : (a.prerelease.size() < b.prerelease.size() ? -1 : 1);
  }

  enum class Op { Equal, Less, LessEqual, Greater, GreaterEqual };

  struct Comparator {
    Op op;
    Version version;
  };

  // Comparator sets joined by `||`, as normalised by scripts/build.py (scripts/semver.py).
  using Range = std::vector<std::vector<Comparator>>;

  bool ParseRange(const std::string& text, Range& range) {
    range.assign(1, {});
    std::stringstream tokens(text);
    std::string token;
    while (tokens >> token) {
      if (token == "||") {
        range.emplace_back();
        continue;
      }
      Comparator comparator;
      std::size_t length = 1;
      if (token.compare(0, 2, ">=") == 0) {
        comparator.op = Op::GreaterEqual, length = 2;
      } else if (token.compare(0, 2, "<=") == 0) {
        comparator.op = Op::LessEqual, length = 2;
      } else if (token[0] == '>') {
        comparator.op = Op::Greater;
      } else if (token[0] == '<') {
        comparator.op = Op::Less;
      } else if (token[0] == '=') {
        comparator.op = Op::Equal;
      } else {
        return false;
      }
      if (!ParseVersion(token.substr(length), comparator.version)) {
        return false;
      }
      range.back().push_back(comparator);
    }
    return std::none_of(range.begin(), range.end(), [](const std::vector<Comparator>& set) { return set.empty(); });
  }

  bool Satisfies(const Version& version, const Range& range) {
    return std::any_of(range.begin(), range.end(), [&](const std::vector<Comparator>& set) {
      return std::all_of(set.begin(), set.end(), [&](const Comparator& comparator) {
        const int order = Compare(version, comparator.version);
        switch (comparator.op) {
          case Op::Equal: return order == 0;
          case Op::Less: return order < 0;
          case Op::LessEqual: return order <= 0;
          case Op::Greater: return order > 0;
          default: return order >= 0;
        }
      });
    });
  }

  struct Dependency {
    std::string name;
    std::string rangeText;
    Range range;
  };

  enum class State { Pending, Visiting, Resolved, Disabled, Off, Loaded };

  struct Plugin {
    std::string name;
    std::string category;
    std::string versionText;
    Version version;
    std::vector<Dependency> dependencies;
    std::vector<std::string> conflicts;  // plugins that must not run with this one
    State state = State::Pending;
    const char* failure = "disabled";  // status.txt state while Disabled: disabled | failed-load | failed-init
    std::string reason;                 // why it is Disabled or Off, as logged
  };

  std::map<std::string, Plugin> g_plugins;  // by name; nodes are stable, PluginVersion hands out versionText
  std::vector<std::string> g_refused;       // DLLs without a valid manifest

  void FailV(Plugin& plugin, const char* failure, const char* format, va_list args) {
    if (plugin.state == State::Disabled || plugin.state == State::Off) {
      return;  // the first reason stands; a plugin turned off stays off
    }
    plugin.state = State::Disabled;
    plugin.failure = failure;
    char reason[512];
    std::vsnprintf(reason, sizeof(reason), format, args);
    plugin.reason = reason;
    Log("%s %s: disabled, %s", plugin.name.c_str(), plugin.versionText.c_str(), reason);
  }

  void Disable(Plugin& plugin, const char* format, ...) {
    va_list args;
    va_start(args, format);
    FailV(plugin, "disabled", format, args);
    va_end(args);
  }

  void Fail(Plugin& plugin, const char* failure, const char* format, ...) {
    va_list args;
    va_start(args, format);
    FailV(plugin, failure, format, args);
    va_end(args);
  }

  const char* PluginVersion(const char* name) {
    const auto found = name ? g_plugins.find(name) : g_plugins.end();
    return found != g_plugins.end() && found->second.state == State::Loaded ? found->second.versionText.c_str() : nullptr;
  }

  constexpr const char* kHostVersion = "1.3.0";
  std::string g_started;  // header fields fixed at RoHost_Start
  std::string g_exeName;  // file name of the running exe, so tools find the game process
  std::string g_status;   // the last status.txt written

  const char* StateName(const Plugin& plugin) {
    switch (plugin.state) {
      case State::Loaded: return "loaded";
      case State::Off: return "off";
      case State::Disabled: return plugin.failure;
      default: return "pending";  // resolved, not loaded yet
    }
  }

  // plugins\status.txt, tab-separated (see RoPlugin.h): a header, then one line per plugin seen, replaced atomically.
  void WriteStatus() {
    std::string text = "RoPluginHost\t" + std::string(kHostVersion) + "\tapi " + std::to_string(RoHostApiVersion) + "\tpid " +
                       std::to_string(GetCurrentProcessId()) + "\tstarted " + g_started + "\texe " + g_exeName + "\n";
    for (const auto& [name, plugin] : g_plugins) {
      std::string dependencies;
      for (const Dependency& dependency : plugin.dependencies) {
        const auto found = g_plugins.find(dependency.name);
        dependencies += (dependencies.empty() ? "" : ",") + dependency.name + "=" +
                        (found != g_plugins.end() ? found->second.versionText : std::string("missing"));
      }
      text += name + "\t" + plugin.versionText + "\t" + StateName(plugin) + "\t" + plugin.reason + "\t" + dependencies + "\n";
    }
    for (const std::string& stem : g_refused) {
      text += stem + "\t-\trefused\tno valid " + stem + ".plugin manifest next to it\t\n";
    }
    g_status = text;
    const std::string path = g_directory + "plugins\\status.txt";
    const std::string temporary = path + ".tmp";
    if (FILE* file = std::fopen(temporary.c_str(), "wb")) {
      const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
      if (std::fclose(file) == 0 && written && MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return;
      }
    }
    Log("RoPluginHost: writing %s failed (%lu)", path.c_str(), GetLastError());
  }

  const char* PluginStatus() {
    return g_status.c_str();
  }

  const RoHostApi g_api = {RoHostApiVersion, Resolve, Replace, Log, PluginVersion, PluginStatus, Hook, HookSystem};

  // plugins\<file>.plugin: `name`, `category`, `version` and `depends <name> <range>` lines.
  bool ReadManifest(const std::string& file, Plugin& plugin) {
    std::ifstream input(g_directory + "plugins\\" + file);
    std::string line;
    bool hasVersion = false;
    for (int number = 1; std::getline(input, line); ++number) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      std::stringstream fields(line);
      std::string key;
      if (!(fields >> key)) {
        continue;
      }
      std::string value;
      fields >> value;
      std::string rest;
      std::getline(fields, rest);
      rest.erase(0, rest.find_first_not_of(' '));
      if (key == "name" && !value.empty() && rest.empty()) {
        plugin.name = value;
      } else if (key == "category" && !value.empty() && rest.empty()) {
        plugin.category = value;
      } else if (key == "version" && rest.empty() && ParseVersion(value, plugin.version)) {
        plugin.versionText = value;
        hasVersion = true;
      } else if (key == "depends" && !value.empty()) {
        Dependency dependency{value, rest, {}};
        if (!ParseRange(rest, dependency.range)) {
          Log("%s: line %d: invalid range \"%s\"", file.c_str(), number, rest.c_str());
          return false;
        }
        plugin.dependencies.push_back(std::move(dependency));
      } else if (key == "conflicts" && !value.empty() && rest.empty()) {
        plugin.conflicts.push_back(value);
      } else {
        Log("%s: line %d: not understood: %s", file.c_str(), number, line.c_str());
        return false;
      }
    }
    if (plugin.name.empty() || plugin.category.empty() || !hasVersion) {
      Log("%s: needs a name, a category and a version line", file.c_str());
      return false;
    }
    if (_stricmp((plugin.name + ".plugin").c_str(), file.c_str()) != 0) {
      Log("%s: names plugin %s; the file must be %s.plugin", file.c_str(), plugin.name.c_str(), plugin.name.c_str());
      return false;
    }
    return true;
  }

  std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
  }

  std::vector<std::string> List(const char* pattern) {
    std::vector<std::string> names;
    WIN32_FIND_DATAA found;
    const HANDLE search = FindFirstFileA((g_directory + "plugins\\" + pattern).c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        names.push_back(found.cFileName);
      } while (FindNextFileA(search, &found));
      FindClose(search);
    }
    std::sort(names.begin(), names.end());
    return names;
  }

  // Depth-first: `order` receives every plugin after its dependencies. A plugin on `stack` reached again closes a
  // cycle, which disables every member; a missing, mismatched or disabled dependency disables the dependent.
  void Visit(Plugin& plugin, std::vector<Plugin*>& stack, std::vector<Plugin*>& order) {
    if (plugin.state == State::Visiting) {
      std::string chain;
      for (auto member = std::find(stack.begin(), stack.end(), &plugin); member != stack.end(); ++member) {
        chain += (*member)->name + " -> ";
      }
      chain += plugin.name;
      for (auto member = std::find(stack.begin(), stack.end(), &plugin); member != stack.end(); ++member) {
        Disable(**member, "in dependency cycle %s", chain.c_str());
      }
      return;
    }
    if (plugin.state != State::Pending) {
      return;
    }
    plugin.state = State::Visiting;
    stack.push_back(&plugin);
    for (const Dependency& dependency : plugin.dependencies) {
      const auto found = g_plugins.find(dependency.name);
      if (found == g_plugins.end()) {
        Disable(plugin, "needs %s %s, not installed", dependency.name.c_str(), dependency.rangeText.c_str());
        continue;
      }
      Plugin& target = found->second;
      Visit(target, stack, order);
      if (target.state == State::Off) {
        Disable(plugin, "needs %s, which is turned off", dependency.name.c_str());
      } else if (target.state == State::Disabled) {
        Disable(plugin, "needs %s, which is disabled", dependency.name.c_str());
      } else if (!Satisfies(target.version, dependency.range)) {
        Disable(plugin, "needs %s %s, found %s", dependency.name.c_str(), dependency.rangeText.c_str(), target.versionText.c_str());
      }
    }
    stack.pop_back();
    if (plugin.state == State::Visiting) {
      plugin.state = State::Resolved;
    }
    order.push_back(&plugin);
  }

  void Load(Plugin& plugin) {
    // A dependency that failed to load or initialize disables its dependents, which are then never loaded.
    for (const Dependency& dependency : plugin.dependencies) {
      if (g_plugins.at(dependency.name).state != State::Loaded) {
        Disable(plugin, "needs %s, which is disabled", dependency.name.c_str());
        return;
      }
    }
    const std::string path = g_directory + "plugins\\" + plugin.name + ".dll";
    const HMODULE module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
      Fail(plugin, "failed-load", "LoadLibrary failed (%lu)", GetLastError());
      return;
    }
    using Init = bool (*)(const RoHostApi*);
    const auto init = reinterpret_cast<Init>(GetProcAddress(module, "RoPlugin_Init"));
    if (!init) {
      Fail(plugin, "failed-load", "no RoPlugin_Init export");
      return;
    }
    const bool initialized = init(&g_api);
    if (initialized) {
      plugin.state = State::Loaded;
      Log("%s %s: loaded", plugin.name.c_str(), plugin.versionText.c_str());
    } else {
      Fail(plugin, "failed-init", "RoPlugin_Init failed");
    }
    WriteStatus();
  }

  // plugins\disabled.txt: plugin names the user turned off, one per line, `#` starts a comment.
  std::set<std::string> TurnedOff() {
    std::set<std::string> names;
    std::ifstream input(g_directory + "plugins\\disabled.txt");
    std::string line;
    while (std::getline(input, line)) {
      line.erase(std::min(line.find('#'), line.size()));
      std::stringstream fields(line);
      std::string name;
      if (fields >> name) {
        names.insert(Lower(name));
      }
    }
    return names;
  }
}

extern "C" __declspec(dllexport) void RoHost_Start() {
  g_exe = GetModuleHandleA(nullptr);
  char path[MAX_PATH];
  const DWORD length = GetModuleFileNameA(g_exe, path, MAX_PATH);
  SYSTEMTIME now;
  GetLocalTime(&now);
  char started[32];
  std::snprintf(started, sizeof(started), "%04u-%02u-%02uT%02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
                now.wMinute, now.wSecond);
  g_started = started;
  g_exeName.assign(path, length);
  g_exeName.erase(0, g_exeName.find_last_of("\\/") + 1);
  g_directory = path;
  g_directory.resize(g_directory.find_last_of("\\/") + 1);
  DeleteFileA((g_directory + "RoPluginHost.log").c_str());
  IndexExports();
  Log("RoPluginHost: %zu exported symbols", g_exportRvas.size());

  std::set<std::string> manifests;
  for (const std::string& file : List("*.plugin")) {
    Plugin plugin;
    if (ReadManifest(file, plugin)) {
      manifests.insert(Lower(plugin.name));
      g_plugins[plugin.name] = std::move(plugin);
    }
  }
  std::set<std::string> dlls;
  for (const std::string& file : List("*.dll")) {
    const std::string stem = file.substr(0, file.size() - 4);
    dlls.insert(Lower(stem));
    if (!manifests.count(Lower(stem))) {
      Log("%s: refused, no valid %s.plugin manifest next to it", file.c_str(), stem.c_str());
      g_refused.push_back(stem);
    }
  }
  const std::set<std::string> off = TurnedOff();
  for (auto& [name, plugin] : g_plugins) {
    if (off.count(Lower(name))) {
      plugin.state = State::Off;
      plugin.reason = "turned off in plugins\\disabled.txt";
      Log("%s %s: off, %s", name.c_str(), plugin.versionText.c_str(), plugin.reason.c_str());
    } else if (!dlls.count(Lower(name))) {
      Fail(plugin, "failed-load", "%s.dll is missing", name.c_str());
    }
  }

  // Conflicts go both ways: in name order, a plugin that is still pending keeps running and every later one it
  // conflicts with is disabled. This runs before dependencies are resolved, so the dependents of a disabled plugin follow.
  const auto lists = [](const Plugin& owner, const std::string& target) {
    return std::any_of(owner.conflicts.begin(), owner.conflicts.end(),
                       [&](const std::string& entry) { return _stricmp(entry.c_str(), target.c_str()) == 0; });
  };
  for (auto first = g_plugins.begin(); first != g_plugins.end(); ++first) {
    if (first->second.state != State::Pending) {
      continue;
    }
    for (auto second = std::next(first); second != g_plugins.end(); ++second) {
      if (second->second.state == State::Pending && (lists(first->second, second->first) || lists(second->second, first->first))) {
        Disable(second->second, "conflicts with %s", first->first.c_str());
      }
    }
  }

  std::vector<Plugin*> stack;
  std::vector<Plugin*> order;
  for (auto& [name, plugin] : g_plugins) {
    Visit(plugin, stack, order);
  }

  std::string sequence;
  for (Plugin* plugin : order) {
    if (plugin->state == State::Resolved) {
      sequence += (sequence.empty() ? "" : ", ") + plugin->name + " " + plugin->versionText;
    }
  }
  Log("RoPluginHost: load order: %s", sequence.empty() ? "(none)" : sequence.c_str());
  WriteStatus();
  for (Plugin* plugin : order) {
    if (plugin->state == State::Resolved) {
      Load(*plugin);
    }
  }
}
