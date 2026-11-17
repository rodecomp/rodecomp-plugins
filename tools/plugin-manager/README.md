# ro-plugins

The plugin manager for a rodecomp client directory. It is one native binary that works as both a GUI and a CLI. It
replaces `scripts/plugins.py`: the commands, output and exit codes are the same.

```sh
cargo run --release -p ro-plugins                        # opens the window
cargo run --release -p ro-plugins -- gui --exe FILE      # the window, on another game exe
cargo run --release -p ro-plugins -- list                # the CLI
cargo test                                               # semver, resolver, status.txt, mutations
```

The release binary is `target/release/ro-plugins(.exe)`. Use the stable toolchain.

## Layout

| Path | What |
| --- | --- |
| `crates/ro-plugins-core/` | Library with no UI. `semver` is a port of scripts/semver.py: versions, npm-style ranges, the `-0` floors. `manifest` parses and validates `<Name>.toml` plugin files (see `build/plugin-toml-spec.md`) and `disabled.txt`. `settings` holds the typed setting declarations, value validation, `toml_edit` edits of a `value` that keep comments and layout, and the install merge of the user's values. `resolve` predicts states the way Host.cpp RoHost_Start does: missing, version mismatch, cycle, cascade, off, refused. `status` reads status.txt, checks its staleness and detects the game process (via `sysinfo`). `ops` has enable, disable, install (with the value merge), uninstall and set/reset of setting values. |
| `crates/ro-plugins/` | The binary. `cli.rs` uses clap. `gui/` uses eframe/egui. `assets/` holds Inter (SIL OFL 1.1, `Inter-LICENSE.txt`), embedded with `include_bytes!`. |

## CLI

```
ro-plugins list|info NAME|enable NAME|disable NAME|install [NAME...]|uninstall NAME|set NAME KEY VALUE|reset NAME KEY
           [--client DIR | --exe FILE] [--build DIR] [--force]
```

- `--client` defaults to `C:/Games/Ragnarok`. `--exe` takes the game executable instead and uses its folder; the two
  are exclusive. The CLI has no command to start the game.
- `--build` takes `<repo>/build` or its `plugins/` directory. It defaults to the `build/` of the repository that
  contains the exe or the working directory.
- `--force` lets `install` go ahead despite unsatisfied dependencies, and makes `uninstall` remove the dependents
  too.
- `info` lists the plugin's settings with their current value (its `value`, else the default; an invalid `value` is
  reported and the default shown).
- `set NAME KEY VALUE` writes `value` of `[settings.KEY]` in `plugins/NAME.toml`. VALUE is TOML: `true`, `3`, `0.5`,
  `'"text"'`, `'["a.grf", "b.grf"]'`; it is validated against the declaration. `reset NAME KEY` removes the `value`, so
  the default applies. Both edit only that key (comments and layout stay) and are refused while the game runs.
- `install` copies `<Name>.dll`/`.pdb` and writes `plugins/<Name>.toml` from the build's `<Name>.toml` plus each
  `value` of the old installed file that is still valid under the new declaration; dropped values are reported as
  `<Name>: dropped setting KEY = VALUE (why)`. It deletes a leftover `<Name>.plugin` of the old format. `uninstall`
  removes `.dll`, `.pdb`, `.toml` and a leftover `.plugin`.
- Exit codes: `0` ok, `1` refused or failed, `2` usage, `3` the game is running from the client directory.

For changes, the game counts as running when a process image is in the client directory and is either the exe named
in status.txt or any `*.exe`: plugins are per folder, and their DLLs stay locked by whichever exe loaded them. The
GUI's Run button looks only at the chosen executable (exact path). status.txt is stale in two cases:

- a `.toml`, a `.dll` or `disabled.txt` is newer than it, by mtime or creation time;
- the recorded pid is gone, or is no longer that client's exe.

## GUI

- **Top bar:** the game executable and its folder (`Ragexe.exe — fullclient`; click it to choose another exe, the full
  path is in its tooltip), one state chip that appears only when it matters (`Game running · changes locked`, or
  `Changes apply at the next game start`), the Run button, the Install button, and a gear menu with the game
  executable, the build output and Reload.
- **Run:** starts the chosen exe detached, with its folder as working directory. It is disabled when no exe is chosen
  or the file is missing, and shows `Running` while that exe runs. When the folder holds several exes, the settings
  note that plugins apply to every one of them.
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
    | Not accepted | A DLL without a valid `.toml` file. |
  - The description, from the installed `<Name>.toml`, and the Enabled switch.
  - **Settings:** one field per `[settings.*]`, in file order, with its label and description: bool as a switch, int as
    a number clamped to `min`/`max`, float as a slider when it has both `min` and `max` (else a number), string as a
    text input, enum as a dropdown, path as a text input with a picker (file or folder, with the declared filters; a
    pick inside the client folder is stored relative with `/`), list as rows with move up/down, remove and Add. Text and
    numbers are written when the field is left. Reset appears when the file has a `value`; an invalid `value` shows its
    error and the default. Editing is locked while the game runs; changes apply at the next game start.
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
- **Settings:** the game executable and build output persist in eframe storage; the plugins are in
  `<exe folder>/plugins`. A setting from an older version that holds only the folder becomes the exe status.txt names,
  else `Ragexe.exe`, if it exists; otherwise the window asks. `--exe`, `--client` or `--build` on the command line
  take precedence.
