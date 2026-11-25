# rodecomp-plugins

Mods for Ragnarok Online, built on top of [rodecomp](https://github.com/rodecomp/rodecomp), a
from-scratch rebuild of the game client's source code.

Because the client is rebuilt from source, plugins don't need to guess at memory addresses or patch
bytes: they call and hook the game's own functions by name, like any other C++ code.

## What's in here

About 80 plugins, sorted by category: camera zoom, chat tweaks, damage numbers, health bars, custom
data folders, a Direct3D 12 renderer, and plenty more. Browse [`Plugins/`](Plugins) to see them all.

Behind them:

- **a plugin loader** baked into the rebuilt game exe,
- **a host DLL** that loads plugins, checks their dependencies and reports what loaded,
- **an SDK** (`Plugins/SDK/RoPlugin.h`) for writing your own,
- **`ro-plugins`**, a small manager app (window + command line) to turn plugins on and off and tweak
  their settings.

## Trying it out

You need Python 3, the MSVC toolchain used by rodecomp, and a Ragnarok Online client folder.

```sh
git clone --recursive https://github.com/rodecomp/rodecomp-plugins.git
cd rodecomp-plugins
python scripts/build.py --toolchain <toolchain dir> --install C:/Games/Ragnarok
```

That builds the game, the host and every plugin, then installs them into your client.
Add `--exe-name Ragexe.plugins.exe` if you'd rather keep your original `Ragexe.exe` untouched.

Then use the manager to pick what you want:

```sh
cd tools/plugin-manager
cargo run --release -p ro-plugins
```

## Writing a plugin

A plugin is a folder `Plugins/<Category>/<Name>/` with a `plugin.toml` and some `.cpp` files.
The smallest useful one hooks a game function:

```cpp
#include "RoPlugin.h"

void (CGameMode::*g_original)();
struct CameraHook : CGameMode {
  void InitializeCamera() { (this->*g_original)(); /* then adjust the camera */ }
};

RO_PLUGIN_EXPORT bool RoPlugin_Init(const RoHostApi* host) {
  return ro::Hook(host, &CGameMode::InitializeCamera, &CameraHook::InitializeCamera, g_original, "Zoom");
}
```

Copy an existing plugin from the same category as a starting point, and read [AGENTS.md](AGENTS.md)
for the do's and don'ts.

## Learn more

- [docs/reference.md](docs/reference.md): the full reference (build options, `plugin.toml` format,
  the host API, how changed game functions are detected).
- [tools/plugin-manager/README.md](tools/plugin-manager/README.md): the manager app.
- Come say hi on [Discord](https://clank.rodecomp.online/discord).
