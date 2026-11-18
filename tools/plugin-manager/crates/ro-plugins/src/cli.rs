//! The command line: the same commands, output and exit codes as the former scripts/plugins.py.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use clap::{Parser, Subcommand};
use ro_plugins_core::manifest::{self, is_disabled};
use ro_plugins_core::ops::{Client, EXIT_OK, EXIT_USAGE, Outcome, parenthesised};
use ro_plugins_core::relsig::{self, Check, ExeRelsig};
use ro_plugins_core::resolve;
use ro_plugins_core::status::{Processes, Status, freshness};

use crate::paths;

#[derive(Parser)]
#[command(
    name = "ro-plugins",
    version,
    about = "Plugin manager for a rodecomp client directory: what is installed, what the host loaded or not and why, \
             and switches. Without a command it opens the GUI.",
    after_help = "Exit codes: 0 success, 1 refused or failed (dependency problem, unknown plugin), 2 usage error, \
                  3 refused because the game is running from this client directory."
)]
pub struct Args {
    /// Client directory [default: C:/Games/Ragnarok]
    #[arg(long, global = true, value_name = "DIR", conflicts_with = "exe")]
    pub client: Option<PathBuf>,
    /// Game executable; the client directory is its folder
    #[arg(long, global = true, value_name = "FILE")]
    pub exe: Option<PathBuf>,
    /// Build output: <repo>/build or its plugins/ directory [default: <repo>/build]
    #[arg(long, global = true, value_name = "DIR")]
    pub build: Option<PathBuf>,
    #[command(subcommand)]
    pub command: Option<Command>,
}

#[derive(Subcommand)]
pub enum Command {
    /// Open the window (also the default without a command)
    Gui,
    /// Installed plugins with recorded and predicted state
    List,
    /// Manifest, dependents, state and reason
    Info { name: String },
    /// Turn a plugin off (adds it to plugins/disabled.txt; warns about dependents)
    Disable { name: String },
    /// Turn a plugin and its needed dependencies on
    Enable { name: String },
    /// Copy plugins (and missing dependencies) from the build output
    Install {
        /// Default: every built plugin
        #[arg(value_name = "NAME")]
        names: Vec<String>,
        /// Install despite unsatisfied dependencies
        #[arg(long)]
        force: bool,
    },
    /// Set a plugin setting in plugins/<NAME>.toml; VALUE is TOML: true, 3, 0.5, '"text"', '["a.grf", "b.grf"]'
    Set { name: String, key: String, value: String },
    /// Reset a plugin setting to its default (removes its value)
    Reset { name: String, key: String },
    /// Remove a plugin's .dll, .pdb and .toml
    Uninstall {
        name: String,
        /// Also uninstall the plugins that depend on it
        #[arg(long)]
        force: bool,
    },
}

/// Paths as plugins.py printed them (pathlib uses backslashes on Windows).
fn native(path: PathBuf) -> PathBuf {
    if cfg!(windows) { PathBuf::from(path.to_string_lossy().replace('/', "\\")) } else { path }
}

pub fn run(args: Args, command: Command) -> i32 {
    let client_dir = match &args.exe {
        Some(exe) => {
            if !exe.is_file() {
                eprintln!("ro-plugins: error: --exe {}: not a file", native(exe.clone()).display());
                return EXIT_USAGE;
            }
            let exe = std::path::absolute(exe).unwrap_or_else(|_| exe.clone());
            native(exe.parent().map(Path::to_owned).unwrap_or_default())
        }
        None => native(args.client.unwrap_or_else(|| PathBuf::from(paths::DEFAULT_CLIENT))),
    };
    if !client_dir.is_dir() {
        eprintln!("ro-plugins: error: --client {}: not a directory", client_dir.display());
        return EXIT_USAGE;
    }
    let client = Client::new(client_dir);
    let build = args.build.map(native).or_else(paths::default_build);
    let mut out = Vec::new();
    let processes = || Processes::new();
    let outcome: Outcome = match command {
        Command::Gui => unreachable!("handled by main"),
        Command::List => Ok(list(&client, args.exe.as_deref(), &processes(), &mut out)),
        Command::Info { name } => info(&client, &name, args.exe.as_deref(), &processes(), &mut out),
        Command::Disable { name } => client.disable(&name, &processes(), &mut out),
        Command::Enable { name } => client.enable(&name, &processes(), &mut out),
        Command::Set { name, key, value } => match value.parse::<toml_edit::Value>() {
            Ok(value) => client.set_value(&name, &key, Some(&toml_edit::Item::Value(value)), &processes(), &mut out),
            Err(error) => {
                eprintln!(
                    "ro-plugins: error: VALUE {value:?} is not a TOML value ({}); quote strings, e.g. '\"text\"'",
                    error.message().trim_end()
                );
                return EXIT_USAGE;
            }
        },
        Command::Reset { name, key } => client.set_value(&name, &key, None, &processes(), &mut out),
        Command::Uninstall { name, force } => client.uninstall(&name, force, &processes(), &mut out),
        Command::Install { names, force } => match build {
            Some(build) => client.install(&paths::build_plugins(&build), &names, force, &processes(), &mut out),
            None => {
                eprintln!("ro-plugins: error: no build output found; pass --build DIR");
                return EXIT_USAGE;
            }
        },
    };
    for line in &out {
        println!("{line}");
    }
    match outcome {
        Ok(code) => code,
        Err(refused) => {
            eprintln!("{refused}");
            refused.code
        }
    }
}

/// Columns left-aligned to the widest cell, trailing spaces removed.
fn table(rows: &[Vec<String>], out: &mut Vec<String>) {
    let columns = rows[0].len();
    let widths: Vec<usize> =
        (0..columns).map(|i| rows.iter().map(|row| row[i].chars().count()).max().unwrap_or(0)).collect();
    for row in rows {
        let line: Vec<String> = row.iter().zip(&widths).map(|(cell, width)| format!("{cell:<width$}")).collect();
        out.push(line.join("  ").trim_end().to_owned());
    }
}

/// The installed exe's `.relsig`: of `--exe` when given, else of the exe status.txt names (or Ragexe.exe).
fn exe_relsig(client: &Client, exe: Option<&Path>, status: &Status) -> Option<ExeRelsig> {
    ExeRelsig::read(&exe.map_or_else(|| relsig::exe_of(&client.dir, status), Path::to_owned))
}

/// What `list` and `info` say about changed exe functions; nothing when they are unchanged or cannot be checked.
fn exe_warning(name: &str, check: &Check) -> Option<String> {
    (!check.changes().is_empty()).then(|| {
        format!(
            "warning: {name} uses exe functions that changed since it was built ({}): it still loads; review its hooks \
             and rebuild it",
            check.summary()
        )
    })
}

fn list(client: &Client, exe: Option<&Path>, processes: &Processes, out: &mut Vec<String>) -> i32 {
    let (found, errors) = client.manifests();
    let status = client.status();
    let predicted = resolve::predict(&client.plugins);
    let exe_sigs = exe_relsig(client, exe, &status);
    let mut warnings = Vec::new();
    if !status.header.is_empty() {
        let field = |key| status.header(key).unwrap_or("None");
        out.push(format!(
            "host {}, api {}, exe {}, pid {}, started {}",
            field("host"),
            field("api"),
            status.header("exe").unwrap_or("?"),
            field("pid"),
            field("started")
        ));
    }
    for note in freshness(&client.dir, &client.plugins, &status, processes).notes {
        out.push(format!("status.txt: {note}"));
    }
    let mut rows =
        vec![["CATEGORY", "NAME", "VERSION", "DEPENDS", "RECORDED", "PREDICTED", "EXE", "REASON"].map(String::from).to_vec()];
    let mut mismatches = 0;
    let mut names: Vec<&String> = found.keys().chain(predicted.keys()).collect::<BTreeSet<_>>().into_iter().collect();
    let category = |name: &String| found.get(name).map_or("-".to_owned(), |m| m.category.clone());
    names.sort_by_key(|name| (category(name).to_lowercase(), name.to_lowercase()));
    for name in names {
        let manifest = found.get(name);
        let depends = match manifest {
            Some(m) if !m.depends.is_empty() => {
                m.depends.iter().map(|d| format!("{} {}", d.name, d.text)).collect::<Vec<_>>().join(", ")
            }
            _ => "-".into(),
        };
        let recorded = status.plugins.get(name);
        let prediction = &predicted[name];
        let mut state = prediction.state.as_str().to_owned();
        if recorded.is_some_and(|r| r.state != state) {
            state += "  <- MISMATCH";
            mismatches += 1;
        }
        let reason = recorded.map(|r| r.reason.as_str()).filter(|r| !r.is_empty()).unwrap_or(&prediction.reason);
        let check = manifest.map(|m| relsig::check(m.exe.as_ref(), exe_sigs.as_ref()));
        let exe_column = match &check {
            Some(Check::Unchanged) => "ok".to_owned(),
            Some(Check::Changed(changes)) => format!("{} CHANGED", changes.len()),
            Some(Check::Unchecked(_)) | None => "-".to_owned(),
        };
        warnings.extend(check.as_ref().and_then(|check| exe_warning(name, check)));
        rows.push(vec![
            category(name),
            name.clone(),
            manifest.map_or("-".into(), |m| m.version.to_string()),
            depends,
            recorded.map_or("-".into(), |r| r.state.clone()),
            state,
            exe_column,
            reason.to_owned(),
        ]);
    }
    table(&rows, out);
    out.extend(errors.iter().map(|error| format!("invalid manifest: {error}")));
    out.extend(warnings);
    if mismatches > 0 {
        out.push(format!(
            "{mismatches} recorded state(s) differ from the prediction: restart the game to apply the installed files \
             and disabled.txt, or see RoPluginHost.log for load/init failures."
        ));
    }
    EXIT_OK
}

/// `<Name>.dll/.pdb/.toml` (and a leftover `.plugin`) present in `plugins`, sorted.
pub fn plugin_files(plugins: &Path, name: &str) -> Vec<String> {
    let mut files: Vec<String> = std::fs::read_dir(plugins)
        .into_iter()
        .flatten()
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| {
            path.file_stem().is_some_and(|stem| stem.to_string_lossy().eq_ignore_ascii_case(name))
                && path.extension().is_some_and(|ext| matches!(ext.to_str(), Some("dll" | "pdb" | "toml" | "plugin")))
        })
        .filter_map(|path| path.file_name().map(|name| name.to_string_lossy().into_owned()))
        .collect();
    files.sort();
    files
}

fn info(client: &Client, name: &str, exe: Option<&Path>, processes: &Processes, out: &mut Vec<String>) -> Outcome {
    let (found, _) = client.manifests();
    let name = client.canonical(&found, name)?;
    let manifest = &found[&name];
    let status = client.status();
    let recorded = status.plugins.get(&name);
    let prediction = resolve::predict(&client.plugins).remove(&name).expect("an installed plugin is predicted");
    out.push(format!("{name} {}", manifest.version));
    if !manifest.description.is_empty() {
        out.push(manifest.description.clone());
    }
    out.push(format!("manifest {}:", manifest.path.display()));
    out.extend(manifest.text().lines().map(|line| format!("    {line}")));
    if manifest.settings.is_empty() {
        out.push("settings: (none)".into());
    } else {
        out.push("settings:".into());
        for setting in &manifest.settings {
            let source = match (&setting.value, &setting.error) {
                (Some(_), _) => String::new(),
                (None, Some(why)) => format!(" (default; the value is invalid: {why})"),
                (None, None) => " (default)".to_owned(),
            };
            out.push(format!("    {} ({}) = {}{source}", setting.key, setting.kind.name(), setting.current()));
        }
    }
    out.push(format!("files: {}", plugin_files(&client.plugins, &name).join(", ")));
    let users = resolve::dependents(&found, &name);
    out.push(format!("dependents: {}", if users.is_empty() { "(none)".into() } else { users.join(", ") }));
    let off = is_disabled(&manifest::read_disabled(&client.plugins), &name);
    out.push(format!("turned off in disabled.txt: {}", if off { "yes" } else { "no" }));
    match recorded {
        Some(record) => {
            out.push(format!("recorded state: {}{}", record.state, parenthesised(&record.reason)));
            if !record.resolved.is_empty() {
                out.push(format!("resolved dependencies: {}", record.resolved.replace(',', ", ").replace('=', " ")));
            }
        }
        None => out.push("recorded state: - (not in status.txt)".into()),
    }
    out.push(format!("predicted state: {}{}", prediction.state.as_str(), parenthesised(&prediction.reason)));
    let check = relsig::check(manifest.exe.as_ref(), exe_relsig(client, exe, &status).as_ref());
    match &check {
        Check::Unchecked(why) => out.push(format!("exe functions: cannot check ({why})")),
        Check::Unchanged => out.push(format!(
            "exe functions: {} used, unchanged since it was built",
            manifest.exe.as_ref().map_or(0, |recorded| recorded.functions.len())
        )),
        Check::Changed(changes) => {
            out.extend(exe_warning(&name, &check));
            out.extend(changes.iter().map(|(function, change)| format!("    {} {function}", change.as_str())));
        }
    }
    for note in freshness(&client.dir, &client.plugins, &status, processes).notes {
        out.push(format!("status.txt: {note}"));
    }
    Ok(EXIT_OK)
}
