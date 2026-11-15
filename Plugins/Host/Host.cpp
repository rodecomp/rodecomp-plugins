// RoPluginHost.dll: loaded by the exe's entry stub (patches/0100_plugin_loader.py) before the CRT starts. Reads
// every <exe dir>\plugins\*.plugin manifest, disables plugins whose dependencies are not met, loads the rest in
// dependency order and gives each the RoHostApi (see RoPlugin.h for the manifest format).
#include "RoPlugin.h"

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

  enum class State { Pending, Visiting, Resolved, Disabled, Loaded };

  struct Plugin {
    std::string name;
    std::string versionText;
    Version version;
    std::vector<Dependency> dependencies;
    State state = State::Pending;
  };

  std::map<std::string, Plugin> g_plugins;  // by name; nodes are stable, PluginVersion hands out versionText

  void Disable(Plugin& plugin, const char* format, ...) {
    if (plugin.state == State::Disabled) {
      return;  // the first reason stands
    }
    plugin.state = State::Disabled;
    char reason[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(reason, sizeof(reason), format, args);
    va_end(args);
    Log("%s %s: disabled, %s", plugin.name.c_str(), plugin.versionText.c_str(), reason);
  }

  const char* PluginVersion(const char* name) {
    const auto found = name ? g_plugins.find(name) : g_plugins.end();
    return found != g_plugins.end() && found->second.state == State::Loaded ? found->second.versionText.c_str() : nullptr;
  }

  const RoHostApi g_api = {RoHostApiVersion, Resolve, Replace, Log, PluginVersion};

  // plugins\<file>.plugin: `name`, `version` and `depends <name> <range>` lines.
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
      } else {
        Log("%s: line %d: not understood: %s", file.c_str(), number, line.c_str());
        return false;
      }
    }
    if (plugin.name.empty() || !hasVersion) {
      Log("%s: needs a name and a version line", file.c_str());
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
      if (target.state == State::Disabled) {
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
      Disable(plugin, "LoadLibrary failed (%lu)", GetLastError());
      return;
    }
    using Init = bool (*)(const RoHostApi*);
    const auto init = reinterpret_cast<Init>(GetProcAddress(module, "RoPlugin_Init"));
    if (!init) {
      Disable(plugin, "no RoPlugin_Init export");
      return;
    }
    if (!init(&g_api)) {
      Disable(plugin, "RoPlugin_Init failed");
      return;
    }
    plugin.state = State::Loaded;
    Log("%s %s: loaded", plugin.name.c_str(), plugin.versionText.c_str());
  }
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
    }
  }
  for (auto& [name, plugin] : g_plugins) {
    if (!dlls.count(Lower(name))) {
      Disable(plugin, "%s.dll is missing", name.c_str());
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
  for (Plugin* plugin : order) {
    if (plugin->state == State::Resolved) {
      Load(*plugin);
    }
  }
}
