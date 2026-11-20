// RoPluginHost.dll: loaded by the exe's entry point (loader/RoLoaderEntry.cpp) before the CRT starts. Reads
// every <exe dir>\plugins\<Name>.toml (manifest and settings), disables plugins whose dependencies are not met, loads
// the rest in dependency order and gives each the RoHostApi (see RoPlugin.h for the file format).
#include "RoPlugin.h"

#include "minhook/include/MinHook.h"
#include "tomlplusplus/toml.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
  HMODULE g_exe;
  std::string g_directory;
  std::vector<std::uint32_t> g_exportRvas;  // sorted, one entry per exported function (access aliases merged)
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

  // A private/protected member's public-form name (scripts/plugin_exe.py) differs from it by one access code after
  // an `@@`: private/protected A-P -> public Q-X (same kind, code % 8), static data 0/1 -> 2.
  bool AccessAlias(const char* alias, const char* name) {
    if (std::strlen(alias) != std::strlen(name)) {
      return false;
    }
    int differences = 0;
    for (std::size_t i = 0; name[i]; ++i) {
      if (alias[i] == name[i]) {
        continue;
      }
      const char a = alias[i];
      const char n = name[i];
      const bool publicForm = (n >= 'A' && n <= 'P' && a == 'Q' + (n - 'A') % 8) || ((n == '0' || n == '1') && a == '2');
      if (++differences > 1 || i < 2 || name[i - 1] != '@' || name[i - 2] != '@' || !publicForm) {
        return false;
      }
    }
    return differences == 1;
  }

  // One RVA per exported function: an access alias of another name at the same RVA is not an /OPT:ICF fold.
  void IndexExports() {
    const auto& directory = Headers()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress) {
      return;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(g_exe);
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    std::vector<std::pair<std::uint32_t, const char*>> named;
    named.reserve(exports->NumberOfNames);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
      named.emplace_back(functions[ordinals[i]], reinterpret_cast<const char*>(base + names[i]));
    }
    std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    g_exportRvas.reserve(named.size());
    for (std::size_t i = 0; i < named.size(); ++i) {
      bool alias = false;
      for (std::size_t j = i; j-- > 0 && named[j].first == named[i].first && !alias;) {
        alias = AccessAlias(named[i].second, named[j].second) || AccessAlias(named[j].second, named[i].second);
      }
      if (!alias) {
        g_exportRvas.push_back(named[i].first);
      }
    }
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

  enum class Type { Bool, Int, Float, String, Path, Enum, List };

  // One [settings.<key>]: its declaration's type and the value in effect (`value` if valid, else `default`).
  struct Setting {
    std::string key;
    Type type = Type::Bool;
    Type of = Type::String;  // List: element type (String, Path or Int)
    bool boolean = false;
    long long integer = 0;
    double number = 0;
    std::string text;                 // String, Enum; Path absolute (empty stays empty)
    std::vector<std::string> items;   // List of String or Path (paths absolute)
    std::vector<long long> integers;  // List of Int
  };

  struct Plugin {
    std::string name;
    std::string category;
    std::string versionText;
    Version version;
    std::vector<Dependency> dependencies;
    std::vector<std::string> conflicts;  // plugins that must not run with this one
    std::vector<Setting> settings;       // in file order
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

  constexpr const char* kHostVersion = "1.6.1";
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
      text += stem + "\t-\trefused\tno valid " + stem + ".toml manifest next to it\t\n";
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

  // plugins\<Name>.toml, see RoPlugin.h. Paths in settings are relative to the client folder (the exe's directory).
  std::string Absolute(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    const bool absolute = (path.size() >= 2 && path[1] == ':') || (!path.empty() && path[0] == '\\');
    return path.empty() || absolute ? path : g_directory + path;
  }

  bool ParseType(std::string_view name, Type& type) {
    static const std::pair<std::string_view, Type> types[] = {{"bool", Type::Bool}, {"int", Type::Int}, {"float", Type::Float},
                                                              {"string", Type::String}, {"path", Type::Path}, {"enum", Type::Enum},
                                                              {"list", Type::List}};
    for (const auto& [text, value] : types) {
      if (name == text) {
        type = value;
        return true;
      }
    }
    return false;
  }

  bool IsStringArray(const toml::node* node) {
    const toml::array* array = node ? node->as_array() : nullptr;
    return array && std::all_of(array->begin(), array->end(), [](const toml::node& entry) { return entry.is_string(); });
  }

  // Why an enum's `choices` table (value -> { label, description }) is invalid, or empty; absent is valid.
  std::string CheckChoices(const toml::node* node, const toml::array& values) {
    if (!node) {
      return {};
    }
    const toml::table* choices = node->as_table();
    if (!choices) {
      return "choices must be a table";
    }
    for (const auto& [value, entry] : *choices) {
      const std::string name(value.str());
      if (std::none_of(values.begin(), values.end(), [&](const toml::node& item) { return item.value<std::string_view>() == value.str(); })) {
        return "choices: \"" + name + "\" is not one of values";
      }
      const toml::table* choice = entry.as_table();
      if (!choice) {
        return "choices: \"" + name + "\" must be a table";
      }
      for (const auto& [key, text] : *choice) {
        if (key.str() != "label" && key.str() != "description") {
          return "choices: \"" + name + "\": unknown key " + std::string(key.str());
        }
        if (!text.is_string()) {
          return "choices: \"" + name + "\": " + std::string(key.str()) + " must be a string";
        }
      }
    }
    return {};
  }

  // Takes `node` as the setting's value if it is valid under `declaration`; else returns why not.
  std::string Accept(const toml::node& node, const toml::table& declaration, Setting& setting) {
    switch (setting.type) {
      case Type::Bool:
        if (const auto* value = node.as_boolean()) {
          setting.boolean = value->get();
          return {};
        }
        return "not a boolean";
      case Type::Int: {
        const auto* value = node.as_integer();
        if (!value) {
          return "not an integer";
        }
        const long long number = value->get();
        if (const auto min = declaration["min"].value<long long>(); min && number < *min) {
          return std::to_string(number) + " is below min " + std::to_string(*min);
        }
        if (const auto max = declaration["max"].value<long long>(); max && number > *max) {
          return std::to_string(number) + " is above max " + std::to_string(*max);
        }
        setting.integer = number;
        return {};
      }
      case Type::Float: {
        double number;
        if (const auto* integer = node.as_integer()) {
          number = static_cast<double>(integer->get());
        } else if (const auto* floating = node.as_floating_point()) {
          number = floating->get();
        } else {
          return "not a number";
        }
        if (const auto min = declaration["min"].value<double>(); min && number < *min) {
          return std::to_string(number) + " is below min " + std::to_string(*min);
        }
        if (const auto max = declaration["max"].value<double>(); max && number > *max) {
          return std::to_string(number) + " is above max " + std::to_string(*max);
        }
        setting.number = number;
        return {};
      }
      case Type::String:
      case Type::Path:
      case Type::Enum: {
        const auto* value = node.as_string();
        if (!value) {
          return "not a string";
        }
        if (setting.type == Type::Enum) {
          const toml::array& values = *declaration["values"].as_array();
          if (std::none_of(values.begin(), values.end(), [&](const toml::node& entry) { return entry.value<std::string_view>() == value->get(); })) {
            return "\"" + value->get() + "\" is not one of its values";
          }
        }
        setting.text = setting.type == Type::Path ? Absolute(value->get()) : value->get();
        return {};
      }
      case Type::List: {
        const auto* value = node.as_array();
        if (!value) {
          return "not a list";
        }
        std::vector<std::string> items;
        std::vector<long long> integers;
        for (std::size_t i = 0; i < value->size(); ++i) {
          const toml::node& element = *value->get(i);
          if (setting.of == Type::Int) {
            const auto* integer = element.as_integer();
            if (!integer) {
              return "element " + std::to_string(i) + " is not an integer";
            }
            integers.push_back(integer->get());
          } else {
            const auto* text = element.as_string();
            if (!text) {
              return "element " + std::to_string(i) + " is not a string";
            }
            items.push_back(setting.of == Type::Path ? Absolute(text->get()) : text->get());
          }
        }
        setting.items = std::move(items);
        setting.integers = std::move(integers);
        return {};
      }
    }
    return "unknown type";
  }

  // A [settings.<key>] declaration: why it is invalid, or empty with `setting` holding its value (`value` when valid,
  // else `default`, after logging why `value` was not used).
  std::string ReadSetting(const std::string& plugin, const std::string& key, const toml::node& node, Setting& setting) {
    const toml::table* declaration = node.as_table();
    if (!declaration) {
      return "must be a table";
    }
    setting.key = key;
    const auto typeName = (*declaration)["type"].value<std::string_view>();
    if (!typeName || !ParseType(*typeName, setting.type)) {
      return "type must be bool, int, float, string, path, enum or list";
    }
    std::set<std::string_view> allowed = {"type", "label", "description", "default", "value"};
    switch (setting.type) {
      case Type::Int:
        allowed.insert({"min", "max"});
        for (const char* bound : {"min", "max"}) {
          if (declaration->contains(bound) && !(*declaration)[bound].is_integer()) {
            return std::string(bound) + " must be an integer";
          }
        }
        break;
      case Type::Float:
        allowed.insert({"min", "max", "step"});
        for (const char* bound : {"min", "max", "step"}) {
          if (declaration->contains(bound) && !(*declaration)[bound].is_number()) {
            return std::string(bound) + " must be a number";
          }
        }
        break;
      case Type::Enum:
        allowed.insert({"values", "choices"});
        if (!IsStringArray(declaration->get("values")) || declaration->get("values")->as_array()->empty()) {
          return "values must be a non-empty list of strings";
        }
        if (std::string why = CheckChoices(declaration->get("choices"), *declaration->get("values")->as_array()); !why.empty()) {
          return why;
        }
        break;
      case Type::List: {
        allowed.insert("of");
        const auto of = (*declaration)["of"].value<std::string_view>();
        if (!of || (*of != "string" && *of != "path" && *of != "int")) {
          return "of must be string, path or int";
        }
        ParseType(*of, setting.of);
        if (setting.of != Type::Path) {
          break;
        }
      }
        [[fallthrough]];
      case Type::Path:
        allowed.insert({"kind", "filter"});
        if (const toml::node* kind = declaration->get("kind");
            kind && kind->value<std::string_view>() != "file" && kind->value<std::string_view>() != "directory") {
          return "kind must be \"file\" or \"directory\"";
        }
        if (declaration->contains("filter") && !IsStringArray(declaration->get("filter"))) {
          return "filter must be a list of strings";
        }
        break;
      default:
        break;
    }
    for (const auto& [name, unused] : *declaration) {
      if (!allowed.count(name.str())) {
        return "unknown key " + std::string(name.str());
      }
    }
    for (const char* text : {"label", "description"}) {
      if (declaration->contains(text) && !(*declaration)[text].is_string()) {
        return std::string(text) + " must be a string";
      }
    }
    const toml::node* fallback = declaration->get("default");
    if (!fallback) {
      return "default is missing";
    }
    if (std::string why = Accept(*fallback, *declaration, setting); !why.empty()) {
      return "default: " + why;
    }
    if (const toml::node* value = declaration->get("value")) {
      Setting chosen = setting;
      if (std::string why = Accept(*value, *declaration, chosen); !why.empty()) {
        Log("%s: setting %s: %s, using the default", plugin.c_str(), key.c_str(), why.c_str());
      } else {
        setting = std::move(chosen);
      }
    }
    return {};
  }

  // Why plugins\<file> is not a valid plugin file, or empty with `plugin` filled in.
  std::string ReadManifest(const std::string& file, Plugin& plugin) {
    toml::table root;
    try {
      root = toml::parse_file(g_directory + "plugins\\" + file);
    } catch (const toml::parse_error& error) {
      return "line " + std::to_string(error.source().begin.line) + ": " + std::string(error.description());
    }
    // "exe" is the [exe] table builds for host 1.5.0 wrote (relsigs checked at runtime): accepted and ignored.
    static const std::set<std::string_view> keys = {"name", "category", "version", "description", "conflicts", "libs", "dependencies", "settings", "exe"};
    for (const auto& [key, unused] : root) {
      if (!keys.count(key.str())) {
        return "unknown key " + std::string(key.str());
      }
    }
    const auto name = root["name"].value<std::string>();
    const auto category = root["category"].value<std::string>();
    const auto version = root["version"].value<std::string>();
    if (!name || name->empty() || !category || category->empty() || !version) {
      return "needs a name, a category and a version";
    }
    if (_stricmp((*name + ".toml").c_str(), file.c_str()) != 0) {
      return "names plugin " + *name + "; the file must be " + *name + ".toml";
    }
    plugin.name = *name;
    plugin.category = *category;
    if (!ParseVersion(*version, plugin.version)) {
      return "invalid version \"" + *version + "\"";
    }
    plugin.versionText = *version;
    if (root.contains("description") && !root["description"].is_string()) {
      return "description must be a string";
    }
    for (const char* list : {"conflicts", "libs"}) {
      if (root.contains(list) && !IsStringArray(root.get(list))) {
        return std::string(list) + " must be a list of strings";
      }
    }
    if (const toml::array* conflicts = root["conflicts"].as_array()) {
      for (const toml::node& entry : *conflicts) {
        plugin.conflicts.push_back(*entry.value<std::string>());
      }
    }
    if (root.contains("dependencies")) {
      const toml::table* dependencies = root["dependencies"].as_table();
      if (!dependencies) {
        return "dependencies must be a table";
      }
      for (const auto& [key, value] : *dependencies) {
        const auto text = value.value<std::string>();
        Dependency dependency{std::string(key.str()), text.value_or(""), {}};
        if (!text || !ParseRange(*text, dependency.range)) {
          return "dependency " + dependency.name + ": invalid range (expected normalised comparator sets)";
        }
        plugin.dependencies.push_back(std::move(dependency));
      }
    }
    if (root.contains("settings")) {
      const toml::table* settings = root["settings"].as_table();
      if (!settings) {
        return "settings must be a table";
      }
      for (const auto& [key, value] : *settings) {
        Setting setting;
        if (std::string why = ReadSetting(plugin.name, std::string(key.str()), value, setting); !why.empty()) {
          return "setting " + std::string(key.str()) + ": " + why;
        }
        plugin.settings.push_back(std::move(setting));
      }
    }
    return {};
  }

  // The setting `key` of `plugin` if it has one of `types` (and, for lists, an element type in `of`); else logs, null.
  const Setting* FindSetting(const char* plugin, const char* key, const char* call, std::initializer_list<Type> types,
                             std::initializer_list<Type> of = {}) {
    const auto found = plugin ? g_plugins.find(plugin) : g_plugins.end();
    if (found == g_plugins.end() || !key) {
      Log("%s(%s, %s): no such plugin or key", call, plugin ? plugin : "(null)", key ? key : "(null)");
      return nullptr;
    }
    for (const Setting& setting : found->second.settings) {
      if (setting.key == key) {
        const bool typed = std::find(types.begin(), types.end(), setting.type) != types.end() &&
                           (setting.type != Type::List || std::find(of.begin(), of.end(), setting.of) != of.end());
        if (!typed) {
          Log("%s: %s(%s): the setting has another type", plugin, call, key);
          return nullptr;
        }
        return &setting;
      }
    }
    Log("%s: %s(%s): no such setting", plugin, call, key);
    return nullptr;
  }

  bool SettingBool(const char* plugin, const char* key) {
    const Setting* setting = FindSetting(plugin, key, "SettingBool", {Type::Bool});
    return setting && setting->boolean;
  }

  long long SettingInt(const char* plugin, const char* key) {
    const Setting* setting = FindSetting(plugin, key, "SettingInt", {Type::Int});
    return setting ? setting->integer : 0;
  }

  double SettingFloat(const char* plugin, const char* key) {
    const Setting* setting = FindSetting(plugin, key, "SettingFloat", {Type::Float});
    return setting ? setting->number : 0;
  }

  const char* SettingString(const char* plugin, const char* key) {
    const Setting* setting = FindSetting(plugin, key, "SettingString", {Type::String, Type::Enum, Type::Path});
    return setting ? setting->text.c_str() : "";
  }

  int SettingCount(const char* plugin, const char* key) {
    const Setting* setting = FindSetting(plugin, key, "SettingCount", {Type::List}, {Type::String, Type::Path, Type::Int});
    return !setting ? 0 : static_cast<int>(setting->of == Type::Int ? setting->integers.size() : setting->items.size());
  }

  const char* SettingItem(const char* plugin, const char* key, int index) {
    const Setting* setting = FindSetting(plugin, key, "SettingItem", {Type::List}, {Type::String, Type::Path});
    if (setting && (index < 0 || static_cast<std::size_t>(index) >= setting->items.size())) {
      Log("%s: SettingItem(%s, %d): out of range", plugin, key, index);
      return "";
    }
    return setting ? setting->items[index].c_str() : "";
  }

  long long SettingItemInt(const char* plugin, const char* key, int index) {
    const Setting* setting = FindSetting(plugin, key, "SettingItemInt", {Type::List}, {Type::Int});
    if (setting && (index < 0 || static_cast<std::size_t>(index) >= setting->integers.size())) {
      Log("%s: SettingItemInt(%s, %d): out of range", plugin, key, index);
      return 0;
    }
    return setting ? setting->integers[index] : 0;
  }

  const RoHostApi g_api = {RoHostApiVersion, Resolve, Replace, Log, PluginVersion, PluginStatus, Hook, HookSystem,
                           SettingBool, SettingInt, SettingFloat, SettingString, SettingCount, SettingItem, SettingItemInt};

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

  // The names the DLL at `path` delay-imports from Ragexe.exe (scripts/build.py links plugins with
  // /DELAYLOAD:Ragexe.exe) that the running exe does not export. Read from the file, so no plugin code runs; a file
  // that cannot be parsed yields nothing and LoadLibrary reports the error.
  std::vector<std::string> MissingExeImports(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    const std::string image((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto at = [&image](std::size_t offset, std::size_t size) {
      return offset <= image.size() && size <= image.size() - offset ? image.data() + offset : nullptr;
    };
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(at(0, sizeof(IMAGE_DOS_HEADER)));
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
      return {};
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(at(dos->e_lfanew, sizeof(IMAGE_NT_HEADERS32)));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) {
      return {};
    }
    const std::size_t sectionTable = dos->e_lfanew + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        at(sectionTable, std::size_t{nt->FileHeader.NumberOfSections} * sizeof(IMAGE_SECTION_HEADER)));
    if (!sections) {
      return {};
    }
    // File offset of an RVA, or 0 (never a valid target here: it is the DOS header).
    const auto offsetOf = [&](std::uint32_t rva) -> std::size_t {
      for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER& section = sections[i];
        if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.SizeOfRawData) {
          return section.PointerToRawData + (rva - section.VirtualAddress);
        }
      }
      return 0;
    };
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    std::vector<std::string> missing;
    for (std::size_t offset = offsetOf(directory.VirtualAddress); offset;) {
      const auto* descriptor = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(at(offset, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)));
      if (!descriptor || !descriptor->DllNameRVA) {
        break;
      }
      offset += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
      // Old-style descriptors hold VAs instead of RVAs.
      const std::uint32_t bias = descriptor->Attributes.RvaBased ? 0 : nt->OptionalHeader.ImageBase;
      const char* dll = at(offsetOf(descriptor->DllNameRVA - bias), 1);
      if (!dll || _strnicmp(dll, "Ragexe.exe", 11) != 0) {
        continue;
      }
      for (std::size_t thunk = offsetOf(descriptor->ImportNameTableRVA - bias); thunk; thunk += sizeof(IMAGE_THUNK_DATA32)) {
        const auto* entry = reinterpret_cast<const IMAGE_THUNK_DATA32*>(at(thunk, sizeof(IMAGE_THUNK_DATA32)));
        if (!entry || !entry->u1.AddressOfData) {
          break;
        }
        if (IMAGE_SNAP_BY_ORDINAL32(entry->u1.Ordinal)) {
          continue;
        }
        const std::size_t name = offsetOf(entry->u1.AddressOfData - bias);
        const char* text = name ? at(name + offsetof(IMAGE_IMPORT_BY_NAME, Name), 1) : nullptr;
        if (!text) {
          break;
        }
        const std::string function(text, strnlen(text, image.size() - (text - image.data())));
        if (!GetProcAddress(g_exe, function.c_str())) {
          missing.push_back(function);
        }
      }
    }
    return missing;
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
    // Binding the exe imports fails inside LoadLibrary with a bare error 1114; name the function instead.
    if (const std::vector<std::string> missing = MissingExeImports(path); !missing.empty()) {
      std::string more = missing.size() > 1 ? " and " + std::to_string(missing.size() - 1) + " more" : "";
      Fail(plugin, "failed-load", "built for a different %s: missing %s%s", g_exeName.c_str(), missing[0].c_str(), more.c_str());
      for (const std::string& function : missing) {
        Log("%s: %s does not export %s", plugin.name.c_str(), g_exeName.c_str(), function.c_str());
      }
      WriteStatus();
      return;
    }
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
  for (const std::string& file : List("*.toml")) {
    Plugin plugin;
    if (const std::string why = ReadManifest(file, plugin); !why.empty()) {
      Log("%s: invalid, %s", file.c_str(), why.c_str());
      continue;
    }
    manifests.insert(Lower(plugin.name));
    g_plugins[plugin.name] = std::move(plugin);
  }
  std::set<std::string> dlls;
  for (const std::string& file : List("*.dll")) {
    const std::string stem = file.substr(0, file.size() - 4);
    dlls.insert(Lower(stem));
    if (!manifests.count(Lower(stem))) {
      Log("%s: refused, no valid %s.toml manifest next to it", file.c_str(), stem.c_str());
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
