# ro-plugins

The plugin manager for a rodecomp client directory. It is one native binary that works as both a GUI and a CLI. It
replaces `scripts/plugins.py`: the commands, output and exit codes are the same.

```sh
cargo run --release -p ro-plugins                        # opens the window
cargo run --release -p ro-plugins -- gui --client DIR    # the window, on another client
cargo run --release -p ro-plugins -- list                # the CLI
cargo test                                               # semver, resolver, status.txt, mutations
```

The release binary is `target/release/ro-plugins(.exe)`. Use the stable toolchain.

## Layout

| Path | What |
| --- | --- |
| `crates/ro-plugins-core/` | Library with no UI. `semver` is a port of scripts/semver.py: versions, npm-style ranges, the `-0` floors. `manifest` parses `<Name>.plugin`, `disabled.txt` and `Plugins/<Category>/<Name>/plugin.json`. `resolve` predicts states the way Host.cpp RoHost_Start does: missing, version mismatch, cycle, cascade, off, refused. `status` reads status.txt, checks its staleness and detects the game process (via `sysinfo`). `ops` has enable, disable, install and uninstall with the plugins.py rules. |
| `crates/ro-plugins/` | The binary. `cli.rs` uses clap. `gui/` uses eframe/egui. `assets/` holds Inter (SIL OFL 1.1, `Inter-LICENSE.txt`), embedded with `include_bytes!`. |

## CLI

```
ro-plugins list|info NAME|enable NAME|disable NAME|install [NAME...]|uninstall NAME [--client DIR] [--build DIR] [--force]
```

- `--client` defaults to `C:/Games/Ragnarok`.
- `--build` takes `<repo>/build` or its `plugins/` directory. It defaults to the `build/` of the repository that
  contains the exe or the working directory.
- `--force` lets `install` go ahead despite unsatisfied dependencies, and makes `uninstall` remove the dependents
  too.
- Exit codes: `0` ok, `1` refused or failed, `2` usage, `3` the game is running from the client directory.

The game counts as running when a process image is in the client directory and is either the exe named in
status.txt or any `*.exe`. status.txt is stale in two cases:

- a `.plugin`, a `.dll` or `disabled.txt` is newer than it, by mtime or creation time;
- the recorded pid is gone, or is no longer that client's exe.

## GUI

- **Top bar:** the game folder (click it to choose another; the full path is in its tooltip), one state chip that
  appears only when it matters (`Game running · changes locked`, or `Changes apply at the next game start`), the
  Install button, and a gear menu with the game folder, the build output and Reload.
- **Left:** a searchable plugin list. Each row shows a status dot, the name, the version and an enable switch, which
  is locked while the game runs. Plugins with a problem are grouped at the top under "Needs attention".
- **Right:** the selected plugin.
  - One status in plain words, merged from the installed files and the last game run:

    | Status | Meaning |
    | --- | --- |
    | Active | Loaded in the last game run, and the same version is still installed. |
    | Ready | Resolves, but the game has not started since it was installed or turned on. |
    | Off | Listed in `disabled.txt`. |
    | Blocked | A requirement is missing, turned off, the wrong version, or in a loop. |
    | Failed to start / Failed to load | The host recorded `failed-init` / `failed-load` in the last run. |
    | Can't load | Its DLL is missing. |
    | Not accepted | A DLL without a valid `.plugin` file. |
  - The description, taken from `Plugins/<Category>/<Name>/plugin.json`, and the Enabled switch.
  - **Requires:** each dependency with its range written as `^1.0.0` or `~1.0.0` where it has that form, and either
    the installed version with a check, or the problem with its fix (Install, Update, Turn on).
  - **Used by:** the plugins that depend on it; hidden when there are none.
  - **Technical details** (collapsed): what the host recorded, what the installed files resolve to, the files and
    the raw manifest.
- **Install:** a page listing every built plugin as New, Update, Downgrade or Reinstall, with the version change.
  Dependencies that are installed automatically show as "also installs …"; incompatibilities are listed and need an
  explicit "Install anyway".
- **Uninstall:** asks for confirmation and lists the plugins that are uninstalled with it.
- **Results:** each change reports in a toast; turning a plugin on or off offers Undo.
- **Keys:** Up/Down move through the list, Space turns the selected plugin on or off, Ctrl+F searches, Esc backs
  out.
- **Refresh:** the window watches `plugins/` with `notify` and polls processes once a second. It repaints only when
  something changes.
- **Settings:** the game folder and build output persist in eframe storage. A `--client` or `--build` given on the
  command line takes precedence.
