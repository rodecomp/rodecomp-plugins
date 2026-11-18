# rodecomp-plugins

Plugins for the decompiled Ragnarok Online client ([rodecomp](https://github.com/rodecomp/rodecomp)): a plugin
loader patched into the rebuilt exe, a host DLL that resolves plugin dependencies, an SDK, and plugins such as a
Direct3D 12 renderer.

## Layout

| Path | What |
| --- | --- |
| `rodecomp/` | upstream decompilation (git submodule). Read-only here: never edit it. |
| `loader/RoLoaderEntry.cpp` | the plugin exe's entry point: loads `RoPluginHost.dll`, then starts the CRT. `scripts/plugin_exe.py` links it with rodecomp's objects, an export table and a fixed base. |
| `Plugins/Host/` | `RoPluginHost.dll`: reads manifests, resolves dependencies, loads plugins, writes `plugins/status.txt`. |
| `Plugins/SDK/` | `RoPlugin.h` (the `RoHostApi`, file formats) and `RoPluginDelayLoad.cpp` (compiled into every plugin). |
| `Plugins/<Category>/<Name>/` | one plugin: `plugin.toml` plus `*.cpp`. Every plugin is in a category. |
| `scripts/build.py` | builds the exe, the host and every plugin into `build/`, and installs them into a client. |
| `tools/plugin-manager/` | `ro-plugins`: plugin manager for an installed client, a GUI and a CLI (Rust; see its README). |
| `scripts/semver.py` | semver versions and npm-style ranges for the build (ported to the manager's core crate). |

## Build

```sh
python scripts/build.py --toolchain <toolchain dir>                    # everything into build/
python scripts/build.py --no-exe ...                                   # reuse rodecomp/build/exe
python scripts/build.py --plugin RendererDX12 ...                      # one plugin and its dependencies
python scripts/build.py --install C:/Games/Ragnarok ...
python scripts/build.py --install <client> --exe-name Ragexe.plugins.exe ...
```

`--install` copies the patched exe (as `Ragexe.exe` by default), `RoPluginHost.dll`, `plugins/<Name>.dll/.pdb` and
`plugins/<Name>.toml` (merged with the installed copy, see below), and deletes a leftover `plugins/<Name>.plugin`.
The exe can have any file name: `--exe-name Ragexe.plugins.exe` installs it next to your own `Ragexe.exe`
without overwriting it. Plugins link `Ragexe.lib` with `/DELAYLOAD:Ragexe.exe`, and the SDK's
`RoPluginDelayLoad.cpp` has a delay-load hook (`__pfnDliNotifyHook2`) that binds that import to the running exe
(`GetModuleHandleA(nullptr)`) while the DLL loads, before `RoPlugin_Init`. Exe globals are absolute symbols at
their fixed address, so the exe name does not matter for them.

## plugin.toml

One TOML file per plugin holds the manifest, the setting declarations and, once installed, the user's values
(full rules: `build/plugin-toml-spec.md`).

```toml
name = "MyEffects"
category = "Graphics"
version = "0.1.0"
description = "Extra Direct3D 12 effects."
conflicts = ["OtherEffects"]
libs = ["d3d12.lib"]

[dependencies]
RendererDX12 = "^1.1.0"

[settings.Strength]
type = "float"
label = "Effect strength"
description = "0 turns the effect off."
default = 0.5
min = 0.0
max = 1.0
```

- `name` equals the directory name, and is unique across categories.
- `category` is required and equals the parent directory name (`Plugins/<Category>/<Name>/`). A `plugin.toml`
  directly under `Plugins/` fails the build.
- `version` is semver: `MAJOR.MINOR.PATCH[-prerelease]`.
- `dependencies` map plugin names to npm-style ranges: `1.2.3`, `^1.2.3`, `~1.2`, `>=1.0.0 <2.0.0`, `*`, `a || b`.
  The build fails on a missing, out-of-range or cyclic dependency and builds in dependency order.
- `conflicts` (optional) lists plugins that must not run with this one, e.g. two camera-angle presets. One side
  listing it is enough.
- `libs` are extra import libraries. Any `*.rc` in the plugin directory is compiled with rc.exe (the toolchain's,
  else the newest installed Windows Kits x86 one) and its `.res` is linked into the DLL, e.g. an icon.
- `[settings.<Key>]` tables declare user settings, in display order. `type` is `bool`, `int` (optional `min`,
  `max`), `float` (optional `min`, `max`, `step`), `string`, `path` (optional `kind = "file" | "directory"`,
  `filter = ["*.ico"]`; relative to the client folder), `enum` (required `values`) or `list` (required
  `of = "string" | "path" | "int"`). `default` is required and must be valid; `label` and `description` are optional.
  Any other key fails the build. The source never has `value` keys.

The build writes `build/plugins/<Name>.toml`, the same file with every range normalised to comparator sets
(`RendererDX12 = ">=1.1.0 <2.0.0-0"`). The user sets a setting by adding `value = ...` under its `[settings.<Key>]`
in the installed `<client>/plugins/<Name>.toml` (or through the plugin manager). `--install` (and the manager's
install) merge: the new file is the build's, plus each old `value` that is valid under the new declaration; values
whose setting is gone or no longer validates are dropped and printed. build.py regenerates the file, so comments in
it are lost.

At start the host reads every `plugins/*.toml` and refuses a DLL with no valid `.toml` (unparsable TOML, an unknown
key, a missing `name`/`category`/`version`, a bad range or setting declaration, or `name` not the file stem), logging
the reason. An invalid `value` (wrong type, out of `min`/`max`, not in `values`, an ill-typed list element) is logged
once as `<Name>: setting <Key>: <why>, using the default` and the default is used. It disables a plugin whose
dependency is missing, out of range, in a cycle, disabled or turned off, logs why to `RoPluginHost.log`, then loads
the rest in dependency order. Two enabled plugins in conflict: the one first by name loads, the other is `disabled`
with `conflicts with X`, and its dependents with it. A plugin turned off in `disabled.txt` conflicts with nothing.

## Plugin manager

`<client>/plugins/disabled.txt` holds one plugin name per line, with `#` comments. A listed plugin is `off` and is
not loaded. Its dependents are `disabled` with the reason `needs X, which is turned off`.

`<client>/plugins/status.txt` is written by the host atomically (a temp file, then `MoveFileEx`) after the load
pass and after every `RoPlugin_Init`. It is tab-separated:

```
RoPluginHost  1.4.0  api 6  pid 52768  started 2026-11-15T21:57:25  exe Ragexe.exe
RendererDX12  1.1.0  loaded        <reason>
MyEffects     0.1.0  loaded        <reason>  RendererDX12=1.1.0
```

The state is one of `loaded`, `disabled`, `failed-init`, `failed-load`, `refused` (no manifest), `off` (in
disabled.txt) or `pending` (not initialised yet). The reason uses the same wording as the log. Plugins can read the
same text in game through `RoHostApi::PluginStatus` (API v3).

`ro-plugins` (in `tools/plugin-manager/`, details in its [README](tools/plugin-manager/README.md)) is one binary:
without a command it opens the window, with one it is the CLI.

```sh
cd tools/plugin-manager
cargo run --release -p ro-plugins                       # the GUI
cargo run --release -p ro-plugins -- list               # the CLI
target/release/ro-plugins list|info|enable|disable|install|uninstall [--client DIR] [--build DIR] [--force]
```

`--client DIR` defaults to `C:/Games/Ragnarok`; `--build DIR` defaults to `<repo>/build`
(its `plugins/` directory, the scripts/build.py output).

| Command | Does |
| --- | --- |
| `list` | Shows each plugin's name, version, dependencies, the state recorded in status.txt, and the state predicted offline with the host's own resolution rules. Flags a mismatch, and says whether status.txt is live or stale (files changed since it was written, or the game exited). |
| `info NAME` | Shows the manifest, files, dependents, disabled.txt entry, the recorded and predicted state with the reason, and the resolved dependency versions. |
| `disable NAME` | Adds NAME to disabled.txt and warns about the dependents that will be disabled. |
| `enable NAME` | Removes NAME and the dependencies it needs from disabled.txt. |
| `install [NAME...]` | Copies from the build output and also installs missing dependencies. Refuses an unsatisfied dependency unless `--force`. |
| `uninstall NAME` | Removes the .dll, .pdb and .toml files (and a leftover .plugin). Refuses when other plugins depend on NAME; `--force` lists them and removes them too. |

Mutating commands refuse while the game runs from the client dir. That means any process whose image is in that
folder and is the exe named in status.txt or any `*.exe`, because the game holds the DLLs and reads disabled.txt
only at start. Exit codes: `0` ok, `1` refused or failed, `2` usage error, `3` game running.

```
$ ro-plugins list
host 1.4.0, api 6, exe Ragexe.exe, pid 32060, started 2026-11-15T21:58:00
status.txt: live: written by Ragexe.exe pid 32060, still running
NAME          VERSION  DEPENDS                        RECORDED  PREDICTED  REASON
MyEffects     0.1.0    RendererDX12 >=1.1.0 <2.0.0-0  disabled  disabled   needs RendererDX12, which is turned off
RendererDX12  1.1.0    -                              off       off        turned off in plugins\disabled.txt
```

## Writing a plugin

1. Create `Plugins/<Category>/<Name>/plugin.toml` and one or more `.cpp` files. Include `RoPlugin.h` and any `rodecomp/Source`
   headers: exe functions and globals are callable as if they were your own (`Ragexe.lib` + `RagexeGlobals.obj`).
2. Export the entry point:

   ```cpp
   RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
     return host->Replace(ro::Address(&RenderSystemHolder::Initialize),
                          ro::Address(&RenderSystemHolderDX12::Initialize), "RendererDX12");
   }
   ```

   It runs from the exe's entry point, before the CRT and the exe's static constructors. Install replacements
   there and touch no exe global. Returning `false` disables the plugin and its dependents.
3. `RoHostApi` (check `host->version` before using a field added later):
   - `Hook(target, detour, &original, plugin)` (v4) chains `detour` in front of an exe function. Several plugins
     may hook one function: the last one hooked (load order) runs first, and each calls `original` to continue
     to the previous detour and finally the original code. A detour that skips `original` skips everything behind
     it. The host builds the trampoline with [MinHook](Plugins/Host/minhook/VERSION.txt) and patches the exe once
     per function. Prefer it to `Replace`.
     In C++ use the typed helpers of `RoPlugin.h`, which stop compiling when rodecomp changes a hooked signature:
     ```cpp
     void (CGameMode::*g_original)();
     struct CameraHook : CGameMode { void InitializeCamera() { (this->*g_original)(); /* then adjust the camera */ } };
     ro::Hook(host, &CGameMode::InitializeCamera, &CameraHook::InitializeCamera, g_original, "Zoom");
     ```
     `ro::HookVirtual` does the same for a virtual function, found by its decorated name.
   - `HookSystem(module, function, detour, &original, plugin)` (v5, typed as `ro::HookSystem`) hooks a function a
     system DLL exports. It is the exception, for behaviour the exe inlines where no exe hook reaches it (e.g.
     `AllowMultipleInstances` on `kernel32!CreateMutexA`); it sees every caller, so the detour filters.
   - `Replace(target, replacement, plugin)` patches an exe function with a jmp; the original never runs. Both
     refuse a function shared by `/OPT:ICF` or too small to patch, and a function can be replaced once or hooked,
     not both.
   - `Resolve(decorated)` returns the address of an exe symbol by its map name.
   - `Log` writes to the log; `PluginVersion(name)` (v2) returns the version of a loaded plugin; `PluginStatus()`
     (v3) returns the status.txt text.
   - `SettingBool`, `SettingInt`, `SettingFloat`, `SettingString`, `SettingCount`, `SettingItem`, `SettingItemInt`
     (v6) return a setting of `plugins/<plugin>.toml` (`value`, else `default`), read before any plugin loads, so
     they work in `RoPlugin_Init`; strings stay valid for the process. `SettingString` serves string, enum and path
     (absolute); `SettingCount`/`SettingItem`/`SettingItemInt` serve lists. A wrong key or type logs and returns the
     zero value (`""` for text). E.g. `WindowFrame` reads `host->SettingString(kName, "Title")` after
     checking `host->version >= 6`.
4. Depend on another plugin's API. A plugin `MyEffects` declares `RendererDX12 = "^1.1.0"` and includes
   `RendererDX12Api.h`. The build compiles RendererDX12 with `RENDERERDX12_EXPORTS`, so its `RENDERERDX12_API`
   functions are `dllexport`. MyEffects gets `/I Plugins/RendererDX12` and links `RendererDX12.lib`, so it calls
   `RendererDX12_AddFrameCallback(Draw, nullptr)` directly to draw each frame on the scene. At run time the host
   loads RendererDX12 first and skips MyEffects if RendererDX12 is not loaded.

## Commits

Commit as `ragnarok-clanker <ragnarok-clanker@rodecomp.online>` only. The repo config sets that identity and
`.git/hooks/pre-commit` refuses any other author or committer.
