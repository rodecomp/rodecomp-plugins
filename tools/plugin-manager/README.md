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
           |setup|update|restore
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

## The game exe: setup, update, restore

Only the plugin exe built by `scripts/build.py` loads plugins. `ro_plugins_core::exe::exe_kind` decides it from the
PE headers alone, read-only: the export table names `RoLoaderEntry` **and** the optional header's
`AddressOfEntryPoint` is that export's RVA. Anything else that parses as an exe is "no loader" (rodecomp's plain exe,
Gravity's original); a file that doesn't parse is "not an exe". `list` and `info` print the exe's kind.

A *plugin set* is the build output: `build/exe/Ragexe.exe` (+ `.pdb`), `build/RoPluginHost.dll`, `build/plugins/`.

- `setup` (exe without loader): `MyRO.exe` becomes `MyRO.original.exe` (its `.pdb` becomes `MyRO.original.pdb`),
  the plugin exe is copied in as `MyRO.exe`, then the host and every plugin are installed. A backup that already
  exists is reused when identical and refused when different (never overwritten).
- `update` (plugin build): exe, host and every plugin are replaced from the set; the backup stays.
- `restore`: the backup becomes the exe again; plugin files stay (the original exe ignores them).
- Both setup and update merge setting values like `install`, keep `disabled.txt`, and add every plugin new to the
  client to `disabled.txt`, except the essential ones and their dependencies: **ClientInfo** (clientinfo.xml server,
  langtype, the login packet rAthena expects), **GrfsEmbedded** (loads `data.grf` then `new.grf`), **ServiceFixes**
  (only the Korean ExternalSettings on, so kRO data works on any langtype) and **SystemTweaks** (settings in HKCU,
  starts without the launcher argument). These are what a private-server client needs to start
  and connect; everything else changes looks or gameplay and is the user's choice. A plugin new to the client gets
  the recommended setting values (`setup::RECOMMENDED_SETTINGS`); an installed one keeps the user's values.
- **First open (GUI):** the first time the window opens a game folder that loads plugins, every recommended plugin
  that is off is turned on, with its dependencies, and the toast offers Undo. The folder is remembered (eframe
  storage, `seen_clients`), so later opens never touch the user's switches. A folder without the loader, or whose game
  is running, gets it at a later open (after setup, right away).
- All refuse while the game runs from the folder. Every new file is copied next to its target under a temporary
  name first; only then are files renamed into place, so a failure while copying leaves the client unchanged.
- **Rights:** when the client folder (or its `plugins/`) is not writable, the command re-runs `ro-plugins.exe`
  elevated (UAC, hidden window: `ro-plugins elevated setup|update|restore --exe … --build … --grant SID --result
  FILE`), waits for it and prints its report. Declining the prompt changes nothing. The elevated run grants the
  calling user modify rights on `<client>/plugins` (icacls, by SID), so switches and settings later need no
  elevation. A writable folder is never elevated.
- **Update offered** (GUI) when a plugin's last run failed with "built for a different …", or the client's exe or
  `RoPluginHost.dll` differs from the build output's by size or modification time (copies keep the build's time, so
  this avoids hashing 13 MB on each refresh).

GUI (worded for players, no loader/build jargon): the game button (`Ragexe.exe — fullclient`) only picks the game; it
opens the file dialog. Beside it, the game's state is a chip: `Plugins on`, `Not a game`, or, when there is something
to do, a clickable chip with a caret: `Update available ›` (opens the update dialog) and `Plugins off ›` (opens the
turn-on dialog). **Go back to the original game…** is in the settings menu, under the game, when a backup exists. An
exe without the loader also shows a
banner "Plugins are off for this game" with **Turn on plugins** and **Choose another game**; switches and settings
are locked and Run is disabled with the reason. Each action opens a short dialog: two equal tiles (Original game →
Game with plugins, reversed for restore; none for update), one or two plain lines (how to undo, what's kept) and a
folded **Details** section with the exact file steps and the build output (changeable; opened when it's missing).
While it runs, a progress modal shows (the work is off the UI thread).

## GUI

- **Top bar:** the game button (`Ragexe.exe — fullclient`, full path in its tooltip) with the game's state chip, one
  state chip that appears only when it matters (`Game running · changes locked`, or `Changes apply at the next game
  start`), Install (secondary), the restart badge, Run (the one filled accent button, far right) and a gear menu with
  the game, where new plugins come from (the build output) and Reload.
- **Restart badge:** while the game runs and switches, settings or installed files changed since it started,
  `2 changes · restart to apply` sits next to Run; its tooltip lists them (`Zoom turned off`, `Fps settings changed`).
  Counted by `status::pending_changes` from the live status.txt, disabled.txt and the files newer than status.txt.
- **Run:** starts the chosen exe detached, with its folder as working directory. It is disabled when no exe is chosen
  or the file is missing, and shows `Running` while that exe runs. When the folder holds several exes, the settings
  note that plugins apply to every one of them.
- **Failure banner:** when a plugin failed in the last run (status.txt, for the installed version), the top of the
  content reads `1 plugin failed: InputLimits — made for another version of the game`, with **Update** when a rebuilt
  set fixes it, else **Show** (selects it). Dismissible for the session; it comes back when the failures change.
- **Left:** a searchable plugin list with an **All / On / ★** switch under Search (each segment shows its count; **On**
  lists only the plugins that are turned on, **★** only the recommended ones; any that need attention always show). Every
  category starts collapsed; selecting a plugin opens its category. Each header shows `Camera 2/3` (on/total). The
  view persists in eframe storage. Each row shows the name and the enable switch (locked while the game runs); the essential plugins
  and their dependencies (`setup::recommended`, the set setup turns on) carry a star. A problem icon (warning or
  octagon, amber or red) appears only for a plugin that needs attention, with the reason in its tooltip; those
  plugins are grouped at the top under "Needs attention" and never hidden by the filter. A switch change shows a
  brief ✓ on its row.
- **Right:** the selected plugin. Nothing is selected at startup; then the right side shows `N of M plugins on` and
  "Pick a plugin on the left to see and change it."
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
  - The description, the Recommended chip when it applies, and the Enabled switch (with an inline "✓ Saved" or the
    error in red). The `⋯` menu in the header holds **Open folder** and **Uninstall…** (red, with a confirmation).
  - **Settings:** one field per `[settings.*]`, in file order except that a setting with `when` comes right after the
    enum it depends on, and is hidden while that enum (its unsaved edit, else its saved value) holds none of the
    listed values (`Setting::is_shown`, `settings::display_order`). Each with its label and description: bool as a
    switch, int as a number clamped to `min`/`max`, float as a slider when it has both `min` and `max` (else a number), string as a
    text input, enum as every choice visible at once (its `choices` label, else the raw value; one click selects and
    saves): stacked cards with the description under the label when any choice has one, else compact wrapping chips;
    the selected one in the accent. Path as a text input with a picker (file or folder, with the declared filters; a
    pick inside the client folder is stored relative with `/`), list as rows with move up/down, remove and Add. Text and
    numbers are written when the field is left; each save shows "✓ Saved" on its row for ~1.5 s, or "Not saved: …" in
    red. A setting whose value differs from its default (`Setting::is_changed`) shows `• Changed` and a reset button
    left of its control (the control never moves); **Reset all** is disabled when no shown setting differs, and also
    resets hidden ones. An invalid `value` shows its error and the default. Editing is locked while the game runs;
    changes apply at the next game start.
  - **Requires:** each dependency with its range written as `^1.0.0` or `~1.0.0` where it has that form, and either
    the installed version with a check, or the problem with its fix (Install, Update, Turn on).
  - **Used by:** the plugins that depend on it; hidden when there are none.
  - **Technical details** (collapsed): what the host recorded, what the installed files resolve to, the files and
    the raw manifest.
- **Install:** a page listing every built plugin as New, Update, Downgrade or Reinstall, with the version change.
  Dependencies that are installed automatically show as "also installs …"; incompatibilities are listed and need an
  explicit "Install anyway".
- **Uninstall:** from the `⋯` menu; asks for confirmation and lists the plugins that are uninstalled with it.
- **Results:** switches and settings confirm inline on their row; other changes report in a toast; turning a plugin on
  or off offers Undo.
- **Keys:** Up/Down move through the list, Space turns the selected plugin on or off, Ctrl+F searches, Esc backs
  out.
- **Refresh:** the window watches `plugins/` with `notify` and polls processes once a second. It repaints only when
  something changes.
- **Settings:** the game executable, build output and the All / On / ★ view persist in eframe
  storage; the plugins are in
  `<exe folder>/plugins`. A setting from an older version that holds only the folder becomes the exe status.txt names,
  else `Ragexe.exe`, if it exists; otherwise the window asks. `--exe`, `--client` or `--build` on the command line
  take precedence.
