# Writing rodecomp plugins

Rules and pitfalls for agents (and people) adding or changing a plugin. `docs/reference.md` documents the formats and
commands in full; this file says how to use them well. Read `Plugins/SDK/RoPlugin.h` before writing code.

## What you are building on

- `rodecomp/` (submodule, **read-only**) is a matching decompilation of Gravity's **unpatched** Ragexe 2026-02-19.
  Its exe behaves exactly like Gravity's: launcher check, kRO servers, no clientinfo.xml, no plugin loader.
- `scripts/build.py` relinks rodecomp's objects into the **plugin exe** (`build/exe/Ragexe.exe`): same game code
  plus `RoLoaderEntry` (loads `RoPluginHost.dll` first), an export table and a fixed base. Every behaviour change a
  player gets (clientinfo.xml, no launcher argument, WARP-style tweaks) is a **plugin**; never a byte patch.
- The exe, `RoPluginHost.dll` and the plugins are **one matched set**. A plugin binds to exe functions by name: run
  it only with the exe it was built against. Ship and install them together (`build.py --install`).

## Build and check

```sh
python scripts/build.py --no-exe --toolchain <dir>                  # host + all plugins (exe objects already built)
python scripts/build.py --no-exe --plugin <Name> --toolchain <dir>  # one plugin and its dependencies
python scripts/build.py --no-exe --install <client> --exe-name Ragexe.exe --toolchain <dir>
```

- After a rodecomp change (`td compile`, submodule bump), drop `--no-exe` once so the plugin exe is relinked.
- **One build at a time.** Every build relinks the plugin exe and copies `Ragexe.lib`; parallel builds fail on
  locked files.
- Then run the client and read `<client>/RoPluginHost.log` and `plugins/status.txt`: your plugin must be `loaded`,
  with one `hooked …` line per hook and no `failed`/`refused` line.

## Hooks

- **Prefer `ro::Hook`** (typed, chained: several plugins can hook one function, each calls `original`). Use
  `ro::Replace` only when the original must never run; a function can be hooked or replaced, not both.
- **Detours are members of a derived struct**, so a signature change in rodecomp stops the build:
  `struct Hook : CGameMode { void InitializeCamera() { (this->*g_original)(); … } };`.
- **Virtual functions** go through `ro::HookVirtual(host, "?Name@Class@@…", …)` by decorated name. Take the name
  from `build/exe/Ragexe.map`; the build records it in `relsig.lock` like any other use.
- **The function must be in the plugin exe.** The host refuses functions that are:
  - **inlined** (no exported symbol): hook the caller instead;
  - **folded by `/OPT:ICF`** (shared with other functions): hook every caller, or one level up;
  - **dropped by `/OPT:REF`** because nothing calls them in the original (e.g. `ReadClientInfo`,
    `UITextureButton::OnLBtnUp`): you cannot call or hook them. Reimplement the logic in the plugin.
  Check with `grep <name> build/exe/Ragexe.def` before designing around a function.
- **Hook the original logic.** rodecomp follows Gravity's unpatched exe; don't assume behaviour a WARP or kit patch
  added. Read the current source of the function you hook, not an old description.
- **`ro::HookSystem` is the exception**, for behaviour the exe inlines into many sites (registry calls, `strstr`,
  `CreateWindowExA`, `Sleep`). It sees every caller in the process: filter on `_ReturnAddress()` inside the exe, and
  on the exact argument (window class, needle) so other callers pass through untouched.
- **Scope a hook in time** with a flag set by an outer hook (e.g. set in `ShowMapName`, read in
  `TextContext::SetFont`) instead of matching strings or guessing callers.
- **Don't keep big detours alive for nothing**: install a hook only when its setting changes something, and log
  `"%s: … off, nothing to do"` otherwise.

## Exe data

- **Exe globals are linked as absolute symbols** at their fixed address (`RagexeGlobals.obj`): declare them as in
  rodecomp's headers and use them directly. Never cache a pointer to one in `RoPlugin_Init`.
- **`RoPlugin_Init` runs before the CRT and the exe's static constructors.** Install hooks and read settings there;
  don't touch exe globals, call exe code or allocate exe objects. Do that from a detour.
- **Private and protected members are yours.** Plugins compile with every member public (`RoPluginAccess.h`,
  force-included), so a detour uses them like the class's own code: `this->m_charset = HANGUL_CHARSET;`, or calls a
  private method. Never `reinterpret_cast` to an offset: a named member fails to compile when rodecomp changes it.
  - Include the member's type's header if it's only forward-declared (e.g. `UIBitmapButton.h`).
  - A standard, Windows or third-party header that isn't already in `RoPluginAccess.h` must be added there, before
    its `#define`s, or its classes get public-form names and the link fails. Headers that redefine names (DirectX 7's
    `d3d.h`) or define macros (`small`) are the exceptions: see the comments there.
  - Members declared before any access label in a `class` stay private (two today:
    `CGameObject::s_nextIdentity`, `NObject::s_nextObjectId`): reach them through a function of the class.
- **Header-inline functions with a `static` local** get their own copy in your DLL (an empty one). Call the exe's
  copy through `host->Resolve("?GetRenderSystemHolder@@…")` instead (see `WindowFrame`).
- **Objects the exe owns** (windows, actors) are created and destroyed by the exe: don't `delete` them, don't keep
  pointers across frames unless the exe does.

## Bases and dependencies

- **One owner per hook point.** If a base already hooks a function, use its API, not a second hook:
  `LuaFiles` (`LuaScript::Load`: replace, load-after, ignore failure), `TextDrawHooks` (`UIWindow::DrawTextAt`:
  rewrite or skip a draw), `ClientInfo` (`ClientInfo_LoginServer`), `RendererDX12` (frame callbacks).
- **Make a base** only when two or more plugins need the same hook point, shared state or helper. Name it by what it
  provides. It exports a C API from `<Name>Api.h` (`<NAME>_API`, `<NAME>_EXPORTS`); dependents declare
  `<Name> = "^x.y.z"` and call it directly.
- **Order is decided by dependencies, nothing else.** If your plugin must run after another, depend on it. Note that
  a dependency loads first, so its hook ends up **behind** yours in a chain: to see what it did, ask its API rather
  than relying on chain order (see `NetworkTweaks` → `ClientInfo`).
- **Mutually exclusive plugins** declare `conflicts`. Prefer one plugin with an `enum` setting over several presets.
- **Dependencies are all-or-nothing** (not per setting). Adding a required dependency is a **major** version bump.

## plugin.toml

- `name` = folder, `category` = parent folder; categories: Audio, Camera, Character, Chat, Data, Graphics,
  Interface, Login, Network, System, Window. A category with one plugin is a sign the plugin is the category.
- **Players read `description`, `label` and setting descriptions** in the manager: plain English, what it does, the
  game's own value. No patch names (WARP, kit), no implementation details. Code comments may name origins.
- **Defaults keep the game's behaviour**, unless the change is what nearly everyone wants (a setting that's "on" by
  default must say so in its description).
- **Enums explain each choice in `[settings.<Key>.choices]`**: `<value> = { label = "…", description = "…" }`, a
  short player label and one line on what that choice does. The manager shows every choice with its text side by
  side, so the setting's `description` is only context (or absent), never a list of values. `values` stay the stored
  names: renaming one drops the users' saved value, changing a label does not.
- **A setting that only matters for some values of an enum gets `when`**: `when = { Mode = "Custom" }` or
  `when = { Mode = ["Beep", "Icon"] }` (one key, an enum setting of the same plugin). The manager hides it otherwise
  and shows it right under that enum, so don't open its description with "With X:".
- **Semver:** patch for fixes, minor for new settings/API, major for breaking API, removed settings or new required
  dependencies. Bump every plugin you change.
- **Settings are read in `RoPlugin_Init`** (`host->version >= 6`), never per frame from the file.

## relsig.lock

- Each plugin has a committed `relsig.lock`, **written by the build, never by hand**. It records a signature of every
  exe function the plugin uses (symbols it links against and `"?…"` names it passes to `HookVirtual`/`Resolve`), a
  hash of the plugin's sources (with its headers, the SDK's and its dependencies'), and a signature of every function
  the plugin compiles.
- A build prints `warning: <Plugin>: exe function changed|missing …` when rodecomp changed one of them. Then **review
  that function's diff in rodecomp** and re-check your hook's assumptions (arguments, offsets, what you skip).
- It prints `warning: <Plugin>: compiles differently with its sources unchanged …` when a rodecomp header changed what
  the plugin compiles to: a message ID, enum value, field offset or inline function moved. Check what the named
  function uses from rodecomp's headers and that it still means what the plugin intends.
- The build rewrites the lock; commit it with the review. `--strict` fails instead (CI).
- New plugin: the first build creates its lock. Commit it.

## Code

- C++17/20 with MSVC 14.29 x86, rodecomp's compile flags. `#include` rodecomp headers by their `Source`-relative path
  (`"Client/Mode/CGameMode.h"`); third-party code by relative path (`"../ThirdParty/…"`).
- One header comment at the top of `Plugin.cpp`: per setting, which functions are hooked and why. Comments explain
  intent and evidence, not history.
- `kName` constant equal to the plugin name, used for every `Log` and setting call.
- Log what the user will need when something goes wrong (what was applied, why a feature is unavailable), once at
  start, not per frame.
- LF line endings. No new `#pragma comment`, no inline assembly, no byte patching, no `VirtualProtect`.

## Verify before you report

1. Builds with no errors and no relsig warning you didn't review.
2. In the client: `loaded`, every expected `hooked` line, no warnings in `RoPluginHost.log`.
3. The behaviour, in game when possible; otherwise state exactly what you could not check and how the user checks it.
4. With the setting at its default the game behaves as before.

## Commits

Include the plugin's `relsig.lock`. Never commit `build/`. Never edit `rodecomp/`: changes to the game's source go
through rodecomp's own process (`td`), then the submodule moves here.
