//! Mutations of a client's plugins directory: enable, disable, install from the build output (merging the user's
//! setting values), uninstall, and setting values. Every mutation is refused while the game runs from the client.
//!
//! Each operation appends its report lines to `out` and returns the exit code, or a [`Refused`].

use std::fmt;
use std::fs;
use std::path::{Path, PathBuf};

use crate::manifest::{self, Manifest, Manifests, is_disabled};
use crate::settings;
use crate::resolve::{self, State};
use crate::status::{Processes, Status};

pub const EXIT_OK: i32 = 0;
pub const EXIT_REFUSED: i32 = 1;
pub const EXIT_USAGE: i32 = 2;
pub const EXIT_RUNNING: i32 = 3;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refused {
    pub message: String,
    pub code: i32,
}

impl Refused {
    pub fn new(message: impl Into<String>) -> Refused {
        Refused { message: message.into(), code: EXIT_REFUSED }
    }
}

impl fmt::Display for Refused {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for Refused {}

pub type Outcome = Result<i32, Refused>;

fn io_refused(action: &str, path: &Path, error: std::io::Error) -> Refused {
    Refused::new(format!("{action} {}: {error}", path.display()))
}

/// A client directory: `<client>/plugins` holds the installed plugins.
#[derive(Debug, Clone)]
pub struct Client {
    pub dir: PathBuf,
    pub plugins: PathBuf,
}

/// `s` when there is one user, as plugins.py words "depends"/"depend".
fn verb_s(count: usize) -> &'static str {
    if count == 1 { "s" } else { "" }
}

impl Client {
    pub fn new(dir: impl Into<PathBuf>) -> Client {
        let dir = dir.into();
        let plugins = dir.join("plugins");
        Client { dir, plugins }
    }

    pub fn manifests(&self) -> (Manifests, Vec<String>) {
        manifest::manifests(&self.plugins)
    }

    pub fn status(&self) -> Status {
        Status::read(&self.plugins)
    }

    pub fn canonical(&self, found: &Manifests, name: &str) -> Result<String, Refused> {
        manifest::canonical(found, name).map(str::to_owned).ok_or_else(|| {
            let names: Vec<&str> = found.keys().map(String::as_str).collect();
            let names = if names.is_empty() { "none".to_owned() } else { names.join(", ") };
            Refused::new(format!("{name}: not installed (installed: {names})"))
        })
    }

    pub fn refuse_while_running(&self, processes: &Processes, action: &str) -> Result<(), Refused> {
        let running = processes.game(&self.dir, &self.status());
        if running.is_empty() {
            return Ok(());
        }
        let listing: Vec<String> = running.iter().map(|p| format!("{} (pid {})", p.name(), p.pid)).collect();
        Err(Refused {
            message: format!(
                "refused: cannot {action} while the game is running from {}: {}. Close the game first; it keeps the \
                 plugin DLLs loaded and reads disabled.txt only at start.",
                self.dir.display(),
                listing.join(", ")
            ),
            code: EXIT_RUNNING,
        })
    }

    fn write_disabled(&self, names: &[String]) -> Result<(), Refused> {
        manifest::write_disabled(&self.plugins, names)
            .map_err(|error| io_refused("writing", &self.plugins.join(manifest::DISABLED_FILE), error))
    }

    pub fn disable(&self, name: &str, processes: &Processes, out: &mut Vec<String>) -> Outcome {
        let (found, _) = self.manifests();
        let name = self.canonical(&found, name)?;
        self.refuse_while_running(processes, "change disabled.txt")?;
        let mut disabled = manifest::read_disabled(&self.plugins);
        let users = resolve::dependents(&found, &name);
        if !users.is_empty() {
            out.push(format!(
                "warning: {} depend{} on {name} and will be disabled (needs {name}, which is turned off)",
                users.join(", "),
                verb_s(users.len())
            ));
        }
        if is_disabled(&disabled, &name) {
            out.push(format!("{name} is already turned off"));
            return Ok(EXIT_OK);
        }
        disabled.push(name.clone());
        self.write_disabled(&disabled)?;
        out.push(format!(
            "{name} turned off in {}; takes effect at the next game start",
            self.plugins.join(manifest::DISABLED_FILE).display()
        ));
        Ok(EXIT_OK)
    }

    pub fn enable(&self, name: &str, processes: &Processes, out: &mut Vec<String>) -> Outcome {
        let (found, _) = self.manifests();
        let name = self.canonical(&found, name)?;
        self.refuse_while_running(processes, "change disabled.txt")?;
        let needed = resolve::requirements(&found, &name);
        let disabled = manifest::read_disabled(&self.plugins);
        let enabled: Vec<&String> = needed.iter().filter(|n| is_disabled(&disabled, n)).collect();
        if enabled.is_empty() {
            out.push(format!("{name} is not turned off"));
        } else {
            let left: Vec<String> =
                disabled.iter().filter(|n| !enabled.iter().any(|e| e.eq_ignore_ascii_case(n))).cloned().collect();
            self.write_disabled(&left)?;
            for other in enabled {
                let why = if *other == name { String::new() } else { format!(" (needed by {name})") };
                out.push(format!("{other} turned on{why}"));
            }
        }
        let prediction = resolve::predict(&self.plugins).remove(&name).expect("an installed plugin is predicted");
        out.push(format!("{name} at the next game start: {}{}", prediction.state.as_str(), parenthesised(&prediction.reason)));
        Ok(if prediction.state == State::Loaded { EXIT_OK } else { EXIT_REFUSED })
    }

    pub fn uninstall(&self, name: &str, force: bool, processes: &Processes, out: &mut Vec<String>) -> Outcome {
        let (found, _) = self.manifests();
        let name = self.canonical(&found, name)?;
        let users = resolve::dependents(&found, &name);
        if !users.is_empty() && !force {
            return Err(Refused::new(format!(
                "refused: {} depend{} on {name}; uninstall {} first or use --force to remove {name} and its dependents",
                users.join(", "),
                verb_s(users.len()),
                if users.len() == 1 { "it" } else { "them" }
            )));
        }
        self.refuse_while_running(processes, "uninstall plugins")?;
        if !users.is_empty() {
            out.push(format!("--force: also removing dependents {}", users.join(", ")));
        }
        let disabled = manifest::read_disabled(&self.plugins);
        let targets: Vec<&String> = users.iter().rev().chain(std::iter::once(&name)).collect();
        for target in &targets {
            let mut removed = Vec::new();
            for suffix in ["dll", "pdb", manifest::EXTENSION, "plugin"] {
                let file = format!("{target}.{suffix}");
                let path = self.plugins.join(&file);
                if path.exists() {
                    fs::remove_file(&path).map_err(|error| io_refused("removing", &path, error))?;
                    removed.push(file);
                }
            }
            out.push(format!("uninstalled {target} {}: removed {}", found[target.as_str()].version, removed.join(", ")));
        }
        let left: Vec<String> =
            disabled.iter().filter(|n| !targets.iter().any(|t| t.eq_ignore_ascii_case(n))).cloned().collect();
        if left.len() != disabled.len() {
            self.write_disabled(&left)?;
        }
        Ok(EXIT_OK)
    }

    /// Plans installing `wanted` (every built plugin when empty) from `source`, plus missing dependencies.
    pub fn plan_install(&self, source: &Path, wanted: &[String]) -> Result<InstallPlan, Refused> {
        let (available, errors) = manifest::manifests(source);
        let mut notes: Vec<String> = errors.iter().map(|error| format!("build: invalid manifest: {error}")).collect();
        if available.is_empty() {
            return Err(Refused::new(format!("no plugins in {}; run scripts/build.py first", source.display())));
        }
        let wanted: Vec<String> = if wanted.is_empty() {
            available.keys().cloned().collect()
        } else {
            wanted
                .iter()
                .map(|name| {
                    manifest::canonical(&available, name).map(str::to_owned).ok_or_else(|| {
                        let names: Vec<&str> = available.keys().map(String::as_str).collect();
                        Refused::new(format!("{name}: not installed (installed: {})", names.join(", ")))
                    })
                })
                .collect::<Result<_, _>>()?
        };
        let (installed, _) = self.manifests();

        // Missing dependencies come from build/ too.
        let mut order: Vec<String> = Vec::new();
        let mut stack: Vec<String> = wanted.iter().rev().cloned().collect();
        while let Some(current) = stack.pop() {
            if order.contains(&current) {
                continue;
            }
            order.push(current.clone());
            for dependency in &available[&current].depends {
                let name = &dependency.name;
                if !installed.contains_key(name) && !order.contains(name) && available.contains_key(name) {
                    notes.push(format!("{name}: also installing, needed by {current}"));
                    stack.push(name.clone());
                }
            }
        }

        let mut after: Manifests = installed.clone();
        for name in &order {
            after.insert(name.clone(), available[name].clone());
        }
        let mut problems = Vec::new();
        for (name, manifest) in &after {
            for dependency in &manifest.depends {
                if !order.contains(name) && !order.contains(&dependency.name) {
                    continue; // unchanged by this install
                }
                match after.get(&dependency.name) {
                    None => problems.push(format!(
                        "{name} {} needs {} {}, which is neither installed nor built",
                        manifest.version, dependency.name, dependency.text
                    )),
                    Some(target) if !dependency.range.satisfied_by(&target.version) => problems.push(format!(
                        "{name} {} needs {} {}, would have {}",
                        manifest.version, dependency.name, dependency.text, target.version
                    )),
                    Some(_) => {}
                }
            }
        }
        let items = order
            .iter()
            .map(|name| InstallItem {
                manifest: available[name].clone(),
                previous: installed.get(name).map(|m| m.version.clone()),
            })
            .collect();
        Ok(InstallPlan { source: source.to_owned(), items, notes, problems })
    }

    pub fn install(&self, source: &Path, wanted: &[String], force: bool, processes: &Processes, out: &mut Vec<String>) -> Outcome {
        let plan = match self.plan_install(source, wanted) {
            Ok(plan) => plan,
            Err(refused) => {
                // plugins.py reports invalid build manifests before refusing.
                let (_, errors) = manifest::manifests(source);
                out.extend(errors.iter().map(|error| format!("build: invalid manifest: {error}")));
                return Err(refused);
            }
        };
        let (notes_errors, notes_deps): (Vec<&String>, Vec<&String>) =
            plan.notes.iter().partition(|note| note.starts_with("build: "));
        out.extend(notes_errors.into_iter().cloned());
        self.refuse_while_running(processes, "install plugins")?;
        out.extend(notes_deps.into_iter().cloned());
        if !plan.problems.is_empty() {
            for problem in &plan.problems {
                out.push(format!("{}: {problem}", if force { "warning" } else { "incompatible" }));
            }
            if !force {
                return Err(Refused::new(
                    "refused: install would leave unsatisfied dependencies (use --force to install anyway)",
                ));
            }
        }
        fs::create_dir_all(&self.plugins).map_err(|error| io_refused("creating", &self.plugins, error))?;
        for item in &plan.items {
            let name = &item.manifest.name;
            let dll = source.join(format!("{name}.dll"));
            if !dll.exists() {
                return Err(Refused::new(format!("{name}: {} is missing; rebuild with scripts/build.py", dll.display())));
            }
            for path in [dll.clone(), dll.with_extension("pdb")] {
                if path.exists() {
                    let target = self.plugins.join(path.file_name().expect("a file"));
                    copy_preserving_mtime(&path, &target).map_err(|error| io_refused("copying", &path, error))?;
                }
            }
            // The build's file plus the user's values from the old installed one that the new declarations accept.
            let new_text = fs::read_to_string(&item.manifest.path).map_err(|error| io_refused("reading", &item.manifest.path, error))?;
            let target = self.plugins.join(format!("{name}.{}", manifest::EXTENSION));
            let old_text = fs::read_to_string(&target).ok();
            let (text, dropped) = settings::merge(&new_text, &item.manifest.settings, old_text.as_deref())
                .map_err(|why| Refused::new(format!("{name}: {why}")))?;
            settings::write_atomically(&target, &text).map_err(|error| io_refused("writing", &target, error))?;
            let leftover = self.plugins.join(format!("{name}.plugin"));
            if leftover.exists() {
                fs::remove_file(&leftover).map_err(|error| io_refused("removing", &leftover, error))?;
            }
            let change = match &item.previous {
                Some(previous) => format!("{previous} -> {}", item.manifest.version),
                None => format!("{} (new)", item.manifest.version),
            };
            out.push(format!("installed {name} {change}"));
            for value in dropped {
                out.push(format!("{name}: dropped setting {} = {} ({})", value.key, value.value, value.why));
            }
        }
        Ok(EXIT_OK)
    }

    /// Sets (`Some`) or resets to its default (`None`) the value of setting `key` of plugin `name`.
    pub fn set_value(&self, name: &str, key: &str, value: Option<&toml_edit::Item>, processes: &Processes, out: &mut Vec<String>) -> Outcome {
        let (found, _) = self.manifests();
        let name = self.canonical(&found, name)?;
        self.refuse_while_running(processes, "change plugin settings")?;
        let plugin = &found[&name];
        let key = plugin.write_value(key, value).map_err(Refused::new)?;
        let shown = match value {
            Some(_) => {
                let setting = Manifest::read(&plugin.path).map_err(Refused::new)?.setting(&key).cloned();
                format!("set to {}", setting.map(|s| s.current().to_string()).unwrap_or_default())
            }
            None => format!("reset to the default {}", plugin.setting(&key).map(|s| s.default.to_string()).unwrap_or_default()),
        };
        out.push(format!("{name} {key} {shown}; takes effect at the next game start"));
        Ok(EXIT_OK)
    }
}

/// Like shutil.copy2: the copy keeps the source's modification time.
fn copy_preserving_mtime(from: &Path, to: &Path) -> std::io::Result<()> {
    fs::copy(from, to)?;
    if let Ok(modified) = fs::metadata(from).and_then(|m| m.modified()) {
        fs::File::options().write(true).open(to)?.set_modified(modified)?;
    }
    Ok(())
}

pub fn parenthesised(reason: &str) -> String {
    if reason.is_empty() { String::new() } else { format!(" ({reason})") }
}

#[derive(Debug, Clone)]
pub struct InstallItem {
    pub manifest: Manifest,
    /// The installed version this replaces.
    pub previous: Option<crate::semver::Version>,
}

impl InstallItem {
    pub fn change(&self) -> Change {
        match &self.previous {
            None => Change::New,
            Some(previous) => match self.manifest.version.cmp(previous) {
                std::cmp::Ordering::Greater => Change::Upgrade,
                std::cmp::Ordering::Less => Change::Downgrade,
                std::cmp::Ordering::Equal => Change::Same,
            },
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Change {
    New,
    Upgrade,
    Downgrade,
    Same,
}

#[derive(Debug, Clone)]
pub struct InstallPlan {
    pub source: PathBuf,
    /// In install order: the wanted plugins, each followed by the missing dependencies it pulls in.
    pub items: Vec<InstallItem>,
    /// Invalid build manifests and auto-installed dependencies.
    pub notes: Vec<String>,
    /// Dependencies the install would leave unsatisfied.
    pub problems: Vec<String>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::TempDir;

    fn plugin(name: &str, version: &str, depends: &str) -> String {
        let depends = if depends.is_empty() { String::new() } else { format!("\n[dependencies]\nB = \"{depends}\"\n") };
        format!("name = \"{name}\"\ncategory = \"T\"\nversion = \"{version}\"\n{depends}")
    }

    fn setup() -> (TempDir, Client, PathBuf) {
        let dir = TempDir::new();
        dir.write("client/plugins/A.toml", &plugin("A", "0.1.0", ">=1.0.0 <2.0.0-0"));
        dir.write("client/plugins/A.dll", "");
        dir.write("client/plugins/B.toml", &plugin("B", "1.0.0", ""));
        dir.write("client/plugins/B.dll", "");
        dir.write("client/plugins/B.pdb", "");
        dir.write("client/plugins/B.plugin", "name B\n");
        dir.write("build/A.toml", &plugin("A", "0.2.0", ">=1.0.0 <2.0.0-0"));
        dir.write("build/A.dll", "");
        dir.write("build/B.toml", &plugin("B", "1.1.0", ""));
        dir.write("build/B.dll", "");
        dir.write("build/C.toml", &plugin("C", "1.0.0", ">=2.0.0 <3.0.0-0"));
        dir.write("build/C.dll", "");
        let client = Client::new(dir.path().join("client"));
        let build = dir.path().join("build");
        (dir, client, build)
    }

    #[test]
    fn install_merges_values_and_reports_dropped_ones() {
        let (dir, client, build) = setup();
        let processes = Processes::new();
        let settings = "\n[settings.Title]\ntype = \"string\"\ndefault = \"x\"\n\n[settings.Size]\ntype = \"int\"\nmax = 10\ndefault = 1\n";
        dir.write("build/B.toml", &(plugin("B", "1.1.0", "") + settings));
        let old = "[settings.Title]\ntype = \"string\"\ndefault = \"x\"\nvalue = \"kept\"\n\n[settings.Size]\ntype = \"int\"\ndefault = 1\nvalue = 50\n\n\
                   [settings.Gone]\ntype = \"bool\"\ndefault = true\nvalue = false\n";
        dir.write("client/plugins/B.toml", &(plugin("B", "1.0.0", "") + "\n" + old));
        let mut out = Vec::new();
        assert_eq!(client.install(&build, &["B".into()], false, &processes, &mut out), Ok(0));
        assert_eq!(
            out,
            [
                "installed B 1.0.0 -> 1.1.0",
                "B: dropped setting Size = 50 (50 is above the maximum 10)",
                "B: dropped setting Gone = false (no longer a setting)",
            ]
        );
        let installed = fs::read_to_string(client.plugins.join("B.toml")).unwrap();
        assert_eq!(installed, plugin("B", "1.1.0", "") + &settings.replace("default = \"x\"\n", "default = \"x\"\nvalue = \"kept\"\n"));
        assert!(!client.plugins.join("B.plugin").exists(), "the old-format file is deleted");
        assert_eq!(client.manifests().0["B"].setting("Title").unwrap().current().as_str(), Some("kept"));
    }

    #[test]
    fn set_value_and_reset() {
        let (dir, client, _) = setup();
        let processes = Processes::new();
        dir.write("client/plugins/B.toml", &(plugin("B", "1.0.0", "") + "\n[settings.On]\ntype = \"bool\"\ndefault = true\n"));
        let mut out = Vec::new();
        assert_eq!(client.set_value("b", "on", Some(&toml_edit::value(false)), &processes, &mut out), Ok(0));
        assert_eq!(client.set_value("B", "On", None, &processes, &mut out), Ok(0));
        assert_eq!(
            out,
            ["B On set to false; takes effect at the next game start", "B On reset to the default true; takes effect at the next game start"]
        );
        let error = client.set_value("B", "On", Some(&toml_edit::value(3)), &processes, &mut out).unwrap_err();
        assert_eq!(error.message, "B On: expected a boolean, got an integer");
    }

    #[test]
    fn disable_warns_and_enable_turns_on_dependencies() {
        let (_dir, client, _) = setup();
        let processes = Processes::new();
        let mut out = Vec::new();
        assert_eq!(client.disable("b", &processes, &mut out), Ok(0));
        assert_eq!(out[0], "warning: A depends on B and will be disabled (needs B, which is turned off)");
        assert_eq!(manifest::read_disabled(&client.plugins), ["B"]);
        out.clear();
        assert_eq!(client.disable("B", &processes, &mut out), Ok(0));
        assert_eq!(out[1], "B is already turned off");
        out.clear();
        assert_eq!(client.enable("A", &processes, &mut out), Ok(0));
        assert_eq!(out, ["B turned on (needed by A)", "A at the next game start: loaded"]);
        assert!(manifest::read_disabled(&client.plugins).is_empty());
        let error = client.disable("Nope", &processes, &mut out).unwrap_err();
        assert_eq!(error.message, "Nope: not installed (installed: A, B)");
    }

    #[test]
    fn uninstall_refuses_dependents_unless_forced() {
        let (_dir, client, _) = setup();
        let processes = Processes::new();
        let mut out = Vec::new();
        let error = client.uninstall("B", false, &processes, &mut out).unwrap_err();
        assert_eq!(error.code, EXIT_REFUSED);
        assert_eq!(error.message, "refused: A depends on B; uninstall it first or use --force to remove B and its dependents");
        client.disable("A", &processes, &mut out).unwrap();
        out.clear();
        assert_eq!(client.uninstall("B", true, &processes, &mut out), Ok(0));
        assert_eq!(
            out,
            [
                "--force: also removing dependents A",
                "uninstalled A 0.1.0: removed A.dll, A.toml",
                "uninstalled B 1.0.0: removed B.dll, B.pdb, B.toml, B.plugin",
            ]
        );
        assert!(client.manifests().0.is_empty());
        assert!(manifest::read_disabled(&client.plugins).is_empty());
    }

    #[test]
    fn install_plans_upgrades_dependencies_and_conflicts() {
        let (_dir, client, build) = setup();
        let processes = Processes::new();
        let plan = client.plan_install(&build, &["a".into()]).unwrap();
        assert_eq!(plan.items.len(), 1);
        assert_eq!(plan.items[0].change(), Change::Upgrade);
        assert!(plan.problems.is_empty());

        let mut out = Vec::new();
        let error = client.install(&build, &["C".into()], false, &processes, &mut out).unwrap_err();
        assert_eq!(error.message, "refused: install would leave unsatisfied dependencies (use --force to install anyway)");
        assert_eq!(out, ["incompatible: C 1.0.0 needs B >=2.0.0 <3.0.0-0, would have 1.0.0"]);
        out.clear();
        assert_eq!(client.install(&build, &["C".into()], true, &processes, &mut out), Ok(0));
        assert_eq!(out, ["warning: C 1.0.0 needs B >=2.0.0 <3.0.0-0, would have 1.0.0", "installed C 1.0.0 (new)"]);
    }

    #[test]
    fn install_pulls_missing_dependencies() {
        let (_dir, client, build) = setup();
        let processes = Processes::new();
        let mut out = Vec::new();
        client.uninstall("B", true, &processes, &mut out).unwrap();
        out.clear();
        assert_eq!(client.install(&build, &["A".into()], false, &processes, &mut out), Ok(0));
        assert_eq!(out, ["B: also installing, needed by A", "installed A 0.2.0 (new)", "installed B 1.1.0 (new)"]);
        let predicted = resolve::predict(&client.plugins);
        assert_eq!(predicted["A"].state, State::Loaded);
    }

    #[test]
    fn install_from_empty_build_is_refused() {
        let (dir, client, _) = setup();
        let processes = Processes::new();
        let empty = dir.path().join("empty");
        let error = client.install(&empty, &[], false, &processes, &mut Vec::new()).unwrap_err();
        assert!(error.message.starts_with("no plugins in "));
    }
}
