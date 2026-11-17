//! What the window shows, read from disk, plus the background watchers: a `notify` watcher on the client's
//! plugins/ directory and a ~1 s process poll. Both wake the UI only when something changed.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use eframe::egui;
use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use ro_plugins_core::manifest::{self, Manifest, Manifests, PluginJson};
use ro_plugins_core::ops::{Change, Client};
use ro_plugins_core::resolve::{self, Prediction, State};
use ro_plugins_core::semver::{Comparator, Op, Range, Version};
use ro_plugins_core::status::{self, Freshness, GameProcess, Processes, Status};

/// How a status reads at a glance.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tone {
    Good,
    Off,
    Warn,
    Bad,
    Info,
}

/// The one status the window shows for a plugin: the installed files and the last game run, merged.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Health {
    pub tone: Tone,
    pub label: &'static str,
    /// One sentence, only when something is wrong or pending.
    pub detail: Option<String>,
}

impl Health {
    fn new(tone: Tone, label: &'static str, detail: Option<String>) -> Health {
        Health { tone, label, detail }
    }

    pub fn needs_attention(&self) -> bool {
        matches!(self.tone, Tone::Warn | Tone::Bad)
    }
}

/// A dependency range the way a person writes it: `^1.2.0`, `~1.2.0`, `1.2.0`, "any version".
pub fn human_range(range: &Range) -> String {
    fn set(comparators: &[Comparator]) -> String {
        let zero = Version::new(0, 0, 0);
        match comparators {
            [] => "any version".to_owned(),
            [only] if only.op == Op::Ge && only.version == zero => "any version".to_owned(),
            [only] if only.op == Op::Eq => only.version.to_string(),
            [low, high] if low.op == Op::Ge && high.op == Op::Lt && high.version.prerelease == ["0"] => {
                let (a, b) = (&low.version, &high.version);
                let next_major = (a.major + 1, 0, 0);
                let next_minor = (a.major, a.minor + 1, 0);
                let upper = (b.major, b.minor, b.patch);
                if (a.major > 0 && upper == next_major) || (a.major == 0 && a.minor > 0 && upper == next_minor) {
                    format!("^{a}")
                } else if upper == next_minor {
                    format!("~{a}")
                } else {
                    format!("{a} or newer, below {}.{}.{}", b.major, b.minor, b.patch)
                }
            }
            several => several.iter().map(|c| format!("{}{}", c.op.as_str(), c.version)).collect::<Vec<_>>().join(" "),
        }
    }
    range.0.iter().map(|comparators| set(comparators)).collect::<Vec<_>>().join(" or ")
}

/// Why a plugin that is installed and turned on still cannot load.
fn blocked_reason(found: &Manifests, predicted: &BTreeMap<String, Prediction>, name: &str) -> String {
    let reason = predicted.get(name).map_or("", |p| p.reason.as_str());
    if let Some(chain) = reason.strip_prefix("in dependency cycle ") {
        return format!("Part of a dependency loop: {}.", chain.replace(" -> ", " → "));
    }
    for dependency in found.get(name).map(|m| m.depends.as_slice()).unwrap_or_default() {
        let Some(target) = found.get(&dependency.name) else {
            return format!("Needs {}, which isn't installed.", dependency.name);
        };
        match predicted.get(&dependency.name).map(|p| p.state) {
            Some(State::Off) => return format!("Needs {}, which is turned off.", dependency.name),
            Some(State::Disabled | State::FailedLoad) => return format!("Needs {}, which can't load either.", dependency.name),
            _ => {}
        }
        if !dependency.range.satisfied_by(&target.version) {
            return format!("Needs {} {}, but {} is installed.", dependency.name, human_range(&dependency.range), target.version);
        }
    }
    reason.to_owned()
}

fn health(found: &Manifests, predicted: &BTreeMap<String, Prediction>, status: &Status, live: bool, name: &str) -> Health {
    let recorded = status.plugins.get(name);
    match predicted[name].state {
        State::Off => {
            let still = live && recorded.is_some_and(|r| r.state == "loaded");
            Health::new(Tone::Off, "Off", still.then(|| "Still loaded in the running game; it stays off from the next start.".to_owned()))
        }
        State::Disabled => Health::new(Tone::Warn, "Blocked", Some(blocked_reason(found, predicted, name))),
        State::FailedLoad => Health::new(Tone::Bad, "Can't load", Some(format!("{name}.dll is missing from the plugins folder."))),
        State::Refused => Health::new(
            Tone::Bad,
            "Not accepted",
            Some(format!("{name}.dll has no valid {name}.plugin file next to it, so the game ignores it.")),
        ),
        State::Loaded | State::FailedInit | State::Pending => {
            // What the last game run did only counts for the version that is still installed.
            let same_version = recorded.is_some_and(|r| found.get(name).is_some_and(|m| m.version.to_string() == r.version));
            match recorded.map(|r| r.state.as_str()).filter(|_| same_version) {
                Some("loaded") => Health::new(Tone::Good, "Active", None),
                Some("failed-init") => Health::new(
                    Tone::Bad,
                    "Failed to start",
                    Some("It loaded, but its start-up reported an error in the last game run. RoPluginHost.log has the details.".to_owned()),
                ),
                Some("failed-load") => Health::new(
                    Tone::Bad,
                    "Failed to load",
                    Some("Windows could not load its DLL in the last game run. RoPluginHost.log has the details.".to_owned()),
                ),
                _ => {
                    let when = if live { "Restart the game to load it." } else { "Loads the next time the game starts." };
                    Health::new(Tone::Info, "Ready", Some(when.to_owned()))
                }
            }
        }
    }
}

/// The installed plugins of a client and everything derived from them.
pub struct Snapshot {
    pub client: Client,
    pub found: Manifests,
    pub errors: Vec<String>,
    pub status: Status,
    pub predicted: BTreeMap<String, Prediction>,
    pub disabled: Vec<String>,
    pub freshness: Freshness,
    /// Names in display order (case-insensitive), including refused DLLs.
    pub names: Vec<String>,
    /// The status shown for each name.
    pub health: BTreeMap<String, Health>,
}

impl Snapshot {
    pub fn read(client: &Path, recorded_image: Option<&Path>) -> Snapshot {
        let client = Client::new(client);
        let (found, errors) = client.manifests();
        let status = client.status();
        let predicted = resolve::predict(&client.plugins);
        let disabled = manifest::read_disabled(&client.plugins);
        let freshness = status::freshness_with_image(&client.dir, &client.plugins, &status, recorded_image);
        let mut names: Vec<String> = predicted.keys().cloned().collect();
        names.sort_by_key(|name| name.to_lowercase());
        let live = status.exists && freshness.live;
        let health = names.iter().map(|name| (name.clone(), health(&found, &predicted, &status, live, name))).collect();
        Snapshot { client, found, errors, status, predicted, disabled, freshness, names, health }
    }

    pub fn is_off(&self, name: &str) -> bool {
        manifest::is_disabled(&self.disabled, name)
    }

    pub fn dependents(&self, name: &str) -> Vec<String> {
        resolve::dependents(&self.found, name)
    }

    /// Plugins whose installed files have not been through a game start yet.
    pub fn pending(&self) -> usize {
        self.health.values().filter(|h| h.tone == Tone::Info).count()
    }
}

/// One plugin available in the build output.
pub struct Available {
    pub manifest: Manifest,
    pub installed: Option<Version>,
}

impl Available {
    pub fn change(&self) -> Change {
        match &self.installed {
            None => Change::New,
            Some(installed) => match self.manifest.version.cmp(installed) {
                std::cmp::Ordering::Greater => Change::Upgrade,
                std::cmp::Ordering::Less => Change::Downgrade,
                std::cmp::Ordering::Equal => Change::Same,
            },
        }
    }
}

/// The build output's plugins, compared with the installed ones.
pub struct BuildListing {
    pub dir: PathBuf,
    pub plugins: Vec<Available>,
    pub errors: Vec<String>,
}

impl BuildListing {
    pub fn read(build_plugins: &Path, installed: &Manifests) -> BuildListing {
        let (available, errors) = manifest::manifests(build_plugins);
        let plugins = available
            .into_values()
            .map(|manifest| {
                let installed = installed.get(&manifest.name).map(|m| m.version.clone());
                Available { manifest, installed }
            })
            .collect();
        BuildListing { dir: build_plugins.to_owned(), plugins, errors }
    }
}

/// Descriptions from `Plugins/<Name>/plugin.json`.
pub fn descriptions(sources: Option<&Path>) -> BTreeMap<String, PluginJson> {
    sources.map(manifest::plugin_sources).unwrap_or_default()
}

/// What the process poll found.
#[derive(Default, Clone, PartialEq)]
pub struct GameState {
    pub running: Vec<GameProcess>,
    /// Live image of the pid in status.txt.
    pub recorded_image: Option<PathBuf>,
}

/// Polls the process table about once a second for the current client.
pub struct ProcessWatch {
    pub processes: Arc<Mutex<Processes>>,
    state: Arc<Mutex<GameState>>,
    client: Arc<Mutex<PathBuf>>,
    stop: Arc<AtomicBool>,
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn poll(processes: &Processes, client: &Path) -> GameState {
    let status = Status::read(&client.join("plugins"));
    let pid = status.pid();
    GameState {
        running: if client.as_os_str().is_empty() { Vec::new() } else { processes.game(client, &status) },
        recorded_image: if pid != 0 { processes.image(pid).map(Path::to_owned) } else { None },
    }
}

impl ProcessWatch {
    pub fn start(ctx: egui::Context, client: PathBuf) -> ProcessWatch {
        let processes = Processes::new();
        let first = poll(&processes, &client);
        let watch = ProcessWatch {
            processes: Arc::new(Mutex::new(processes)),
            state: Arc::new(Mutex::new(first)),
            client: Arc::new(Mutex::new(client)),
            stop: Arc::new(AtomicBool::new(false)),
        };
        let (processes, state, client, stop) =
            (watch.processes.clone(), watch.state.clone(), watch.client.clone(), watch.stop.clone());
        std::thread::Builder::new()
            .name("process-poll".into())
            .spawn(move || {
                while !stop.load(Ordering::Relaxed) {
                    std::thread::sleep(Duration::from_millis(1000));
                    let client = lock(&client).clone();
                    let next = {
                        let mut processes = lock(&processes);
                        processes.refresh();
                        poll(&processes, &client)
                    };
                    let mut current = lock(&state);
                    if *current != next {
                        *current = next;
                        ctx.request_repaint();
                    }
                }
            })
            .expect("spawning the process poll thread");
        watch
    }

    pub fn state(&self) -> GameState {
        lock(&self.state).clone()
    }

    pub fn set_client(&self, client: PathBuf) {
        let mut processes = lock(&self.processes);
        processes.refresh();
        *lock(&self.state) = poll(&processes, &client);
        *lock(&self.client) = client;
    }

    /// The process table refreshed now, for a mutation's running-game check.
    pub fn fresh(&self) -> MutexGuard<'_, Processes> {
        let mut processes = lock(&self.processes);
        processes.refresh();
        processes
    }
}

impl Drop for ProcessWatch {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }
}

/// Watches a plugins/ directory (and the client dir, so a plugins/ created later is noticed).
pub struct FileWatch {
    _watcher: Option<RecommendedWatcher>,
    dirty: Arc<AtomicBool>,
    pub error: Option<String>,
}

impl FileWatch {
    pub fn start(ctx: egui::Context, client: &Path, extra: Option<&Path>) -> FileWatch {
        let dirty = Arc::new(AtomicBool::new(false));
        let flag = dirty.clone();
        let created = notify::recommended_watcher(move |event: notify::Result<notify::Event>| {
            if event.is_ok_and(|event| !event.kind.is_access()) {
                flag.store(true, Ordering::Relaxed);
                ctx.request_repaint_after(Duration::from_millis(120));
            }
        });
        let mut error = None;
        let watcher = match created {
            Ok(mut watcher) => {
                let plugins = client.join("plugins");
                let target = if plugins.is_dir() { plugins } else { client.to_owned() };
                if target.is_dir() {
                    if let Err(e) = watcher.watch(&target, RecursiveMode::NonRecursive) {
                        error = Some(format!("cannot watch {}: {e}", target.display()));
                    }
                }
                if let Some(extra) = extra.filter(|dir| dir.is_dir()) {
                    let _ = watcher.watch(extra, RecursiveMode::NonRecursive);
                }
                Some(watcher)
            }
            Err(e) => {
                error = Some(format!("file watching unavailable: {e}"));
                None
            }
        };
        FileWatch { _watcher: watcher, dirty, error }
    }

    /// Whether files changed since the last call.
    pub fn take(&self) -> bool {
        self.dirty.swap(false, Ordering::Relaxed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn found(plugins: &[(&str, &str, &[(&str, &str)])]) -> Manifests {
        plugins
            .iter()
            .map(|(name, version, depends)| {
                let depends = depends
                    .iter()
                    .map(|(dependency, range)| manifest::Dependency {
                        name: (*dependency).to_owned(),
                        text: (*range).to_owned(),
                        range: Range::parse(range).unwrap(),
                    })
                    .collect();
                let manifest = Manifest {
                    path: PathBuf::new(),
                    name: (*name).to_owned(),
                    category: "T".to_owned(),
                    version: Version::parse(version).unwrap(),
                    depends,
                    conflicts: Vec::new(),
                };
                ((*name).to_owned(), manifest)
            })
            .collect()
    }

    fn shown(found: &Manifests, disabled: &[&str], status: &str, live: bool, name: &str) -> Health {
        let dlls: Vec<String> = found.keys().cloned().collect();
        let disabled: Vec<String> = disabled.iter().map(|name| (*name).to_owned()).collect();
        let predicted = resolve::predict_from(found, &dlls, &disabled);
        health(found, &predicted, &Status::parse(status), live, name)
    }

    #[test]
    fn ranges_read_the_way_they_were_written() {
        for (comparators, written) in [
            (">=1.2.0 <2.0.0-0", "^1.2.0"),
            (">=0.1.0 <0.2.0-0", "^0.1.0"),
            (">=1.2.3 <1.3.0-0", "~1.2.3"),
            ("=1.0.0", "1.0.0"),
            (">=0.0.0", "any version"),
            (">=1.0.0 <3.0.0-0", "1.0.0 or newer, below 3.0.0"),
            (">=1.0.0 <2.0.0-0 || >=3.0.0 <4.0.0-0", "^1.0.0 or ^3.0.0"),
            (">1.0.0", ">1.0.0"),
        ] {
            assert_eq!(human_range(&Range::parse(comparators).unwrap()), written, "{comparators}");
        }
    }

    #[test]
    fn one_status_merges_the_files_and_the_last_run() {
        let plugins = found(&[("Base", "1.1.0", &[]), ("Effects", "0.1.0", &[("Base", ">=1.0.0 <2.0.0-0")])]);
        let ran = "RoPluginHost\t1.1.0\tpid 1\nBase\t1.1.0\tloaded\t\t\nEffects\t0.1.0\tfailed-init\tRoPlugin_Init returned false\t\n";
        assert_eq!(shown(&plugins, &[], ran, false, "Base"), Health::new(Tone::Good, "Active", None));
        assert_eq!(shown(&plugins, &[], ran, false, "Effects").label, "Failed to start");

        // A version the game has not started with yet is not "Active", whatever the last run recorded.
        let older = "RoPluginHost\t1.1.0\tpid 1\nBase\t1.0.0\tloaded\t\t\n";
        let ready = shown(&plugins, &[], older, false, "Base");
        assert_eq!((ready.tone, ready.label), (Tone::Info, "Ready"));

        let blocked = shown(&plugins, &["Base"], ran, false, "Effects");
        assert_eq!(blocked, Health::new(Tone::Warn, "Blocked", Some("Needs Base, which is turned off.".to_owned())));
        assert_eq!(shown(&plugins, &["Base"], ran, false, "Base"), Health::new(Tone::Off, "Off", None));

        let wrong = found(&[("Base", "0.9.0", &[]), ("Effects", "0.1.0", &[("Base", ">=1.0.0 <2.0.0-0")])]);
        assert_eq!(shown(&wrong, &[], "", false, "Effects").detail.as_deref(), Some("Needs Base ^1.0.0, but 0.9.0 is installed."));
        let alone = found(&[("Effects", "0.1.0", &[("Base", ">=1.0.0 <2.0.0-0")])]);
        assert_eq!(shown(&alone, &[], "", false, "Effects").detail.as_deref(), Some("Needs Base, which isn't installed."));
    }
}
