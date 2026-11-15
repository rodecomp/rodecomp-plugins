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
| `crates/ro-plugins-core/` | Library with no UI. `semver` is a port of scripts/semver.py: versions, npm-style ranges, the `-0` floors. `manifest` parses `<Name>.plugin`, `disabled.txt` and `Plugins/<Name>/plugin.json`. `resolve` predicts states the way Host.cpp RoHost_Start does: missing, version mismatch, cycle, cascade, off, refused. `status` reads status.txt, checks its staleness and detects the game process (via `sysinfo`). `ops` has enable, disable, install and uninstall with the plugins.py rules. |
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

- **Top bar:** the client folder (click it to choose another), a game pill (`Running · pid` or `Not running`), and a
  status.txt pill (`Live`, or `Stale` with the reason in its tooltip).
- **Left:** a searchable plugin list. Each row shows the name, the version, a state badge and an enable switch. The
  switch is greyed out while the game runs.
- **Right, Details:**
  - the description, taken from `Plugins/<Name>/plugin.json`;
  - the predicted and recorded states with their reasons, and a warning when they differ;
  - each dependency with its range, the installed and resolved versions, and a check or a cross;
  - the dependents, the files and the manifest.
- **Right, Graph:** dependencies on the left and dependents on the right. Nodes are coloured by state; red edges are
  unsatisfied ranges.
- **Actions:**
  - Install from build lists every built plugin as new, upgrade, downgrade or reinstall. It previews the dependencies
    that are installed automatically and any incompatibilities, which `--force` overrides.
  - Uninstall asks for confirmation. When dependents exist it lists them and requires `--force`.
  - Open folder.
- **Refresh:** the window watches `plugins/` with `notify` and polls processes once a second. It repaints only when
  something changes.
- **Settings:** the client and build paths persist in eframe storage. A `--client` or `--build` given on the command
  line takes precedence.
