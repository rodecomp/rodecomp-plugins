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
use ro_plugins_core::resolve::{self, Prediction};
use ro_plugins_core::semver::Version;
use ro_plugins_core::status::{self, Freshness, GameProcess, Processes, Status};

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
        Snapshot { client, found, errors, status, predicted, disabled, freshness, names }
    }

    pub fn is_off(&self, name: &str) -> bool {
        manifest::is_disabled(&self.disabled, name)
    }

    pub fn dependents(&self, name: &str) -> Vec<String> {
        resolve::dependents(&self.found, name)
    }

    pub fn mismatches(&self) -> usize {
        self.predicted
            .iter()
            .filter(|(name, p)| self.status.plugins.get(*name).is_some_and(|r| r.state != p.state.as_str()))
            .count()
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
