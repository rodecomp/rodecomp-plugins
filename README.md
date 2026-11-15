# rodecomp-plugins

Plugins for the decompiled Ragnarok Online client ([rodecomp](https://github.com/rodecomp/rodecomp)): a plugin
loader patched into the rebuilt exe, a host DLL that resolves plugin dependencies, an SDK, and plugins such as a
Direct3D 12 renderer.

## Layout

| Path | What |
| --- | --- |
| `rodecomp/` | upstream decompilation (git submodule). Read-only here: never edit it. |
| `patches/NNNN_*.py` | patches applied to a copy of rodecomp's exe; `0100_plugin_loader.py` adds the entry stub that loads `RoPluginHost.dll` and exports every exe function. |
| `Plugins/Host/` | `RoPluginHost.dll`: reads manifests, resolves dependencies, loads plugins, writes `plugins/status.txt`. |
| `Plugins/SDK/` | `RoPlugin.h` (the `RoHostApi`, file formats) and `RoPluginDelayLoad.cpp` (compiled into every plugin). |
| `Plugins/<Name>/` | one plugin: `plugin.json` plus `*.cpp`. `RendererDX12` and `DX12Effects` are the examples. |
| `scripts/build.py` | builds the exe, the host and every plugin into `build/`, and installs them into a client. |
| `scripts/plugins.py` | plugin manager for an installed client. |
| `scripts/semver.py` | semver versions and npm-style ranges, shared by both scripts. |

## Build

```sh
python scripts/build.py --toolchain <toolchain dir>                    # everything into build/
python scripts/build.py --no-exe ...                                   # reuse rodecomp/build/exe
python scripts/build.py --plugin DX12Effects ...                       # one plugin and its dependencies
python scripts/build.py --install C:/Games/Ragnarok ...
python scripts/build.py --install <client> --exe-name Ragexe.plugins.exe ...
```

`--install` copies the patched exe (as `Ragexe.exe` by default), `RoPluginHost.dll` and `plugins/<Name>.dll/.pdb/.plugin`.
The exe can have any file name: `--exe-name Ragexe.plugins.exe` installs it next to your own `Ragexe.exe`
without overwriting it. Plugins link `Ragexe.lib` with `/DELAYLOAD:Ragexe.exe`, and the SDK's
`RoPluginDelayLoad.cpp` has a delay-load hook (`__pfnDliNotifyHook2`) that binds that import to the running exe
(`GetModuleHandleA(nullptr)`) while the DLL loads, before `RoPlugin_Init`. Exe globals are absolute symbols at
their fixed address, so the exe name does not matter for them.

## plugin.json

```json
{"name": "DX12Effects", "version": "0.1.0", "description": "Screen-edge pulse.",
 "dependencies": {"RendererDX12": "^1.0.0"}, "libs": ["winmm.lib"]}
```

- `name` equals the directory name.
- `version` is semver: `MAJOR.MINOR.PATCH[-prerelease]`.
- `dependencies` map plugin names to npm-style ranges: `1.2.3`, `^1.2.3`, `~1.2`, `>=1.0.0 <2.0.0`, `*`, `a || b`.
  The build fails on a missing, out-of-range or cyclic dependency and builds in dependency order.
- `libs` are extra import libraries.

The build writes `<Name>.plugin` next to each DLL with normalised ranges. The host reads this file:

```
name DX12Effects
version 0.1.0
depends RendererDX12 >=1.0.0 <2.0.0-0
```

At start the host refuses a DLL with no `.plugin` file. It disables a plugin whose dependency is missing, out of
range, in a cycle, disabled or turned off, logs why to `RoPluginHost.log`, then loads the rest in dependency order.

## Plugin manager

`<client>/plugins/disabled.txt` holds one plugin name per line, with `#` comments. A listed plugin is `off` and is
not loaded. Its dependents are `disabled` with the reason `needs X, which is turned off`.

`<client>/plugins/status.txt` is written by the host atomically (a temp file, then `MoveFileEx`) after the load
pass and after every `RoPlugin_Init`. It is tab-separated:

```
RoPluginHost  1.1.0  api 3  pid 52768  started 2026-11-15T21:57:25  exe Ragexe.exe
DX12Effects   0.1.0  loaded        <reason>  RendererDX12=1.0.0
RendererDX12  1.0.0  loaded        <reason>
```

The state is one of `loaded`, `disabled`, `failed-init`, `failed-load`, `refused` (no manifest), `off` (in
disabled.txt) or `pending` (not initialised yet). The reason uses the same wording as the log. Plugins can read the
same text in game through `RoHostApi::PluginStatus` (API v3).

`scripts/plugins.py` (stdlib only; `--client DIR`, default `C:/Games/Ragnarok`):

| Command | Does |
| --- | --- |
| `list` | Shows each plugin's name, version, dependencies, the state recorded in status.txt, and the state predicted offline with the host's own resolution rules. Flags a mismatch, and says whether status.txt is live or stale (files changed since it was written, or the game exited). |
| `info NAME` | Shows the manifest, files, dependents, disabled.txt entry, the recorded and predicted state with the reason, and the resolved dependency versions. |
| `disable NAME` | Adds NAME to disabled.txt and warns about the dependents that will be disabled. |
| `enable NAME` | Removes NAME and the dependencies it needs from disabled.txt. |
| `install [NAME...]` | Copies from `build/plugins` (`--build DIR`) and also installs missing dependencies. Refuses an unsatisfied dependency unless `--force`. |
| `uninstall NAME` | Removes the .dll, .pdb and .plugin files. Refuses when other plugins depend on NAME; `--force` lists them and removes them too. |

Mutating commands refuse while the game runs from the client dir. That means the exe named in status.txt or any
`Ragexe*.exe` there, because the game holds the DLLs and reads disabled.txt only at start. Exit codes: `0` ok, `1`
refused or failed, `2` usage error, `3` game running.

```
$ python scripts/plugins.py list
host 1.1.0, api 3, exe Ragexe.exe, pid 32060, started 2026-11-15T21:58:00
status.txt: live: written by Ragexe.exe pid 32060, still running
NAME          VERSION  DEPENDS                        RECORDED  PREDICTED  REASON
DX12Effects   0.1.0    RendererDX12 >=1.0.0 <2.0.0-0  disabled  disabled   needs RendererDX12, which is turned off
RendererDX12  1.0.0    -                              off       off        turned off in plugins\disabled.txt
```

## Writing a plugin

1. Create `Plugins/<Name>/plugin.json` and one or more `.cpp` files. Include `RoPlugin.h` and any `rodecomp/Source`
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
   - `Replace(target, replacement, plugin)` patches an exe function with a jmp. It refuses a function shared by
     `/OPT:ICF`, one too small to patch, or one already replaced.
   - `Resolve(decorated)` returns the address of an exe symbol by its map name.
   - `Log` writes to the log; `PluginVersion(name)` (v2) returns the version of a loaded plugin; `PluginStatus()`
     (v3) returns the status.txt text.
4. Depend on another plugin's API. `DX12Effects` declares `"RendererDX12": "^1.0.0"` and includes
   `RendererDX12Api.h`. The build compiles RendererDX12 with `RENDERERDX12_EXPORTS`, so its `RENDERERDX12_API`
   functions are `dllexport`. DX12Effects gets `/I Plugins/RendererDX12` and links `RendererDX12.lib`, so it calls
   `RendererDX12_AddFrameCallback(DrawPulse, nullptr)` directly. At run time the host loads RendererDX12 first and
   skips DX12Effects if RendererDX12 is not loaded.

## Commits

Commit as `ragnarok-clanker <ragnarok-clanker@rodecomp.online>` only. The repo config sets that identity and
`.git/hooks/pre-commit` refuses any other author or committer.
