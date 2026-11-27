//! The window: a quiet top bar (game exe, one state chip, run, install, settings), the plugin list on the left and the
//! selected plugin on the right. Each plugin shows one status in plain words; what the host recorded and the raw
//! files sit under "Technical details". Changes go through the same core rules as the CLI. It repaints only on
//! input, file events and process changes.

mod model;
mod search;
mod theme;

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use eframe::egui::{
    self, Align, Align2, Color32, CornerRadius, CursorIcon, FontId, Id, Key, Layout, Margin, Modifiers, Rect, RichText,
    ScrollArea, Sense, Shadow, Stroke, StrokeKind, Ui, Vec2,
};
use egui_phosphor::regular as icon;
use ro_plugins_core::manifest;
use ro_plugins_core::settings::{self, Choice, Element, Kind, PathKind, Setting, Value, display_order};
use ro_plugins_core::exe::{ExeKind, exe_kind};
use ro_plugins_core::ops::{Change, Client, EXIT_RUNNING, Outcome};
use ro_plugins_core::setup::{self as exe_setup, PluginSet};
use ro_plugins_core::resolve::{self, State};
use ro_plugins_core::status::{self, PendingChange, Processes};

use crate::elevate::{self, Op};
use crate::i18n::{self, Lang};
use crate::paths;
use model::{BuildListing, Failure, FileWatch, GameState, Health, ProcessWatch, Snapshot, Tone};
use search::{Hit, Listing};

/// The client folder; kept so a folder without a known exe still opens (the window then asks for the exe).
const KEY_CLIENT: &str = "client";
/// The chosen game exe; the client folder is its parent.
const KEY_CLIENT_EXE: &str = "client_exe";
const KEY_BUILD: &str = "build";
const LIST_WIDTH: f32 = 288.0;
const KEY_SHOW: &str = "show";
/// The game folders whose recommended plugins were already turned on, one per line.
const KEY_SEEN: &str = "seen_clients";
/// The language the user picked in the window (`pt-BR`, `en`…); empty while the system's language is followed.
const KEY_LANGUAGE: &str = "language";
/// How long the inline "Saved" tick stays, the last part of it fading.
const SAVED_FOR: Duration = Duration::from_millis(1500);
const CONTENT_WIDTH: f32 = 720.0;

/// `exe` (from `--exe`) and `client` (from `--client`) are exclusive; either wins over the stored settings.
pub fn run(client: Option<PathBuf>, exe: Option<PathBuf>, build: Option<PathBuf>) -> Result<(), String> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("RO Plugins")
            .with_app_id("ro-plugins")
            .with_inner_size([1080.0, 700.0])
            .with_min_inner_size([860.0, 520.0]),
        centered: true,
        ..Default::default()
    };
    eframe::run_native(
        "ro-plugins",
        options,
        Box::new(move |cc| {
            theme::install(&cc.egui_ctx);
            let stored = |key| cc.storage.and_then(|s| s.get_string(key)).filter(|s| !s.is_empty()).map(PathBuf::from);
            let absolute = |p: PathBuf| if p.as_os_str().is_empty() { p } else { std::path::absolute(&p).unwrap_or(p) };
            // A path on the command line wins over the stored setting, which wins over the defaults. A folder without
            // a stored exe (the old setting, --client, the default) gets the exe status.txt names or Ragexe.exe, if
            // present; otherwise the exe stays unset and the window asks for it.
            let stored_exe = stored(KEY_CLIENT_EXE).map(absolute);
            let (client, exe) = match exe.map(absolute) {
                Some(exe) => (model::client_of(&exe), exe),
                None => {
                    let client = client.or_else(|| stored_exe.as_deref().map(model::client_of)).or_else(|| stored(KEY_CLIENT));
                    let client = absolute(client.or_else(paths::default_client).unwrap_or_default());
                    let same = |exe: &PathBuf| status::comparable(&model::client_of(exe)) == status::comparable(&client);
                    let exe = stored_exe.filter(same).or_else(|| paths::exe_in(&client)).unwrap_or_default();
                    (client, exe)
                }
            };
            let release = paths::default_build().filter(|dir| dir.ends_with(paths::RELEASE_BUILD));
            // A stored folder that is gone (a deleted or moved release) falls back to the defaults instead of failing.
            let stored_build = stored(KEY_BUILD).filter(|dir| dir.is_dir());
            let build = absolute(build.or(release).or(stored_build).or_else(paths::default_build).unwrap_or_default());
            let text = |key| cc.storage.and_then(|s| s.get_string(key)).unwrap_or_default();
            // The language comes first, so the first texts (plugin statuses, toasts) are already in it: the user's
            // choice from an earlier run, else the system's language when supported, else English.
            let language = Lang::from_code(&text(KEY_LANGUAGE));
            let system = i18n::system_language();
            i18n::set(i18n::initial(language, system));
            let mut app = App::new(cc.egui_ctx.clone(), client, exe, build);
            app.language = language;
            app.system_language = system;
            app.show = Show::from_key(&text(KEY_SHOW));
            app.seen = text(KEY_SEEN).lines().filter(|line| !line.is_empty()).map(str::to_owned).collect();
            app.first_open();
            Ok(Box::new(app))
        }),
    )
    .map_err(|error| error.to_string())
}

struct InstallForm {
    checked: BTreeSet<String>,
    force: bool,
}

enum Page {
    Plugins,
    Install(InstallForm),
}

/// The plugin list's view: every plugin, the ones turned on, or the recommended ones.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Show {
    All,
    On,
    Recommended,
}

impl Show {
    fn key(self) -> &'static str {
        match self {
            Show::All => "all",
            Show::On => "on",
            Show::Recommended => "recommended",
        }
    }

    fn from_key(key: &str) -> Show {
        match key {
            "on" => Show::On,
            "recommended" => Show::Recommended,
            _ => Show::All,
        }
    }
}

struct UninstallDialog {
    name: String,
    dependents: Vec<String>,
}

/// The confirmation of setup, update or restore.
struct SetupDialog {
    op: Op,
}

/// The chosen exe: whether it loads plugins, its backup, and why it should be updated from the build output.
#[derive(Default)]
struct ExeInfo {
    kind: Option<ExeKind>,
    backup: Option<PathBuf>,
    update: Option<String>,
}

struct Toast {
    text: String,
    tone: Tone,
    /// disabled.txt as it was before the change.
    undo: Option<Vec<String>>,
    until: Instant,
}

/// What the last save of one row did: the plugin's switch (`key` None) or one setting.
struct Feedback {
    error: Option<String>,
    at: Instant,
}

type RowId = (String, Option<String>);

/// A long operation running off the UI thread; the window shows a modal until it ends.
struct Job {
    title: String,
    kind: JobKind,
    /// Set by the worker: Ok, or the error as a sentence.
    result: Arc<Mutex<Option<Result<(), String>>>>,
}

enum JobKind {
    /// The plugins asked for and how many the plan installs.
    Install(Vec<String>, usize),
    Setup(Op),
}

enum Action {
    Select(String),
    SetOff(String, bool),
    RestoreDisabled(Vec<String>),
    /// Opens the install page; with a name, only that plugin is ticked.
    OpenInstall(Option<String>),
    Install(Vec<String>, bool),
    AskUninstall(String),
    Uninstall(String),
    OpenFolder(PathBuf),
    PickExe,
    Run,
    PickBuild,
    Reload,
    AskSetup(Op),
    RunSetup(Op),
    /// Writes a setting's value (`None`: back to the default).
    SetValue(String, String, Option<Value>),
    /// Picks a path for a setting; with an index, for that list element (one past the end adds one).
    PickPath(String, String, Option<usize>),
    /// Switches the window's language and remembers it (`None`: follow the system's language again).
    SetLanguage(Option<Lang>),
}

struct App {
    ctx: egui::Context,
    /// The client folder: the exe's folder, or a folder whose exe is not chosen yet.
    client: PathBuf,
    /// The chosen game exe; empty when none is chosen.
    exe: PathBuf,
    build: PathBuf,
    snapshot: Option<Snapshot>,
    build_listing: Option<BuildListing>,
    /// Setting values being edited and not written yet, by (plugin, key).
    drafts: BTreeMap<(String, String), Value>,
    game: GameState,
    processes: ProcessWatch,
    files: FileWatch,
    toast: Option<Toast>,
    selected: Option<String>,
    search: String,
    focus_search: bool,
    /// Expanded categories of the plugin list, by title; all collapsed at start-up.
    expanded: BTreeSet<String>,
    /// Which plugins the list shows (plugins that need attention always show).
    show: Show,
    /// Game folders already opened once: their recommended plugins are not turned on again.
    seen: BTreeSet<String>,
    /// The inline result of the last save of each row.
    feedback: BTreeMap<RowId, Feedback>,
    /// The failures the banner was dismissed for; it comes back when they change.
    dismissed: Option<Vec<Failure>>,
    job: Option<Job>,
    page: Page,
    uninstall: Option<UninstallDialog>,
    setup: Option<SetupDialog>,
    exe_info: ExeInfo,
    /// The language the user picked; `None` follows the system's language.
    language: Option<Lang>,
    /// The system's language when supported, read once at start.
    system_language: Option<Lang>,
    actions: Vec<Action>,
}

fn search_id() -> Id {
    Id::new("plugin-search")
}

fn folder_name(path: &Path) -> String {
    path.file_name().map_or_else(|| path.display().to_string(), |name| name.to_string_lossy().into_owned())
}

/// How many `*.exe` files a folder holds.
fn exe_count(dir: &Path) -> usize {
    let Ok(entries) = std::fs::read_dir(dir) else { return 0 };
    entries
        .flatten()
        .filter(|entry| entry.path().extension().is_some_and(|ext| ext.eq_ignore_ascii_case("exe")) && entry.path().is_file())
        .count()
}

/// Starts the game detached: its own folder as working directory, no console, no inherited std handles, not waited
/// for.
fn launch(exe: &Path) -> std::io::Result<()> {
    let mut command = std::process::Command::new(exe);
    command
        .current_dir(model::client_of(exe))
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // winbase.h: no console for the game, and Ctrl+C in a parent console does not reach it.
        const DETACHED_PROCESS: u32 = 0x0000_0008;
        const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;
        command.creation_flags(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP);
    }
    command.spawn().map(drop)
}

/// `first, second and third`.
fn list(names: &[String]) -> String {
    match names {
        [] => String::new(),
        [one] => one.clone(),
        [head @ .., last] => t!("common.list", head = head.join(", "), last = last),
    }
}

fn plugins(count: usize) -> String {
    tn!("plugins", count)
}

/// A core refusal as a sentence for the window.
fn sentence(message: &str) -> String {
    let text = message.strip_prefix("refused: ").unwrap_or(message);
    let mut chars = text.chars();
    chars.next().map_or_else(String::new, |first| first.to_uppercase().chain(chars).collect())
}

/// A core refusal for the window: the running game in plain words, anything else as a sentence.
fn refusal(refused: &ro_plugins_core::ops::Refused) -> String {
    if refused.code == EXIT_RUNNING { t!("common.close_game_first_long").to_owned() } else { sentence(&refused.message) }
}

impl App {
    fn new(ctx: egui::Context, client: PathBuf, exe: PathBuf, build: PathBuf) -> App {
        let processes = ProcessWatch::start(ctx.clone(), client.clone(), exe.clone());
        let build_plugins = paths::build_plugins(&build);
        let files = FileWatch::start(ctx.clone(), &client, Some(&build_plugins));
        let mut app = App {
            ctx,
            client,
            exe,
            build,
            snapshot: None,
            build_listing: None,
            drafts: BTreeMap::new(),
            game: processes.state(),
            processes,
            files,
            toast: None,
            selected: None,
            search: String::new(),
            focus_search: false,
            expanded: BTreeSet::new(),
            show: Show::All,
            seen: BTreeSet::new(),
            feedback: BTreeMap::new(),
            dismissed: None,
            job: None,
            page: Page::Plugins,
            uninstall: None,
            setup: None,
            exe_info: ExeInfo::default(),
            language: None,
            system_language: None,
            actions: Vec::new(),
        };
        app.reload();
        app
    }

    fn client_ok(&self) -> bool {
        !self.client.as_os_str().is_empty() && self.client.is_dir()
    }

    fn running(&self) -> bool {
        !self.game.running.is_empty()
    }

    /// The exe is known to have no plugin loader: switches, settings and Run wait for setup.
    fn no_loader(&self) -> bool {
        matches!(self.exe_info.kind, Some(ExeKind::NoLoader))
    }

    /// Changes are locked: the game runs, or the exe can't load plugins yet.
    fn locked(&self) -> bool {
        self.running() || self.no_loader()
    }

    fn read_exe_info(&self) -> ExeInfo {
        if self.exe.as_os_str().is_empty() || !self.exe.is_file() {
            return ExeInfo::default();
        }
        let kind = exe_kind(&self.exe);
        let backup = Some(exe_setup::backup_of(&self.exe)).filter(|path| path.is_file());
        let update = match (&kind, PluginSet::find(&self.build), &self.snapshot) {
            (ExeKind::PluginBuild, Ok(set), Some(snapshot)) => exe_setup::update_reason(&self.exe, &set, &snapshot.status),
            _ => None,
        };
        ExeInfo { kind: Some(kind), backup, update }
    }

    fn reload(&mut self) {
        self.snapshot = self.client_ok().then(|| Snapshot::read(&self.client, self.game.recorded_image.as_deref()));
        self.exe_info = self.read_exe_info();
        let installed = self.snapshot.as_ref().map(|s| s.found.clone()).unwrap_or_default();
        let build_plugins = paths::build_plugins(&self.build);
        self.build_listing =
            (!self.build.as_os_str().is_empty() && build_plugins.is_dir()).then(|| BuildListing::read(&build_plugins, &installed));
        if let (Some(snapshot), Some(selected)) = (&self.snapshot, &self.selected) {
            if !snapshot.names.contains(selected) {
                self.selected = None;
            }
        }
    }

    /// Selects a plugin and expands its category, so the selection is always visible.
    fn select(&mut self, name: String) {
        if let Some(category) = self.snapshot.as_ref().and_then(|s| s.found.get(&name)).map(|m| m.category.clone()) {
            self.expanded.insert(category);
        }
        self.selected = Some(name);
    }

    fn set_exe(&mut self, exe: PathBuf) {
        self.client = model::client_of(&exe);
        self.exe = exe;
        self.selected = None;
        self.page = Page::Plugins;
        self.processes.set_target(self.client.clone(), self.exe.clone());
        self.game = self.processes.state();
        self.files = FileWatch::start(self.ctx.clone(), &self.client, Some(&paths::build_plugins(&self.build)));
        self.reload();
        self.first_open();
    }

    /// The first time a game folder that loads plugins is opened: every recommended plugin is turned on, with its
    /// dependencies, and the toast offers Undo. Later opens leave the user's switches alone. A folder whose exe can't
    /// load plugins yet, or whose game is running, waits for a later open.
    fn first_open(&mut self) {
        let key = self.client.to_string_lossy().to_lowercase();
        if key.is_empty() || self.seen.contains(&key) || self.running() || !matches!(self.exe_info.kind, Some(ExeKind::PluginBuild)) {
            return;
        }
        let Some(snapshot) = &self.snapshot else { return };
        let before = snapshot.disabled.clone();
        let off: Vec<String> = snapshot.recommended.iter().filter(|name| snapshot.is_off(name)).cloned().collect();
        self.seen.insert(key);
        if off.is_empty() {
            return;
        }
        let mut failed = None;
        for name in &off {
            if let Err(refused) = self.run_op(|client, processes, out| client.enable(name, processes, out)) {
                failed = Some(refused);
                break;
            }
        }
        self.reload();
        match failed {
            None => self.notify(Tone::Good, t!("toast.recommended_on", names = list(&off)), Some(before)),
            Some(why) => self.notify(Tone::Bad, t!("toast.recommended_failed", why = why), Some(before)),
        }
    }

    fn set_build(&mut self, build: PathBuf) {
        self.build = build;
        self.files = FileWatch::start(self.ctx.clone(), &self.client, Some(&paths::build_plugins(&self.build)));
        self.reload();
    }

    /// The list as shown: plugins that need attention first, then the category tree, filtered by the search and the
    /// All / On / Recommended switch.
    fn visible(&self) -> Listing {
        let Some(snapshot) = &self.snapshot else { return Listing::default() };
        Listing::build(
            &self.search,
            &snapshot.names,
            |name| snapshot.health[name].needs_attention(),
            |name| match self.show {
                Show::All => true,
                Show::On => snapshot.found.contains_key(name) && !snapshot.is_off(name),
                Show::Recommended => snapshot.is_recommended(name),
            },
            |name| snapshot.found.get(name).map(|manifest| (manifest.category.as_str(), manifest.description.as_str())),
        )
    }

    /// Whether a category shows its plugins: always while searching, else when expanded.
    fn is_open(&self, title: &str) -> bool {
        !self.search.trim().is_empty() || self.expanded.contains(title)
    }

    /// The shown rows in order, for selection and the keyboard.
    fn visible_order(&self) -> Vec<String> {
        self.visible().order(|group| self.is_open(group.title()))
    }

    fn saved(&mut self, row: RowId, result: &Result<(), String>) {
        self.feedback.insert(row, Feedback { error: result.clone().err(), at: Instant::now() });
    }

    fn notify(&mut self, tone: Tone, text: impl Into<String>, undo: Option<Vec<String>>) {
        let seconds = if tone == Tone::Bad { 8 } else { 6 };
        self.toast = Some(Toast { text: text.into(), tone, undo, until: Instant::now() + Duration::from_secs(seconds) });
    }

    /// Runs a core operation against a fresh process table and reloads; a refusal comes back as a sentence.
    fn run_op(&mut self, op: impl FnOnce(&Client, &Processes, &mut Vec<String>) -> Outcome) -> Result<(), String> {
        let Some(snapshot) = &self.snapshot else { return Err(t!("error.choose_exe_first").to_owned()) };
        let client = snapshot.client.clone();
        let mut out = Vec::new();
        let outcome = {
            let processes = self.processes.fresh();
            op(&client, &processes, &mut out)
        };
        self.reload();
        outcome.map(drop).map_err(|refused| refusal(&refused))
    }

    fn set_off(&mut self, name: String, off: bool) {
        let Some(snapshot) = &self.snapshot else { return };
        let before = snapshot.disabled.clone();
        let text = if off {
            let blocked: Vec<String> = snapshot.dependents(&name).into_iter().filter(|user| !snapshot.is_off(user)).collect();
            if blocked.is_empty() { t!("toast.turned_off", name = name) } else { t!("toast.turned_off_blocks", name = name, names = list(&blocked)) }
        } else {
            let also: Vec<String> =
                resolve::requirements(&snapshot.found, &name).into_iter().filter(|needed| *needed != name && snapshot.is_off(needed)).collect();
            if also.is_empty() { t!("toast.turned_on", name = name) } else { t!("toast.turned_on_with", name = name, names = list(&also)) }
        };
        let result = self.run_op(|client, processes, out| if off { client.disable(&name, processes, out) } else { client.enable(&name, processes, out) });
        self.saved((name.clone(), None), &result);
        if result.is_ok() {
            self.notify(Tone::Good, text, Some(before));
        }
    }

    /// Writes a setting; the row shows "Saved" or the error inline.
    fn set_value(&mut self, name: String, key: String, value: Option<Value>) {
        let item = value.as_ref().map(|value| toml_edit::Item::Value(value.to_toml()));
        let result = self.run_op(|client, processes, out| client.set_value(&name, &key, item.as_ref(), processes, out));
        self.saved((name, Some(key)), &result);
    }

    /// Starts a long operation on a worker thread; `work` returns Ok or the error as a sentence.
    fn start_job(&mut self, title: String, kind: JobKind, work: impl FnOnce() -> Result<(), String> + Send + 'static) {
        let result = Arc::new(Mutex::new(None));
        let (slot, ctx) = (result.clone(), self.ctx.clone());
        std::thread::spawn(move || {
            let outcome = work();
            *slot.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(outcome);
            ctx.request_repaint();
        });
        self.job = Some(Job { title, kind, result });
    }

    /// Ends the running job once its worker is done: reload and report.
    fn finish_job(&mut self) {
        let Some(job) = &self.job else { return };
        let Some(outcome) = job.result.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).take() else { return };
        let Some(job) = self.job.take() else { return };
        self.reload();
        match (job.kind, outcome) {
            (_, Err(error)) => self.notify(Tone::Bad, error, None),
            (JobKind::Install(names, planned), Ok(())) => {
                self.page = Page::Plugins;
                if let Some(first) = names.first() {
                    self.select(first.clone());
                }
                let text = if planned == 1 { t!("toast.installed", name = names[0]) } else { t!("toast.installed", name = plugins(planned)) };
                self.notify(Tone::Good, text, None);
            }
            (JobKind::Setup(op), Ok(())) => {
                let text = match op {
                    Op::Setup => t!("toast.setup_done", name = folder_name(&self.exe)),
                    Op::Update => t!("toast.update_done").to_owned(),
                    Op::Restore => t!("toast.restore_done", name = folder_name(&self.exe)),
                };
                self.notify(Tone::Good, text, None);
                self.first_open();
            }
        }
    }

    fn pick_path(&mut self, name: String, key: String, element: Option<usize>) {
        let Some(setting) = self.snapshot.as_ref().and_then(|s| s.found.get(&name)).and_then(|m| m.setting(&key)).cloned() else { return };
        let kind = match &setting.kind {
            Kind::Path(kind) | Kind::List(Element::Path(kind)) => kind,
            _ => return,
        };
        let Some(picked) = pick(kind, &self.client, &setting.label) else { return };
        let picked = Value::Str(settings::client_relative(&self.client, &picked));
        let id = (name.clone(), key.clone());
        let value = match (element, self.drafts.remove(&id).unwrap_or_else(|| setting.current().clone())) {
            (Some(index), Value::List(mut items)) => {
                if index < items.len() {
                    items[index] = picked;
                } else {
                    items.push(picked);
                }
                Value::List(items)
            }
            _ => picked,
        };
        self.set_value(name, key, Some(value));
    }

    fn apply(&mut self, action: Action) {
        match action {
            Action::Select(name) => {
                self.select(name);
                self.page = Page::Plugins;
            }
            Action::SetOff(name, off) => self.set_off(name, off),
            Action::RestoreDisabled(names) => {
                let result = if self.running() {
                    Err(t!("common.close_game_first_long").to_owned())
                } else {
                    manifest::write_disabled(&Client::new(&self.client).plugins, &names).map_err(|error| t!("error.undo_failed", error = error))
                };
                self.reload();
                match result {
                    Ok(()) => self.notify(Tone::Good, t!("toast.undone"), None),
                    Err(error) => self.notify(Tone::Bad, error, None),
                }
            }
            Action::OpenInstall(only) => {
                let checked = match only {
                    Some(name) => BTreeSet::from([name]),
                    None => self
                        .build_listing
                        .iter()
                        .flat_map(|listing| &listing.plugins)
                        .filter(|available| available.change() != Change::Same)
                        .map(|available| available.manifest.name.clone())
                        .collect(),
                };
                self.page = Page::Install(InstallForm { checked, force: false });
            }
            Action::Install(names, force) => {
                let Some(snapshot) = &self.snapshot else { return };
                let source = paths::build_plugins(&self.build);
                let client = snapshot.client.clone();
                let planned = client.plan_install(&source, &names).ok().map_or(names.len(), |plan| plan.items.len());
                let title = if planned == 1 { t!("job.installing", what = names[0]) } else { t!("job.installing", what = plugins(planned)) };
                let wanted = names.clone();
                self.start_job(title, JobKind::Install(names, planned), move || {
                    let processes = Processes::new();
                    client.install(&source, &wanted, force, &processes, &mut Vec::new()).map(drop).map_err(|refused| refusal(&refused))
                });
            }
            Action::AskUninstall(name) => {
                let dependents = self.snapshot.as_ref().map(|s| s.dependents(&name)).unwrap_or_default();
                self.uninstall = Some(UninstallDialog { name, dependents });
            }
            Action::Uninstall(name) => {
                let others = self.snapshot.as_ref().map(|s| s.dependents(&name)).unwrap_or_default();
                match self.run_op(|client, processes, out| client.uninstall(&name, !others.is_empty(), processes, out)) {
                    Ok(()) if others.is_empty() => self.notify(Tone::Good, t!("toast.uninstalled", name = name), None),
                    Ok(()) => self.notify(Tone::Good, t!("toast.uninstalled_with", name = name, names = list(&others)), None),
                    Err(error) => self.notify(Tone::Bad, error, None),
                }
            }
            Action::OpenFolder(path) => {
                if let Err(error) = crate::open::open_folder(&path) {
                    self.notify(Tone::Bad, t!("error.open_folder", error = error), None);
                }
            }
            Action::PickExe => {
                let mut dialog = rfd::FileDialog::new().set_title(t!("dialog.choose_exe")).add_filter(t!("dialog.exe_filter"), &["exe"]);
                if self.client.is_dir() {
                    dialog = dialog.set_directory(&self.client);
                }
                if let Some(exe) = dialog.pick_file() {
                    self.set_exe(exe);
                }
            }
            Action::Run => {
                if let Err(error) = launch(&self.exe) {
                    self.notify(Tone::Bad, t!("error.start_failed", name = folder_name(&self.exe), error = error), None);
                }
                // Show it as running without waiting for the next poll.
                self.processes.set_target(self.client.clone(), self.exe.clone());
                self.game = self.processes.state();
            }
            Action::PickBuild => {
                let mut dialog = rfd::FileDialog::new().set_title(t!("dialog.choose_build"));
                if self.build.is_dir() {
                    dialog = dialog.set_directory(&self.build);
                }
                if let Some(dir) = dialog.pick_folder() {
                    self.set_build(dir);
                }
            }
            Action::Reload => self.reload(),
            Action::AskSetup(op) => self.setup = Some(SetupDialog { op }),
            Action::RunSetup(op) => self.run_setup(op),
            Action::SetValue(name, key, value) => self.set_value(name, key, value),
            Action::PickPath(name, key, index) => self.pick_path(name, key, index),
            Action::SetLanguage(choice) => {
                self.language = choice;
                i18n::set(i18n::initial(choice, self.system_language));
                // Statuses and their details are worded when the snapshot is read: read it again in the new language.
                self.reload();
            }
        }
    }

    /// Up/Down move through the list, Space toggles, Ctrl+F searches, Esc backs out.
    fn keyboard(&mut self, ctx: &egui::Context) {
        if self.uninstall.is_some() || self.setup.is_some() {
            return;
        }
        let typing = ctx.memory(|memory| memory.focused() == Some(search_id()));
        if ctx.input_mut(|input| input.consume_key(Modifiers::COMMAND, Key::F)) {
            self.page = Page::Plugins;
            self.focus_search = true;
        }
        if ctx.input_mut(|input| input.consume_key(Modifiers::NONE, Key::Escape)) {
            if matches!(self.page, Page::Install(_)) {
                self.page = Page::Plugins;
            } else if !self.search.is_empty() {
                self.search.clear();
            } else if typing {
                ctx.memory_mut(|memory| memory.surrender_focus(search_id()));
            }
        }
        if !matches!(self.page, Page::Plugins) {
            return;
        }
        let down = ctx.input_mut(|input| input.consume_key(Modifiers::NONE, Key::ArrowDown));
        let up = ctx.input_mut(|input| input.consume_key(Modifiers::NONE, Key::ArrowUp));
        if down != up {
            let names = self.visible_order();
            if !names.is_empty() {
                let current = self.selected.as_ref().and_then(|selected| names.iter().position(|name| name == selected));
                let next = match (current, down) {
                    (None, _) => 0,
                    (Some(index), true) => (index + 1).min(names.len() - 1),
                    (Some(index), false) => index.saturating_sub(1),
                };
                self.selected = Some(names[next].clone());
            }
        }
        if !typing && !self.locked() && ctx.input_mut(|input| input.consume_key(Modifiers::NONE, Key::Space)) {
            if let (Some(snapshot), Some(name)) = (&self.snapshot, &self.selected) {
                if snapshot.found.contains_key(name) {
                    self.actions.push(Action::SetOff(name.clone(), !snapshot.is_off(name)));
                }
            }
        }
    }

    // ---- top bar ---------------------------------------------------------------------------------------------

    fn top_bar(&mut self, ui: &mut Ui) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 8.0;
            ui.label(RichText::new(icon::PUZZLE_PIECE).size(18.0).color(theme::ACCENT));
            ui.label(RichText::new(t!("top.plugins")).font(FontId::new(14.5, theme::semibold())).color(theme::TEXT));
            ui.add_space(12.0);

            self.exe_selector(ui);

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 8.0;
                self.settings_menu(ui);
                self.run_button(ui);
                let Some(snapshot) = &self.snapshot else { return };
                // Changes made while the game runs reach it only at the next start.
                let changes = &snapshot.pending_changes;
                if !self.game.running.is_empty() && !changes.is_empty() {
                    let text = tn!("top.restart", changes.len());
                    theme::chip(ui, &format!("{}  {text}", icon::ARROWS_CLOCKWISE), theme::BLUE).on_hover_ui(|ui| {
                        ui.label(t!("top.restart.tooltip"));
                        for change in changes {
                            ui.label(RichText::new(format!("•  {}", change_text(change))).color(theme::SOFT));
                        }
                    });
                }
                if matches!(self.page, Page::Plugins) {
                    let button = theme::secondary(format!("{}  {}", icon::PLUS, t!("top.install")));
                    if ui.add(button).on_hover_cursor(CursorIcon::PointingHand).on_hover_text(t!("top.install.hint")).clicked() {
                        self.actions.push(Action::OpenInstall(None));
                    }
                }
                // Only the lock needs saying: it explains why the switches are disabled.
                if let Some(process) = self.game.running.first() {
                    theme::chip(ui, &format!("{}  {}", icon::LOCK_SIMPLE, t!("top.running_locked")), theme::AMBER).on_hover_ui(|ui| {
                        ui.label(t!("common.running_pid", name = process.name(), pid = process.pid));
                        for note in &snapshot.freshness.notes {
                            ui.label(RichText::new(note).size(12.0).color(theme::MUTED));
                        }
                    });
                }
            });
        });
    }

    /// `Ragexe.exe — fullclient` opens the file dialog to choose another game; beside it, the game's state as a chip
    /// that is itself the action when there is one: `Update available` opens the update dialog, `Plugins off` the
    /// turn-on dialog.
    fn exe_selector(&mut self, ui: &mut Ui) {
        if self.exe.as_os_str().is_empty() {
            let text = RichText::new(format!("{}  {}", icon::FOLDER, t!("exe.choose"))).color(theme::ACCENT);
            let button = egui::Button::new(text).fill(theme::SURFACE).corner_radius(CornerRadius::same(8));
            if ui.add(button).on_hover_cursor(CursorIcon::PointingHand).on_hover_text(t!("exe.choose.hint")).clicked() {
                self.actions.push(Action::PickExe);
            }
            return;
        }
        let name = folder_name(&self.exe);
        let text = RichText::new(format!("{}  {name}  —  {}", icon::FOLDER, folder_name(&self.client))).color(theme::SOFT);
        let button = egui::Button::new(text).fill(theme::SURFACE).corner_radius(CornerRadius::same(8));
        if ui.add(button).on_hover_cursor(CursorIcon::PointingHand).on_hover_text(t!("exe.hint", path = self.exe.display())).clicked() {
            self.actions.push(Action::PickExe);
        }

        let locked = self.running();
        let mut action = |ui: &mut Ui, text: String, color: Color32, hint: String, op: Op| {
            let response = ui.add_enabled(!locked, theme::chip_button(&text, color));
            if response.on_hover_text(hint).on_disabled_hover_text(t!("common.close_game_first")).clicked() {
                self.actions.push(Action::AskSetup(op));
            }
        };
        match &self.exe_info.kind {
            Some(ExeKind::PluginBuild) => match &self.exe_info.update {
                Some(_) => action(ui, format!("{}  {}", icon::ARROW_CIRCLE_UP, t!("exe.update_available")), theme::AMBER, t!("exe.update_available.hint").to_owned(), Op::Update),
                None => {
                    theme::chip(ui, &format!("{}  {}", icon::CHECK_CIRCLE, t!("exe.plugins_on")), theme::GREEN).on_hover_text(t!("exe.plugins_on.hint", name = name));
                }
            },
            Some(ExeKind::NoLoader) => action(ui, format!("{}  {}", icon::WARNING, t!("exe.plugins_off")), theme::AMBER, t!("exe.plugins_off.hint").to_owned(), Op::Setup),
            Some(ExeKind::NotAnExe(why)) => {
                theme::chip(ui, &format!("{}  {}", icon::WARNING, t!("exe.not_a_game")), theme::RED).on_hover_text(why.clone());
            }
            None => {}
        }
    }

    /// Starts the chosen exe: the one filled accent button. Shows "Running" instead while that exe (not another one of
    /// the folder) runs.
    fn run_button(&mut self, ui: &mut Ui) {
        if let Some(process) = self.game.launched.first() {
            let button = egui::Button::new(RichText::new(format!("{}  {}", icon::PLAY, t!("run.running"))).color(theme::GREEN)).fill(theme::SURFACE).corner_radius(CornerRadius::same(8));
            ui.add_enabled(false, button).on_disabled_hover_text(t!("common.running_pid", name = process.name(), pid = process.pid));
            return;
        }
        let missing = if self.exe.as_os_str().is_empty() {
            Some(t!("run.no_exe").to_owned())
        } else if !self.exe.is_file() {
            Some(t!("run.missing", path = self.exe.display()))
        } else if self.no_loader() {
            Some(t!("run.no_loader", name = folder_name(&self.exe)))
        } else {
            None
        };
        // The accent marks the one thing to do next; while Run can't run, that's elsewhere (e.g. Set up plugins).
        let label = format!("{}  {}", icon::PLAY, t!("run.button"));
        let button = if missing.is_none() { theme::primary(label) } else { theme::secondary(label) };
        let response = ui.add_enabled(missing.is_none(), button);
        match missing {
            Some(reason) => {
                response.on_disabled_hover_text(reason);
            }
            None => {
                if response.on_hover_cursor(CursorIcon::PointingHand).on_hover_text(t!("run.hint", path = self.exe.display())).clicked() {
                    self.actions.push(Action::Run);
                }
            }
        }
    }

    /// The gear menu: a standard menu, one line and one click per item. Safe items first, the destructive one (go back
    /// to the original game) last and in red; the paths each item acts on are in its tooltip.
    fn settings_menu(&mut self, ui: &mut Ui) {
        let gear = RichText::new(icon::GEAR_SIX).size(16.0).color(theme::MUTED);
        let can_go_back = matches!(self.exe_info.kind, Some(ExeKind::PluginBuild)) && self.exe_info.backup.is_some();
        let running = self.running();
        let several_exes = exe_count(&self.client) > 1;
        let mut picked = None;
        let mut reload = false;
        let mut go_back = false;
        let mut language = None;
        let system = self.system_language.unwrap_or(Lang::FALLBACK);
        let chosen = self.language;
        ui.menu_button(gear, |ui| {
            ui.set_width(250.0);
            ui.spacing_mut().item_spacing.y = 0.0;
            let item = |ui: &mut Ui, glyph: &str, text: &str, color: Color32, enabled: bool| {
                let button = egui::Button::new(RichText::new(format!("{glyph}   {text}")).size(13.5).color(color))
                    .frame_when_inactive(false)
                    .min_size(Vec2::new(ui.available_width(), 30.0));
                ui.add_enabled(enabled, button).on_hover_cursor(CursorIcon::PointingHand)
            };
            let shown = |path: &Path| if path.as_os_str().is_empty() { t!("common.not_chosen").to_owned() } else { path.display().to_string() };

            let mut game_hint = t!("common.now", path = shown(&self.exe));
            if several_exes {
                game_hint.push('\n');
                game_hint.push_str(t!("menu.several_exes"));
            }
            if item(ui, icon::GAME_CONTROLLER, t!("menu.choose_game"), theme::SOFT, true).on_hover_text(game_hint).clicked() {
                picked = Some(Action::PickExe);
            }
            if item(ui, icon::FOLDER_OPEN, t!("menu.choose_build"), theme::SOFT, true).on_hover_text(t!("common.now", path = shown(&self.build))).clicked() {
                picked = Some(Action::PickBuild);
            }
            ui.add_space(3.0);
            ui.separator();
            ui.add_space(3.0);
            if item(ui, icon::ARROWS_CLOCKWISE, t!("menu.reload"), theme::SOFT, true).on_hover_text(t!("menu.reload.hint")).clicked() {
                reload = true;
            }
            // Language: a submenu with the system's language first, then each language in its own name; the one in
            // use carries a check.
            let globe = RichText::new(format!("{}   {}", icon::TRANSLATE, t!("menu.language"))).size(13.5).color(theme::SOFT);
            ui.menu_button(globe, |ui| {
                ui.set_width(240.0);
                ui.spacing_mut().item_spacing.y = 0.0;
                let options = std::iter::once((None, t!("menu.language.system", language = system.native_name())))
                    .chain(Lang::ALL.into_iter().map(|lang| (Some(lang), lang.native_name().to_owned())));
                for (option, label) in options {
                    let mark = if option == chosen { icon::CHECK } else { " " };
                    let button = egui::Button::new(RichText::new(format!("{mark}   {label}")).size(13.5).color(theme::SOFT))
                        .frame_when_inactive(false)
                        .min_size(Vec2::new(ui.available_width(), 30.0));
                    if ui.add(button).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                        language = Some(option);
                    }
                }
            });
            if can_go_back {
                ui.add_space(3.0);
                ui.separator();
                ui.add_space(3.0);
                let response = item(ui, icon::ARROW_COUNTER_CLOCKWISE, t!("menu.go_back"), theme::RED, !running);
                if response.on_hover_text(t!("menu.go_back.hint")).on_disabled_hover_text(t!("common.close_game_first")).clicked() {
                    go_back = true;
                }
            }
            ui.add_space(4.0);
            ui.separator();
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.add_space(4.0);
                ui.label(RichText::new(format!("RO Plugins {}", env!("CARGO_PKG_VERSION"))).size(11.0).color(theme::FAINT));
            });
            ui.add_space(2.0);
            if picked.is_some() || reload || go_back || language.is_some() {
                ui.close();
            }
        })
        .response
        .on_hover_text(t!("menu.settings"));
        if let Some(action) = picked {
            self.actions.push(action);
        }
        if reload {
            self.actions.push(Action::Reload);
        }
        if go_back {
            self.actions.push(Action::AskSetup(Op::Restore));
        }
        if let Some(choice) = language {
            self.actions.push(Action::SetLanguage(choice));
        }
    }

    // ---- left: plugin list ----------------------------------------------------------------------------------

    fn plugin_list(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else { return };
        let search = egui::TextEdit::singleline(&mut self.search)
            .id(search_id())
            .hint_text(RichText::new(format!("{}  {}", icon::MAGNIFYING_GLASS, t!("list.search"))).color(theme::FAINT))
            .margin(Margin::symmetric(10, 8))
            .desired_width(ui.available_width());
        ui.add(search);
        if std::mem::take(&mut self.focus_search) {
            ui.memory_mut(|memory| memory.request_focus(search_id()));
        }
        ui.add_space(6.0);
        // One segmented control for the three views: labelled, with counts, one active at a time. The accent pill
        // slides to the chosen segment and the labels fade between muted and white as it passes.
        let total = snapshot.found.len();
        let on = snapshot.found.keys().filter(|name| !snapshot.is_off(name)).count();
        let recommended = snapshot.found.keys().filter(|name| snapshot.is_recommended(name)).count();
        let segments = [
            (Show::All, format!("{}  {total}", icon::SQUARES_FOUR), t!("list.show.all")),
            (Show::On, format!("{}  {on}", icon::TOGGLE_RIGHT), t!("list.show.on")),
            (Show::Recommended, format!("{}  {recommended}", icon::STAR), t!("list.show.recommended")),
        ];
        let (track, _) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 28.0), Sense::hover());
        let width = track.width() / segments.len() as f32;
        let chosen = segments.iter().position(|(show, _, _)| *show == self.show).unwrap_or(0) as f32;
        let at = ui.ctx().animate_value_with_time(ui.id().with("show-pill"), chosen, 0.09);
        let painter = ui.painter_at(track);
        painter.rect_filled(track, CornerRadius::same(8), theme::FIELD);
        let pill = Rect::from_min_size(egui::pos2(track.left() + at * width, track.top()), Vec2::new(width, track.height()));
        painter.rect_filled(pill, CornerRadius::same(8), theme::ACCENT_FILL);
        for (index, (show, label, hint)) in segments.into_iter().enumerate() {
            let rect = Rect::from_min_size(egui::pos2(track.left() + index as f32 * width, track.top()), Vec2::new(width, track.height()));
            let response = ui.interact(rect, ui.id().with(("show", index)), Sense::click());
            let under = (1.0 - (at - index as f32).abs()).clamp(0.0, 1.0);
            let hover = if response.hovered() && self.show != show { 0.25 } else { 0.0 };
            let ink = theme::lerp_color(theme::lerp_color(theme::MUTED, theme::TEXT, hover), Color32::WHITE, under);
            painter.text(rect.center(), Align2::CENTER_CENTER, label, FontId::proportional(12.5), ink);
            if response.on_hover_cursor(CursorIcon::PointingHand).on_hover_text(hint).clicked() {
                self.show = show;
            }
        }
        ui.add_space(8.0);

        let running = self.locked();
        let listing = self.visible();
        let searching = !self.search.trim().is_empty();
        let mut fold = None;
        let now = Instant::now();
        ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 2.0;
            let mut rows = |ui: &mut Ui, names: &[String]| {
                for name in names {
                    let selected = matches!(self.page, Page::Plugins) && self.selected.as_ref() == Some(name);
                    let feedback = self.feedback.get(&(name.clone(), None)).filter(|f| f.error.is_none()).map(|f| saved_alpha(f.at, now));
                    if let Some(action) = plugin_row(ui, snapshot, name, &listing.hits[name], selected, running, feedback.unwrap_or(0.0)) {
                        self.actions.push(action);
                    }
                }
            };
            if !listing.attention.is_empty() {
                ui.add_space(6.0);
                ui.horizontal(|ui| {
                    ui.add_space(10.0);
                    ui.label(RichText::new(t!("list.needs_attention")).size(11.5).color(theme::FAINT));
                });
                ui.add_space(2.0);
                rows(ui, &listing.attention);
            }
            for group in &listing.groups {
                let open = searching || self.expanded.contains(group.title());
                let on = group.names.iter().filter(|name| snapshot.found.contains_key(*name) && !snapshot.is_off(name)).count();
                ui.add_space(6.0);
                if category_header(ui, group.title(), on, group.names.len(), open, !searching) {
                    fold = Some(group.title().to_owned());
                }
                if open {
                    rows(ui, &group.names);
                }
            }
            if listing.is_empty() {
                ui.add_space(28.0);
                ui.vertical_centered(|ui| {
                    let text = match (snapshot.names.is_empty(), self.show, searching) {
                        (true, _, _) => t!("list.empty.none"),
                        (false, Show::On, true) => t!("list.empty.on_search"),
                        (false, Show::On, false) => t!("list.empty.on"),
                        (false, Show::Recommended, true) => t!("list.empty.recommended_search"),
                        (false, Show::Recommended, false) => t!("list.empty.recommended"),
                        (false, Show::All, _) => t!("list.empty.search"),
                    };
                    ui.label(RichText::new(text).color(theme::MUTED));
                });
            }
            let problems = snapshot.errors.iter().map(|error| t!("list.unreadable", error = error)).chain(self.files.error.clone());
            for problem in problems {
                ui.add_space(8.0);
                notice(ui, icon::WARNING, theme::AMBER, &problem);
            }
        });
        if let Some(title) = fold {
            if !self.expanded.remove(&title) {
                self.expanded.insert(title);
            }
        }
    }

    // ---- right: the selected plugin ---------------------------------------------------------------------------

    fn details(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else { return };
        let running = self.locked();
        // Why editing is locked, said as it is: a closed game with no plugin loader is not "running".
        let lock_reason = if self.running() { t!("details.locked_running") } else { t!("details.locked_no_loader") };
        let Some(name) = self.selected.clone().filter(|name| snapshot.health.contains_key(name)) else {
            let can_install = self.build_listing.as_ref().is_some_and(|listing| !listing.plugins.is_empty());
            if snapshot.names.is_empty() {
                empty_state(ui, icon::PUZZLE_PIECE, t!("details.empty.title"), t!("details.empty.text"));
                if can_install {
                    ui.vertical_centered(|ui| {
                        if ui.add(theme::primary(t!("details.install_plugins"))).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                            self.actions.push(Action::OpenInstall(None));
                        }
                    });
                }
            } else {
                self.overview(ui);
            }
            return;
        };
        let health = &snapshot.health[&name];
        let manifest = snapshot.found.get(&name);
        let color = theme::tone_color(health.tone);

        ui.horizontal(|ui| {
            ui.label(RichText::new(&name).font(FontId::new(22.0, theme::semibold())).color(theme::TEXT));
            if let Some(manifest) = manifest {
                ui.label(RichText::new(manifest.version.to_string()).size(14.0).color(theme::MUTED));
                ui.label(RichText::new(&manifest.category).size(14.0).color(theme::FAINT));
            }
            if snapshot.is_recommended(&name) {
                theme::chip(ui, &format!("{}  {}", icon::STAR, t!("recommended.chip")), theme::ACCENT).on_hover_text(t!("recommended.hint"));
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                // Rare actions out of the way: Open folder and the destructive Uninstall behind ⋯.
                let dots = egui::Button::new(RichText::new(icon::DOTS_THREE).size(18.0).color(theme::MUTED)).frame_when_inactive(false).corner_radius(CornerRadius::same(8));
                let (response, _) = egui::containers::menu::MenuButton::from_button(dots).ui(ui, |ui| {
                    ui.set_min_width(200.0);
                    let item = |ui: &mut Ui, text: RichText, enabled: bool| {
                        ui.add_enabled(enabled, egui::Button::new(text).frame_when_inactive(false).min_size(Vec2::new(ui.available_width(), 28.0)))
                    };
                    if item(ui, RichText::new(format!("{}  {}", icon::FOLDER_OPEN, t!("details.open_folder"))), true).clicked() {
                        self.actions.push(Action::OpenFolder(snapshot.client.plugins.clone()));
                        ui.close();
                    }
                    if manifest.is_some() {
                        ui.separator();
                        let response = item(ui, RichText::new(format!("{}  {}", icon::TRASH, t!("details.uninstall"))).color(theme::RED), !running);
                        if response.on_disabled_hover_text(t!("details.uninstall_locked")).clicked() {
                            self.actions.push(Action::AskUninstall(name.clone()));
                            ui.close();
                        }
                    }
                });
                response.on_hover_cursor(CursorIcon::PointingHand).on_hover_text(t!("details.more"));
            });
        });
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing.x = 8.0;
            let (dot, _) = ui.allocate_exact_size(Vec2::new(8.0, 18.0), Sense::hover());
            status_dot(ui.painter(), dot.center(), health.tone);
            ui.label(RichText::new(i18n::text(health.label)).font(FontId::new(13.5, theme::semibold())).color(theme::lerp_color(color, theme::TEXT, 0.3)));
            if let Some(detail) = &health.detail {
                ui.label(RichText::new(detail).color(theme::MUTED));
            }
        });
        if let Some(text) = manifest.map(|m| m.description.as_str()).filter(|text| !text.is_empty()) {
            ui.add_space(6.0);
            ui.label(RichText::new(text).size(14.0).color(theme::SOFT));
        }

        if manifest.is_some() {
            ui.add_space(18.0);
            let mut on = !snapshot.is_off(&name);
            card(ui, |ui| {
                row(ui, |ui| {
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 2.0;
                        ui.label(RichText::new(t!("details.enabled")).color(theme::TEXT));
                        let hint = if running {
                            lock_reason
                        } else if !on {
                            t!("details.enabled.off")
                        } else if health.needs_attention() {
                            t!("details.enabled.problem")
                        } else {
                            t!("details.enabled.on")
                        };
                        ui.label(RichText::new(hint).size(12.5).color(theme::MUTED));
                    });
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        let (rect, _) = ui.allocate_exact_size(Vec2::new(40.0, 22.0), Sense::hover());
                        if theme::toggle(ui, rect, ui.id().with("enabled"), &mut on, !running).changed() {
                            self.actions.push(Action::SetOff(name.clone(), !on));
                        }
                        save_feedback(ui, self.feedback.get(&(name.clone(), None)));
                    });
                });
            });
        }

        if let Some(manifest) = manifest.filter(|m| !m.settings.is_empty()) {
            ui.add_space(22.0);
            theme::section(ui, t!("details.settings"));
            let note = if running {
                format!("{}  {lock_reason}", icon::LOCK_SIMPLE)
            } else {
                t!("details.settings.note").to_owned()
            };
            // The settings that apply, in display order: a setting with `when` hides unless its controlling setting
            // (its draft while being edited, else its saved value) holds one of the listed values.
            let value_of = |key: &str| {
                let setting = manifest.settings.iter().find(|setting| setting.key == key)?;
                Some(self.drafts.get(&(name.clone(), setting.key.clone())).unwrap_or_else(|| setting.current()))
            };
            let shown: Vec<&Setting> = display_order(&manifest.settings).into_iter().filter(|setting| setting.is_shown(value_of)).collect();
            // One "Reset all" for the whole section, beside the note just above the card; disabled when every shown
            // setting is at its default (a hidden one is ignored by its plugin), but it resets hidden ones too.
            let changed = shown.iter().any(|setting| setting.is_changed());
            ui.horizontal(|ui| {
                ui.label(RichText::new(note).size(12.5).color(if running { theme::AMBER } else { theme::MUTED }));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.add_enabled_ui(!running && changed, |ui| {
                        let response = theme::quiet_button(ui, t!("details.reset_all"), if changed { theme::MUTED } else { theme::FAINT }, theme::ACCENT);
                        let response = if changed { response.on_hover_text(t!("details.reset_all.hint")) } else { response.on_disabled_hover_text(t!("details.reset_all.disabled")) };
                        if response.clicked() {
                            for setting in manifest.settings.iter().filter(|s| s.is_set()) {
                                self.drafts.remove(&(name.clone(), setting.key.clone()));
                                self.actions.push(Action::SetValue(name.clone(), setting.key.clone(), None));
                            }
                        }
                    });
                });
            });
            ui.add_space(4.0);
            card(ui, |ui| {
                for (index, setting) in shown.into_iter().enumerate() {
                    if index > 0 {
                        divider(ui);
                    }
                    let id = (name.clone(), setting.key.clone());
                    let mut value = self.drafts.get(&id).cloned().unwrap_or_else(|| setting.current().clone());
                    let feedback = self.feedback.get(&(name.clone(), Some(setting.key.clone())));
                    let edit = setting_field(ui, setting, &mut value, !running, feedback);
                    match edit {
                        Edit::None => {}
                        Edit::Draft => {
                            self.drafts.insert(id, value);
                        }
                        // Leaving a field unchanged writes nothing (no `value = <default>` appears).
                        Edit::Commit => {
                            self.drafts.remove(&id);
                            if value == *setting.current() {
                                continue;
                            }
                            self.actions.push(Action::SetValue(name.clone(), setting.key.clone(), Some(value)));
                        }
                        Edit::Reset => {
                            self.drafts.remove(&id);
                            self.actions.push(Action::SetValue(name.clone(), setting.key.clone(), None));
                        }
                        Edit::Pick(element) => self.actions.push(Action::PickPath(name.clone(), setting.key.clone(), element)),
                    }
                }
            });
        }

        let depends = manifest.map(|m| m.depends.as_slice()).unwrap_or_default();
        if !depends.is_empty() {
            ui.add_space(22.0);
            theme::section(ui, t!("details.requires"));
            card(ui, |ui| {
                for (index, dependency) in depends.iter().enumerate() {
                    if index > 0 {
                        divider(ui);
                    }
                    let target = snapshot.found.get(&dependency.name);
                    let state = snapshot.predicted.get(&dependency.name).map(|p| p.state);
                    let built = self.build_listing.iter().flat_map(|listing| &listing.plugins).find(|a| a.manifest.name == dependency.name);
                    let install = || Action::OpenInstall(Some(dependency.name.clone()));
                    // What is wrong with this requirement, and the one thing that fixes it.
                    let problem: Option<(String, Option<(&str, Action)>)> = match target {
                        None => Some((t!("dep.not_installed").to_owned(), built.map(|_| (t!("dep.install"), install())))),
                        Some(target) if !dependency.range.satisfied_by(&target.version) => {
                            let fix = built.filter(|a| dependency.range.satisfied_by(&a.manifest.version)).map(|_| (t!("dep.update"), install()));
                            Some((t!("dep.version_installed", version = target.version), fix))
                        }
                        Some(_) if state == Some(State::Off) => {
                            Some((t!("dep.turned_off").to_owned(), (!running).then(|| (t!("dep.turn_on"), Action::SetOff(dependency.name.clone(), false)))))
                        }
                        Some(_) if matches!(state, Some(State::Disabled | State::FailedLoad)) => Some((t!("status.cant_load").to_owned(), None)),
                        Some(_) => None,
                    };
                    row(ui, |ui| {
                        let label = egui::Label::new(RichText::new(&dependency.name).color(theme::TEXT)).sense(Sense::click());
                        if target.is_some() {
                            if ui.add(label).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                                self.actions.push(Action::Select(dependency.name.clone()));
                            }
                        } else {
                            ui.label(RichText::new(&dependency.name).color(theme::TEXT));
                        }
                        ui.label(RichText::new(model::human_range(&dependency.range)).size(13.0).color(theme::MUTED));
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| match problem {
                            None => {
                                ui.label(RichText::new(icon::CHECK).color(theme::GREEN));
                                let version = target.map(|t| t.version.to_string()).unwrap_or_default();
                                ui.label(RichText::new(version).size(13.0).color(theme::MUTED));
                            }
                            Some((text, fix)) => {
                                if let Some((verb, action)) = fix {
                                    if theme::quiet_button(ui, verb, theme::ACCENT, theme::ACCENT).clicked() {
                                        self.actions.push(action);
                                    }
                                }
                                ui.label(RichText::new(text).color(theme::AMBER));
                            }
                        });
                    });
                }
            });
        }

        let users = snapshot.dependents(&name);
        if !users.is_empty() {
            ui.add_space(22.0);
            theme::section(ui, t!("details.used_by"));
            ui.horizontal_wrapped(|ui| {
                for user in &users {
                    let pill = egui::Button::new(RichText::new(user).color(theme::SOFT)).fill(theme::SURFACE).corner_radius(CornerRadius::same(255));
                    if ui.add(pill).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                        self.actions.push(Action::Select(user.clone()));
                    }
                }
            });
        }

        ui.add_space(26.0);
        egui::CollapsingHeader::new(RichText::new(t!("tech.title")).size(13.0).color(theme::MUTED)).id_salt("technical").show(ui, |ui| {
            ui.add_space(4.0);
            let prediction = &snapshot.predicted[&name];
            let with_reason = |state: &str, reason: &str| if reason.is_empty() { state.to_owned() } else { format!("{state} · {reason}") };
            let recorded = snapshot.status.plugins.get(&name).map_or_else(|| t!("tech.no_record").to_owned(), |r| with_reason(&r.state, &r.reason));
            let files = crate::cli::plugin_files(&snapshot.client.plugins, &name)
                .into_iter()
                .map(|file| {
                    let size = std::fs::metadata(snapshot.client.plugins.join(&file)).map(|m| m.len()).unwrap_or(0);
                    format!("{file} ({})", human_size(size))
                })
                .collect::<Vec<_>>();
            egui::Grid::new("technical").num_columns(2).spacing([20.0, 6.0]).show(ui, |ui| {
                for (key, value) in [
                    (t!("tech.last_run"), recorded),
                    (t!("tech.installed_files"), with_reason(prediction.state.as_str(), &prediction.reason)),
                    (t!("tech.files"), if files.is_empty() { t!("tech.none").to_owned() } else { files.join("   ") }),
                ] {
                    ui.label(RichText::new(key).size(12.5).color(theme::MUTED));
                    ui.label(RichText::new(value).size(12.5).color(theme::SOFT));
                    ui.end_row();
                }
            });
            if let Some(manifest) = manifest {
                ui.add_space(8.0);
                egui::Frame::new().fill(theme::FIELD).corner_radius(CornerRadius::same(8)).inner_margin(Margin::same(12)).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.label(RichText::new(manifest.text().trim_end()).monospace().color(theme::MUTED));
                });
            }
        });
    }

    /// What the right side shows with no plugin selected: how many plugins are on, and where to go next, centred in
    /// the visible area.
    fn overview(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else { return };
        let total = snapshot.found.len();
        let on = snapshot.found.keys().filter(|name| !snapshot.is_off(name)).count();
        let block = 100.0;
        let visible = ui.clip_rect().bottom() - ui.cursor().top();
        ui.add_space(((visible - block) / 2.0).max(32.0));
        ui.vertical_centered(|ui| {
            ui.label(RichText::new(icon::PUZZLE_PIECE).size(40.0).color(theme::ACCENT));
            ui.add_space(6.0);
            ui.label(RichText::new(t!("overview.count", on = on, total = total)).font(FontId::new(20.0, theme::semibold())).color(theme::TEXT));
            ui.label(RichText::new(format!("{}  {}", icon::ARROW_LEFT, t!("overview.hint"))).color(theme::MUTED));
        });
    }

    // ---- install ----------------------------------------------------------------------------------------------

    fn install_page(&mut self, ui: &mut Ui) {
        let running = self.locked();
        let Page::Install(form) = &mut self.page else { return };
        let Some(snapshot) = &self.snapshot else { return };
        ui.label(RichText::new(t!("install.title")).font(FontId::new(22.0, theme::semibold())).color(theme::TEXT));
        ui.label(RichText::new(t!("install.subtitle")).color(theme::MUTED));
        ui.add_space(18.0);

        let Some(listing) = self.build_listing.as_ref().filter(|listing| !listing.plugins.is_empty()) else {
            empty_state(ui, icon::PACKAGE, t!("install.empty.title"), t!("install.empty.text"));
            ui.vertical_centered(|ui| {
                if ui.add(theme::primary(t!("install.choose_folder"))).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                    self.actions.push(Action::PickBuild);
                }
            });
            return;
        };

        // The plan decides what else comes along and what would break.
        let wanted: Vec<String> = form.checked.iter().cloned().collect();
        let plan = (!wanted.is_empty()).then(|| snapshot.client.plan_install(&listing.dir, &wanted));
        let mut pulled: BTreeMap<&str, Vec<String>> = BTreeMap::new();
        let mut count = 0;
        if let Some(Ok(plan)) = &plan {
            count = plan.items.len();
            let mut owner = "";
            for item in &plan.items {
                if form.checked.contains(&item.manifest.name) {
                    owner = &item.manifest.name;
                } else {
                    pulled.entry(owner).or_default().push(format!("{} {}", item.manifest.name, item.manifest.version));
                }
            }
        }

        card(ui, |ui| {
            for (index, available) in listing.plugins.iter().enumerate() {
                if index > 0 {
                    divider(ui);
                }
                let name = &available.manifest.name;
                let checked = form.checked.contains(name);
                let requires = available.manifest.depends.iter().map(|d| format!("{} {}", d.name, model::human_range(&d.range))).collect::<Vec<_>>();
                let (tag, color) = match available.change() {
                    Change::New => (t!("install.tag.new"), theme::BLUE),
                    Change::Upgrade => (t!("install.tag.update"), theme::GREEN),
                    Change::Downgrade => (t!("install.tag.downgrade"), theme::AMBER),
                    Change::Same => (t!("install.tag.reinstall"), theme::MUTED),
                };
                let version = match &available.installed {
                    Some(installed) if *installed != available.manifest.version => format!("{installed}  {}  {}", icon::ARROW_RIGHT, available.manifest.version),
                    _ => available.manifest.version.to_string(),
                };
                let sub = (!requires.is_empty()).then(|| t!("install.requires", list = requires.join(", ")));
                if install_row(ui, checked, name, sub.as_deref(), &version, tag, color) {
                    if checked {
                        form.checked.remove(name);
                    } else {
                        form.checked.insert(name.clone());
                    }
                }
                for extra in pulled.get(name.as_str()).into_iter().flatten() {
                    let (rect, _) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 26.0), Sense::hover());
                    let text = format!("{}  {}", icon::ARROW_ELBOW_DOWN_RIGHT, t!("install.also", plugin = extra));
                    ui.painter().text(rect.left_center() + Vec2::new(48.0, -4.0), Align2::LEFT_CENTER, text, FontId::proportional(12.5), theme::MUTED);
                }
            }
        });
        for error in &listing.errors {
            ui.add_space(8.0);
            notice(ui, icon::WARNING, theme::AMBER, &t!("install.unreadable", error = error));
        }

        let mut blocked = wanted.is_empty() || running;
        match &plan {
            Some(Ok(plan)) if !plan.problems.is_empty() => {
                for problem in &plan.problems {
                    ui.add_space(8.0);
                    notice(ui, icon::WARNING, theme::AMBER, &format!("{}.", sentence(problem)));
                }
                ui.add_space(6.0);
                ui.checkbox(&mut form.force, t!("install.force"));
                blocked |= !form.force;
            }
            Some(Err(refused)) => {
                ui.add_space(8.0);
                notice(ui, icon::WARNING, theme::RED, &sentence(&refused.message));
                blocked = true;
            }
            _ => {}
        }

        ui.add_space(18.0);
        ui.horizontal(|ui| {
            if running {
                ui.label(RichText::new(format!("{}  {}", icon::LOCK_SIMPLE, t!("install.locked"))).color(theme::AMBER));
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let label = if count == 0 { t!("install.button").to_owned() } else { t!("install.button_count", plugins = plugins(count)) };
                if ui.add_enabled(!blocked, theme::primary(label)).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                    self.actions.push(Action::Install(wanted.clone(), form.force));
                }
                if theme::quiet_button(ui, t!("common.cancel"), theme::MUTED, theme::TEXT).clicked() {
                    self.actions.push(Action::Select(self.selected.clone().unwrap_or_default()));
                }
            });
        });
    }

    // ---- exe: setup, update, restore -----------------------------------------------------------------------------

    /// Runs setup, update or restore on a worker thread (elevated only when the client folder needs it); the window
    /// shows a progress modal and reports in a toast.
    fn run_setup(&mut self, op: Op) {
        if self.running() {
            return self.notify(Tone::Bad, t!("common.close_game_first_long"), None);
        }
        let request = elevate::Request { op, exe: self.exe.clone(), build: Some(self.build.clone()) };
        let title = match op {
            Op::Setup => t!("job.setup"),
            Op::Update => t!("job.update"),
            Op::Restore => t!("job.restore"),
        };
        self.start_job(title.to_owned(), JobKind::Setup(op), move || {
            let reply = elevate::perform(&request, elevate::elevated);
            match reply.error {
                None => Ok(()),
                Some(_) if reply.code == EXIT_RUNNING => Err(t!("common.close_game_first_long").to_owned()),
                Some(error) => Err(sentence(&error)),
            }
        });
    }

    /// Above the content: the no-loader banner (the exe needs setup before anything works), then the failure banner.
    fn banners(&mut self, ui: &mut Ui) {
        let name = folder_name(&self.exe);
        if let Some(ExeKind::NoLoader) = &self.exe_info.kind {
            egui::Frame::new().fill(theme::AMBER.gamma_multiply(0.09)).corner_radius(CornerRadius::same(10)).inner_margin(Margin::symmetric(16, 12)).show(ui, |ui| {
                ui.set_width(ui.available_width());
                // Text, then its actions on their own row under it, primary first: they follow the reading order,
                // sit right by the sentence they answer, and can never overlap the text however long the exe name.
                ui.horizontal_top(|ui| {
                    ui.label(RichText::new(icon::WARNING).size(18.0).color(theme::AMBER));
                    ui.vertical(|ui| {
                        ui.add(egui::Label::new(RichText::new(t!("banner.no_loader.title")).font(FontId::new(14.0, theme::semibold())).color(theme::TEXT)).wrap());
                        ui.add(egui::Label::new(RichText::new(t!("banner.no_loader.text", name = name)).size(12.5).color(theme::SOFT)).wrap());
                        ui.add_space(8.0);
                        ui.horizontal(|ui| {
                            ui.spacing_mut().item_spacing.x = 8.0;
                            let setup = ui.add_enabled(!self.running(), theme::primary(format!("{}  {}", icon::PUZZLE_PIECE, t!("banner.turn_on")))).on_hover_cursor(CursorIcon::PointingHand);
                            if setup.on_disabled_hover_text(t!("common.close_game_first")).clicked() {
                                self.actions.push(Action::AskSetup(Op::Setup));
                            }
                            if ui.add(theme::secondary(format!("{}  {}", icon::FOLDER_OPEN, t!("banner.choose_other")))).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                                self.actions.push(Action::PickExe);
                            }
                        });
                    });
                });
            });
            ui.add_space(18.0);
        }
        self.failure_banner(ui);
    }

    /// `1 plugin failed: InputLimits — built for a different Ragexe.exe`, with Update when rebuilt plugins fix it,
    /// else Show. Dismissed for the session until the failures change.
    fn failure_banner(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else { return };
        let failures = snapshot.failures();
        if failures.is_empty() || self.dismissed.as_ref() == Some(&failures) {
            return;
        }
        let why = |failure: &Failure| {
            if failure.other_exe {
                t!("failure.other_exe").to_owned()
            } else if failure.reason.is_empty() {
                i18n::text(failure.label).to_lowercase()
            } else {
                failure.reason.clone()
            }
        };
        let title = match failures.as_slice() {
            [one] => t!("failure.one", name = one.name, why = why(one)),
            many => t!("failure.many", plugins = plugins(many.len()), names = many.iter().map(|f| f.name.as_str()).collect::<Vec<_>>().join(", ")),
        };
        let update = failures.iter().any(|f| f.other_exe) && self.exe_info.update.is_some();
        let mut dismiss = false;
        egui::Frame::new().fill(theme::RED.gamma_multiply(0.09)).corner_radius(CornerRadius::same(10)).inner_margin(Margin::symmetric(16, 10)).show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::WARNING_OCTAGON).size(18.0).color(theme::RED));
                let detail = failures.iter().map(|f| format!("{} — {}", f.name, why(f))).collect::<Vec<_>>().join("\n");
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if theme::quiet_button(ui, icon::X, theme::MUTED, theme::TEXT).on_hover_text(t!("failure.dismiss")).clicked() {
                        dismiss = true;
                    }
                    if update {
                        let enabled = !self.running();
                        if ui.add_enabled(enabled, theme::secondary(t!("failure.update"))).on_hover_text(t!("failure.update.hint")).clicked() {
                            self.actions.push(Action::AskSetup(Op::Update));
                        }
                    } else if theme::quiet_button(ui, t!("failure.show"), theme::ACCENT, theme::ACCENT).clicked() {
                        self.actions.push(Action::Select(failures[0].name.clone()));
                    }
                    ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                        ui.add(egui::Label::new(RichText::new(&title).color(theme::TEXT)).truncate()).on_hover_text(detail);
                    });
                });
            });
        });
        ui.add_space(14.0);
        if dismiss {
            self.dismissed = Some(failures);
        }
    }

    /// Setup, update or restore, said for someone who knows nothing of loaders or builds: a before → after picture,
    /// three short points (what changes, what's kept, how to undo), and the technical details folded away.
    fn setup_dialog(&mut self, ctx: &egui::Context) {
        let Some(dialog) = &self.setup else { return };
        let op = dialog.op;
        let name = folder_name(&self.exe);
        let backup = folder_name(&exe_setup::backup_of(&self.exe));
        let set = PluginSet::find(&self.build);
        let mut close = false;
        let mut confirm = false;
        let mut pick = false;
        let frame = egui::Frame::new()
            .fill(theme::SURFACE)
            .corner_radius(CornerRadius::same(14))
            .inner_margin(Margin::same(24))
            .shadow(Shadow { offset: [0, 16], blur: 40, spread: 0, color: Color32::from_black_alpha(160) });
        let modal = egui::Modal::new(Id::new("setup")).frame(frame).show(ctx, |ui| {
            ui.set_width(440.0);
            let title = match op {
                Op::Setup => t!("setup.title.setup"),
                Op::Update => t!("setup.title.update"),
                Op::Restore => t!("setup.title.restore"),
            };
            ui.label(RichText::new(title).font(FontId::new(17.0, theme::semibold())).color(theme::TEXT));
            ui.add_space(14.0);

            // One simple, symmetric picture of what the game becomes, worded the way a player thinks of it (the game,
            // not its files); file names live under Details. Update changes nothing visible, so it has no picture.
            let original = (icon::GAME_CONTROLLER, t!("setup.tile.original"), theme::MUTED);
            let modded = (icon::PUZZLE_PIECE, t!("setup.tile.modded"), theme::ACCENT);
            match op {
                Op::Setup => before_after(ui, original, modded),
                Op::Restore => before_after(ui, modded, original),
                Op::Update => {}
            }
            if op != Op::Update {
                ui.add_space(16.0);
            }

            // Only what the picture can't show, one short line each, plain text: no per-line icons to parse.
            let points: &[&str] = match op {
                Op::Setup => &[t!("setup.point.go_back"), t!("setup.point.kept")],
                Op::Update => &[t!("setup.point.newest"), t!("setup.point.kept")],
                Op::Restore => &[t!("setup.point.nothing_deleted")],
            };
            for point in points {
                ui.add(egui::Label::new(RichText::new(*point).size(13.5).color(theme::SOFT)).wrap());
            }

            // The source folder only needs attention when it's wrong; otherwise it waits under Details.
            if op != Op::Restore {
                if set.is_err() {
                    ui.add_space(6.0);
                    let (headline, hint) = missing_build(&self.build);
                    notice(ui, icon::WARNING, theme::RED, &format!("{headline}\n{hint}"));
                    ui.add_space(4.0);
                    if ui.add(theme::secondary(format!("{}  {}", icon::FOLDER_OPEN, t!("setup.choose_build")))).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                        pick = true;
                    }
                }
            }
            ui.add_space(6.0);
            let details_open = op != Op::Restore && set.is_err();
            egui::CollapsingHeader::new(RichText::new(t!("setup.details")).size(12.5).color(theme::MUTED)).id_salt("setup-details").default_open(details_open).show(ui, |ui| {
                let steps: Vec<String> = match op {
                    Op::Setup => vec![
                        t!("setup.step.rename", name = name, backup = backup),
                        t!("setup.step.copy", name = name),
                        t!("setup.step.install").to_owned(),
                    ],
                    Op::Update => vec![t!("setup.step.update", name = name)],
                    Op::Restore => vec![t!("setup.step.restore", backup = backup, name = name)],
                };
                for step in steps {
                    ui.add(egui::Label::new(RichText::new(format!("•  {step}")).size(12.5).color(theme::MUTED)).wrap());
                }
                if op == Op::Update {
                    let changes: Vec<(String, String, Color32)> = self
                        .build_listing
                        .iter()
                        .flat_map(|listing| &listing.plugins)
                        .filter_map(|plugin| {
                            let new = plugin.manifest.version.to_string();
                            match plugin.change() {
                                Change::Same => None,
                                Change::New => Some((plugin.manifest.name.clone(), t!("setup.change.new", version = new), theme::GREEN)),
                                Change::Upgrade => Some((plugin.manifest.name.clone(), format!("{} → {new}", plugin.installed.as_ref()?), theme::ACCENT)),
                                Change::Downgrade => Some((plugin.manifest.name.clone(), format!("{} → {new}", plugin.installed.as_ref()?), theme::AMBER)),
                            }
                        })
                        .collect();
                    ui.add_space(6.0);
                    let heading = if changes.is_empty() { t!("setup.all_current").to_owned() } else { t!("setup.changed", plugins = plugins(changes.len())) };
                    ui.label(RichText::new(heading).size(12.5).color(theme::SOFT));
                    ScrollArea::vertical().id_salt("update-changes").max_height(180.0).show(ui, |ui| {
                        egui::Grid::new("update-changes-grid").num_columns(2).spacing([24.0, 3.0]).show(ui, |ui| {
                            for (name, change, color) in &changes {
                                ui.label(RichText::new(name).size(12.5).color(theme::TEXT));
                                ui.label(RichText::new(change).size(12.5).color(*color));
                                ui.end_row();
                            }
                        });
                    });
                }
                if op != Op::Restore {
                    ui.add_space(4.0);
                    ui.horizontal(|ui| {
                        ui.label(RichText::new(t!("setup.from")).size(12.5).color(theme::MUTED));
                        let shown = if self.build.as_os_str().is_empty() { t!("setup.not_set").to_owned() } else { self.build.display().to_string() };
                        ui.add(egui::Label::new(RichText::new(shown).size(12.5).color(theme::SOFT)).truncate());
                        if theme::quiet_button(ui, t!("setup.change"), theme::ACCENT, theme::ACCENT).clicked() {
                            pick = true;
                        }
                    });
                }
            });
            if exe_setup::needs_elevation(&self.exe) {
                ui.add_space(6.0);
                ui.label(RichText::new(format!("{}  {}", icon::SHIELD_CHECK, t!("setup.elevation"))).size(12.5).color(theme::AMBER));
            }
            ui.add_space(16.0);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let label = match op {
                    Op::Setup => t!("setup.confirm.setup"),
                    Op::Update => t!("setup.confirm.update"),
                    Op::Restore => t!("setup.confirm.restore"),
                };
                let ready = op == Op::Restore || set.is_ok();
                if ui.add_enabled(ready, theme::primary(label)).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                    confirm = true;
                }
                if theme::quiet_button(ui, t!("common.cancel"), theme::MUTED, theme::TEXT).clicked() {
                    close = true;
                }
            });
        });
        if pick {
            self.actions.push(Action::PickBuild);
        }
        if confirm {
            self.actions.push(Action::RunSetup(op));
        }
        if confirm || close || modal.should_close() {
            self.setup = None;
        }
    }

    fn uninstall_dialog(&mut self, ctx: &egui::Context) {
        let Some(dialog) = &self.uninstall else { return };
        let mut close = false;
        let mut confirm = false;
        let frame = egui::Frame::new()
            .fill(theme::SURFACE)
            .corner_radius(CornerRadius::same(14))
            .inner_margin(Margin::same(24))
            .shadow(Shadow { offset: [0, 16], blur: 40, spread: 0, color: Color32::from_black_alpha(160) });
        let modal = egui::Modal::new(Id::new("uninstall")).frame(frame).show(ctx, |ui| {
            ui.set_width(400.0);
            ui.label(RichText::new(t!("uninstall.title", name = dialog.name)).font(FontId::new(17.0, theme::semibold())).color(theme::TEXT));
            ui.add_space(2.0);
            ui.label(RichText::new(t!("uninstall.text")).color(theme::MUTED));
            if !dialog.dependents.is_empty() {
                ui.add_space(10.0);
                ui.label(RichText::new(t!("uninstall.dependents")).color(theme::SOFT));
                for name in &dialog.dependents {
                    ui.label(RichText::new(format!("•  {name}")).color(theme::TEXT));
                }
            }
            ui.add_space(16.0);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let label = if dialog.dependents.is_empty() { t!("uninstall.button").to_owned() } else { t!("uninstall.button_count", plugins = plugins(dialog.dependents.len() + 1)) };
                if ui.add(theme::destructive(label)).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                    confirm = true;
                }
                if theme::quiet_button(ui, t!("common.cancel"), theme::MUTED, theme::TEXT).clicked() {
                    close = true;
                }
            });
        });
        if confirm {
            self.actions.push(Action::Uninstall(dialog.name.clone()));
        }
        if confirm || close || modal.should_close() {
            self.uninstall = None;
        }
    }

    fn show_toast(&mut self, ctx: &egui::Context) {
        let Some(toast) = &self.toast else { return };
        let now = Instant::now();
        if now >= toast.until {
            self.toast = None;
            return;
        }
        ctx.request_repaint_after(toast.until - now);
        let (glyph, color) = match toast.tone {
            Tone::Bad | Tone::Warn => (icon::WARNING_CIRCLE, theme::tone_color(toast.tone)),
            _ => (icon::CHECK_CIRCLE, theme::GREEN),
        };
        let mut undo = false;
        egui::Area::new(Id::new("toast")).anchor(Align2::CENTER_BOTTOM, [0.0, -28.0]).order(egui::Order::Foreground).show(ctx, |ui| {
            egui::Frame::new()
                .fill(Color32::from_rgb(0x2a, 0x2e, 0x38))
                .corner_radius(CornerRadius::same(10))
                .inner_margin(Margin::symmetric(14, 6))
                .shadow(Shadow { offset: [0, 8], blur: 24, spread: 0, color: Color32::from_black_alpha(120) })
                .show(ui, |ui| {
                    ui.set_max_width(560.0);
                    ui.horizontal(|ui| {
                        ui.label(RichText::new(glyph).size(16.0).color(color));
                        ui.add(egui::Label::new(RichText::new(&toast.text).color(theme::TEXT)).wrap());
                        if toast.undo.is_some() && theme::quiet_button(ui, t!("toast.undo"), theme::ACCENT, theme::ACCENT).clicked() {
                            undo = true;
                        }
                    });
                });
        });
        if undo {
            if let Some(names) = self.toast.take().and_then(|toast| toast.undo) {
                self.actions.push(Action::RestoreDisabled(names));
            }
        }
    }

    /// While a long operation runs: a modal with a spinner, so the window stays responsive but nothing else starts.
    fn job_modal(&mut self, ctx: &egui::Context) {
        let Some(job) = &self.job else { return };
        let frame = egui::Frame::new()
            .fill(theme::SURFACE)
            .corner_radius(CornerRadius::same(14))
            .inner_margin(Margin::same(24))
            .shadow(Shadow { offset: [0, 16], blur: 40, spread: 0, color: Color32::from_black_alpha(160) });
        egui::Modal::new(Id::new("job")).frame(frame).show(ctx, |ui| {
            ui.set_width(340.0);
            ui.horizontal(|ui| {
                ui.add(egui::Spinner::new().size(20.0).color(theme::ACCENT));
                ui.add_space(6.0);
                ui.vertical(|ui| {
                    ui.label(RichText::new(&job.title).font(FontId::new(15.0, theme::semibold())).color(theme::TEXT));
                    let hint = if matches!(job.kind, JobKind::Setup(_)) { t!("job.hint.setup") } else { t!("job.hint.copy") };
                    ui.label(RichText::new(hint).size(12.5).color(theme::MUTED));
                });
            });
        });
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut Ui, _frame: &mut eframe::Frame) {
        let ctx = ui.ctx().clone();
        // Background events: process changes and file changes.
        let game = self.processes.state();
        let files_changed = self.files.take();
        if game != self.game || files_changed {
            self.game = game;
            self.reload();
        }
        self.finish_job();
        // "Saved" ticks fade out; errors stay until the row is saved again.
        let now = Instant::now();
        self.feedback.retain(|_, f| f.error.is_some() || now.duration_since(f.at) < SAVED_FOR);
        if self.feedback.values().any(|f| f.error.is_none()) {
            ctx.request_repaint_after(Duration::from_millis(33));
        }
        if self.job.is_none() {
            self.keyboard(&ctx);
        }

        egui::Panel::top("top")
            .frame(egui::Frame::new().fill(theme::CHROME).inner_margin(Margin { left: 16, right: 12, top: 10, bottom: 10 }))
            .show_separator_line(false)
            .show(ui, |ui| self.top_bar(ui));
        if self.snapshot.is_some() {
            egui::Panel::left("list")
                .frame(egui::Frame::new().fill(theme::CHROME).inner_margin(Margin { left: 12, right: 12, top: 4, bottom: 12 }))
                .resizable(false)
                .exact_size(LIST_WIDTH)
                .show_separator_line(false)
                .show(ui, |ui| self.plugin_list(ui));
        }
        egui::CentralPanel::no_frame().frame(egui::Frame::new().fill(theme::BG)).show(ui, |ui| {
            ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
                egui::Frame::new().inner_margin(Margin { left: 36, right: 36, top: 28, bottom: 32 }).show(ui, |ui| {
                    if self.snapshot.is_none() {
                        // No plugin list beside it: centred in the whole window, not in the detail column.
                        empty_state(ui, icon::FOLDER, t!("start.title"), t!("start.text"));
                        ui.vertical_centered(|ui| {
                            if ui.add(theme::primary(t!("start.button"))).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                                self.actions.push(Action::PickExe);
                            }
                        });
                        return;
                    }
                    ui.set_max_width(CONTENT_WIDTH);
                    self.banners(ui);
                    if matches!(self.page, Page::Install(_)) {
                        self.install_page(ui);
                    } else {
                        self.details(ui);
                    }
                });
            });
        });

        self.uninstall_dialog(&ctx);
        self.setup_dialog(&ctx);
        self.show_toast(&ctx);
        self.job_modal(&ctx);

        for action in std::mem::take(&mut self.actions) {
            self.apply(action);
            ctx.request_repaint();
        }
    }

    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        storage.set_string(KEY_CLIENT, self.client.to_string_lossy().into_owned());
        storage.set_string(KEY_CLIENT_EXE, self.exe.to_string_lossy().into_owned());
        storage.set_string(KEY_BUILD, self.build.to_string_lossy().into_owned());
        storage.set_string(KEY_SHOW, self.show.key().to_owned());
        storage.set_string(KEY_SEEN, self.seen.iter().cloned().collect::<Vec<_>>().join("\n"));
        storage.set_string(KEY_LANGUAGE, self.language.map_or_else(String::new, |lang| lang.code().to_owned()));
    }
}

/// Filled for a live status, a ring for a plugin that is off.
fn status_dot(painter: &egui::Painter, center: egui::Pos2, tone: Tone) {
    let color = theme::tone_color(tone);
    if tone == Tone::Off {
        painter.circle_stroke(center, 3.5, Stroke::new(1.5, color));
    } else {
        painter.circle_filled(center, 4.0, color);
    }
}

fn status_hint(health: &Health) -> String {
    let label = i18n::text(health.label);
    match &health.detail {
        Some(detail) => format!("{label} · {detail}"),
        None => label.to_owned(),
    }
}

/// One line of the restart badge's tooltip.
fn change_text(change: &PendingChange) -> String {
    match change {
        PendingChange::TurnedOn(name) => t!("change.turned_on", name = name),
        PendingChange::TurnedOff(name) => t!("change.turned_off", name = name),
        PendingChange::Installed(name) => t!("change.installed", name = name),
        PendingChange::SettingsChanged(name) => t!("change.settings", name = name),
    }
}

/// A category title as shown: the known categories of plugin.toml translated, anything else as written. The list
/// keeps the raw title for folding and searching.
fn category_title(title: &str) -> &str {
    match title {
        search::UNACCEPTED => t!("category.unaccepted"),
        "Audio" => t!("category.audio"),
        "Camera" => t!("category.camera"),
        "Character" => t!("category.character"),
        "Chat" => t!("category.chat"),
        "Data" => t!("category.data"),
        "Graphics" => t!("category.graphics"),
        "Interface" => t!("category.interface"),
        "Login" => t!("category.login"),
        "Network" => t!("category.network"),
        "System" => t!("category.system"),
        "Window" => t!("category.window"),
        other => other,
    }
}

/// The opacity of a "Saved" tick `at` old: full, then fading over the last half of [`SAVED_FOR`].
fn saved_alpha(at: Instant, now: Instant) -> f32 {
    let age = now.duration_since(at).as_secs_f32() / SAVED_FOR.as_secs_f32();
    ((1.0 - age) * 2.0).clamp(0.0, 1.0)
}

/// The inline result of a save, laid out right to left beside its control: a fading "✓ Saved", or the error in red.
fn save_feedback(ui: &mut Ui, feedback: Option<&Feedback>) {
    let Some(feedback) = feedback else { return };
    match &feedback.error {
        Some(error) => {
            ui.add(egui::Label::new(RichText::new(format!("{}  {error}", icon::WARNING_CIRCLE)).size(12.5).color(theme::RED)).wrap());
        }
        None => {
            let alpha = saved_alpha(feedback.at, Instant::now());
            ui.label(RichText::new(format!("{}  {}", icon::CHECK, t!("common.saved"))).size(12.5).color(theme::GREEN.gamma_multiply(alpha)));
        }
    }
}

/// Draws one line of text left-centered at `origin`, the byte range `mark` in white on an accent pill; returns its
/// size.
fn marked_text(painter: &egui::Painter, origin: egui::Pos2, text: String, mark: Option<&std::ops::Range<usize>>, font: FontId, ink: Color32) -> Vec2 {
    let (galley, pill) = match mark {
        None => (painter.layout_no_wrap(text, font, ink), None),
        Some(mark) => {
            let mut job = egui::text::LayoutJob::default();
            let format = |color| egui::text::TextFormat::simple(font.clone(), color);
            job.append(&text[..mark.start], 0.0, format(ink));
            job.append(&text[mark.clone()], 0.0, format(Color32::WHITE));
            job.append(&text[mark.end..], 0.0, format(ink));
            let galley = painter.layout_job(job);
            let at = |byte: usize| egui::text::CCursor::new(text[..byte].chars().count());
            let (left, right) = (galley.pos_from_cursor(at(mark.start)), galley.pos_from_cursor(at(mark.end)));
            (galley, Some(Rect::from_min_max(left.min, right.max).expand2(Vec2::new(2.0, 0.0))))
        }
    };
    let origin = origin - Vec2::new(0.0, galley.size().y / 2.0);
    if let Some(pill) = pill {
        painter.rect_filled(pill.translate(origin.to_vec2()), CornerRadius::same(4), theme::ACCENT_FILL);
    }
    let size = galley.size();
    painter.galley(origin, galley, ink);
    size
}

/// One list row: name (the search match marked), the recommended star, a problem icon only when something is wrong
/// (its tooltip says what), the inline "Saved" tick, the switch, and an excerpt of the description when only the
/// description matched. The switch is registered after the row, so it gets its click.
fn plugin_row(ui: &mut Ui, snapshot: &Snapshot, name: &str, hit: &Hit, selected: bool, running: bool, saved: f32) -> Option<Action> {
    let health = &snapshot.health[name];
    let excerpt = match (&hit.name, &hit.description, snapshot.found.get(name)) {
        (None, Some(range), Some(manifest)) => Some(search::excerpt(&manifest.description, range.clone())),
        _ => None,
    };
    let height = if excerpt.is_some() { 58.0 } else { 40.0 };
    let (rect, response) = ui.allocate_exact_size(Vec2::new(ui.available_width(), height), Sense::click());
    let hover = ui.ctx().animate_bool_with_time(response.id.with("hover"), response.hovered(), 0.12);
    let active = ui.ctx().animate_bool_with_time(response.id.with("selected"), selected, 0.12);
    let has_switch = snapshot.found.contains_key(name);
    let line = if excerpt.is_some() { rect.top() + 18.0 } else { rect.center().y };
    let switch = Rect::from_center_size(egui::pos2(rect.right() - 28.0, line), Vec2::new(32.0, 18.0));
    let marks_right = if has_switch { switch.left() - 8.0 } else { rect.right() - 12.0 };
    // A problem: an icon in the tone's colour (never colour alone), right before the switch.
    let problem = health.needs_attention().then(|| {
        let glyph = if health.tone == Tone::Bad { icon::WARNING_OCTAGON } else { icon::WARNING };
        (glyph, theme::tone_color(health.tone), Rect::from_center_size(egui::pos2(marks_right - 9.0, line), Vec2::splat(18.0)))
    });
    let recommended = snapshot.is_recommended(name);

    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let fill = theme::lerp_color(theme::lerp_color(theme::CHROME, theme::ROW_HOVER, hover), theme::SURFACE, active);
        painter.rect_filled(rect, CornerRadius::same(8), fill);
        if active > 0.0 {
            let bar = Rect::from_center_size(egui::pos2(rect.left() + 1.5, rect.center().y), Vec2::new(3.0, 18.0 * active));
            painter.rect_filled(bar, CornerRadius::same(2), theme::ACCENT);
        }
        let ink = if has_switch && snapshot.is_off(name) { theme::MUTED } else { theme::TEXT };
        let mut text_right = problem.map_or(marks_right, |(_, _, at)| at.left() - 4.0);
        if saved > 0.0 {
            let tick = egui::pos2(text_right - 8.0, line);
            painter.text(tick, Align2::CENTER_CENTER, icon::CHECK, FontId::proportional(13.0), theme::GREEN.gamma_multiply(saved));
            text_right -= 18.0;
        }
        if let Some((glyph, color, at)) = problem {
            painter.text(at.center(), Align2::CENTER_CENTER, glyph, FontId::proportional(15.0), color);
        }
        let clipped = painter.with_clip_rect(Rect::from_min_max(rect.min, egui::pos2(text_right, rect.bottom())));
        let left = rect.left() + 14.0;
        let title = marked_text(&clipped, egui::pos2(left, line), name.to_owned(), hit.name.as_ref(), FontId::new(13.5, theme::semibold()), ink);
        if recommended {
            clipped.text(egui::pos2(left + title.x + 6.0, line), Align2::LEFT_CENTER, icon::STAR, FontId::proportional(12.0), theme::ACCENT);
        }
        if let Some((text, range)) = excerpt {
            let clipped = painter.with_clip_rect(Rect::from_min_max(rect.min, egui::pos2(rect.right() - 12.0, rect.bottom())));
            marked_text(&clipped, egui::pos2(left, rect.bottom() - 14.0), text, Some(&range), FontId::proportional(12.0), theme::MUTED);
        }
    }

    let mut action = None;
    if has_switch {
        let mut on = !snapshot.is_off(name);
        let toggled = theme::toggle(ui, switch, ui.id().with(("switch", name)), &mut on, !running);
        let toggled = if running { toggled.on_hover_text(t!("list.switch_locked")) } else { toggled };
        if toggled.changed() {
            action = Some(Action::SetOff(name.to_owned(), !on));
        }
    }
    if let Some((_, _, at)) = problem {
        ui.interact(at, ui.id().with(("problem", name)), Sense::hover()).on_hover_text(status_hint(health));
    }
    let response = response.on_hover_cursor(CursorIcon::PointingHand);
    let response = if recommended { response.on_hover_text(t!("recommended.hint")) } else { response };
    if response.clicked() && action.is_none() {
        action = Some(Action::Select(name.to_owned()));
    }
    action
}

/// A category of the plugin list: caret, name, `on/total`. A category with plugins turned on has a brighter name and an
/// accent count pill, so the ones in use stand out. Returns whether it was clicked (only when `foldable`).
fn category_header(ui: &mut Ui, title: &str, on: usize, total: usize, open: bool, foldable: bool) -> bool {
    let sense = if foldable { Sense::click() } else { Sense::hover() };
    let (rect, response) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 26.0), sense);
    if ui.is_rect_visible(rect) {
        let hover = ui.ctx().animate_bool_with_time(response.id.with("hover"), foldable && response.hovered(), 0.12);
        let painter = ui.painter();
        painter.rect_filled(rect, CornerRadius::same(6), theme::ROW_HOVER.gamma_multiply(hover));
        let rest = if on > 0 { theme::TEXT } else { theme::MUTED };
        let ink = theme::lerp_color(rest, theme::TEXT, hover);
        let caret = if open { icon::CARET_DOWN } else { icon::CARET_RIGHT };
        painter.text(egui::pos2(rect.left() + 12.0, rect.center().y), Align2::LEFT_CENTER, caret, FontId::proportional(11.5), theme::FAINT);
        let label = painter.layout_no_wrap(category_title(title).to_owned(), FontId::new(12.5, theme::semibold()), ink);
        let x = rect.left() + 28.0;
        let after = x + label.size().x + 8.0;
        painter.galley(egui::pos2(x, rect.center().y - label.size().y / 2.0), label, ink);
        let count = painter.layout_no_wrap(format!("{on}/{total}"), FontId::proportional(11.5), if on > 0 { theme::ACCENT } else { theme::FAINT });
        let pill = Rect::from_min_size(egui::pos2(after, rect.center().y - 9.0), Vec2::new(count.size().x + 12.0, 18.0));
        if on > 0 {
            painter.rect_filled(pill, CornerRadius::same(9), theme::ACCENT.gamma_multiply(0.14));
        }
        painter.galley(egui::pos2(pill.left() + 6.0, rect.center().y - count.size().y / 2.0), count, theme::FAINT);
    }
    let response = response.on_hover_text(t!("list.category_count", on = on, total = total));
    foldable && response.on_hover_cursor(CursorIcon::PointingHand).clicked()
}

/// One row of the install list; returns whether it was clicked.
fn install_row(ui: &mut Ui, checked: bool, name: &str, sub: Option<&str>, version: &str, tag: &str, color: Color32) -> bool {
    let (rect, response) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 52.0), Sense::click());
    if ui.is_rect_visible(rect) {
        let hover = ui.ctx().animate_bool_with_time(response.id.with("hover"), response.hovered(), 0.12);
        let on = ui.ctx().animate_bool_with_time(response.id.with("on"), checked, 0.12);
        let painter = ui.painter();
        painter.rect_filled(rect.shrink(4.0), CornerRadius::same(8), theme::SURFACE_HOVER.gamma_multiply(hover));
        let tick = Rect::from_center_size(egui::pos2(rect.left() + 26.0, rect.center().y), Vec2::splat(18.0));
        painter.rect_filled(tick, CornerRadius::same(5), theme::ACCENT_FILL.gamma_multiply(on));
        painter.rect_stroke(tick, CornerRadius::same(5), Stroke::new(1.5, theme::lerp_color(theme::FAINT, theme::ACCENT_FILL, on)), StrokeKind::Inside);
        if checked {
            painter.text(tick.center(), Align2::CENTER_CENTER, icon::CHECK, FontId::proportional(12.0), Color32::WHITE);
        }
        let left = rect.left() + 48.0;
        match sub {
            Some(sub) => {
                painter.text(egui::pos2(left, rect.center().y - 9.0), Align2::LEFT_CENTER, name, FontId::new(13.5, theme::semibold()), theme::TEXT);
                painter.text(egui::pos2(left, rect.center().y + 9.0), Align2::LEFT_CENTER, sub, FontId::proportional(12.0), theme::MUTED);
            }
            None => {
                painter.text(egui::pos2(left, rect.center().y), Align2::LEFT_CENTER, name, FontId::new(13.5, theme::semibold()), theme::TEXT);
            }
        }
        let label = painter.layout_no_wrap(tag.to_owned(), FontId::proportional(12.0), theme::lerp_color(color, theme::TEXT, 0.3));
        let pill = Rect::from_min_size(
            egui::pos2(rect.right() - 16.0 - label.size().x - 18.0, rect.center().y - 11.0),
            Vec2::new(label.size().x + 18.0, 22.0),
        );
        painter.rect_filled(pill, CornerRadius::same(255), color.gamma_multiply(0.13));
        painter.galley(pill.center() - label.size() / 2.0, label, color);
        painter.text(egui::pos2(pill.left() - 12.0, rect.center().y), Align2::RIGHT_CENTER, version, FontId::proportional(13.0), theme::MUTED);
    }
    response.on_hover_cursor(CursorIcon::PointingHand).clicked()
}

/// The one card level: a filled group of rows.
fn card<R>(ui: &mut Ui, add: impl FnOnce(&mut Ui) -> R) -> R {
    egui::Frame::new()
        .fill(theme::SURFACE)
        .corner_radius(CornerRadius::same(10))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.spacing_mut().item_spacing.y = 0.0;
            add(ui)
        })
        .inner
}

fn row<R>(ui: &mut Ui, add: impl FnOnce(&mut Ui) -> R) -> R {
    egui::Frame::new()
        .inner_margin(Margin::symmetric(16, 12))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.set_min_height(24.0);
                add(ui)
            })
            .inner
        })
        .inner
}

fn divider(ui: &mut Ui) {
    let (rect, _) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 1.0), Sense::hover());
    ui.painter().hline((rect.left() + 16.0)..=(rect.right() - 16.0), rect.center().y, Stroke::new(1.0, theme::DIVIDER));
}

fn notice(ui: &mut Ui, glyph: &str, color: Color32, text: &str) {
    egui::Frame::new().fill(color.gamma_multiply(0.09)).corner_radius(CornerRadius::same(8)).inner_margin(Margin::symmetric(12, 8)).show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.horizontal_top(|ui| {
            ui.label(RichText::new(glyph).color(color));
            ui.add(egui::Label::new(RichText::new(text).size(13.0).color(theme::SOFT)).wrap());
        });
    });
}

/// Why the plugin files can't be used, in plain words: what is wrong with the folder, then what to do.
/// The exact path stays under Details ("From").
fn missing_build(build: &Path) -> (&'static str, &'static str) {
    if build.as_os_str().is_empty() {
        (t!("build.none"), t!("build.hint"))
    } else if !build.is_dir() {
        (t!("build.gone"), t!("build.gone.hint"))
    } else {
        (t!("build.wrong"), t!("build.hint"))
    }
}

/// Two equal tiles joined by an arrow: what the game is now, and what it becomes. Same size, centred icon and label:
/// one simple shape to read, the "after" tile set apart by the accent alone.
fn before_after(ui: &mut Ui, before: (&str, &str, Color32), after: (&str, &str, Color32)) {
    let arrow = 48.0;
    let width = ((ui.available_width() - arrow) / 2.0).floor();
    let height = 96.0;
    let tile = |ui: &mut Ui, (glyph, label, color): (&str, &str, Color32)| {
        let (rect, _) = ui.allocate_exact_size(Vec2::new(width, height), Sense::hover());
        let painter = ui.painter();
        painter.rect(rect, CornerRadius::same(12), color.gamma_multiply(0.08), Stroke::new(1.0, color.gamma_multiply(0.5)), StrokeKind::Inside);
        painter.text(rect.center() - Vec2::new(0.0, 14.0), Align2::CENTER_CENTER, glyph, FontId::proportional(30.0), color);
        painter.text(rect.center() + Vec2::new(0.0, 22.0), Align2::CENTER_CENTER, label, FontId::new(14.0, theme::semibold()), theme::TEXT);
    };
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 0.0;
        tile(ui, before);
        let (rect, _) = ui.allocate_exact_size(Vec2::new(arrow, height), Sense::hover());
        ui.painter().text(rect.center(), Align2::CENTER_CENTER, icon::ARROW_RIGHT, FontId::proportional(22.0), theme::MUTED);
        tile(ui, after);
    });
}

fn empty_state(ui: &mut Ui, glyph: &str, title: &str, text: &str) {
    ui.vertical_centered(|ui| {
        ui.add_space(72.0);
        ui.label(RichText::new(glyph).size(36.0).color(theme::FAINT));
        ui.add_space(8.0);
        ui.label(RichText::new(title).font(FontId::new(16.0, theme::semibold())).color(theme::TEXT));
        ui.label(RichText::new(text).color(theme::MUTED));
        ui.add_space(14.0);
    });
}

fn human_size(bytes: u64) -> String {
    match bytes {
        b if b >= 1 << 20 => format!("{:.1} MB", b as f64 / (1 << 20) as f64),
        b if b >= 1 << 10 => format!("{:.0} KB", b as f64 / 1024.0),
        b => format!("{b} B"),
    }
}

/// What a setting field did this frame.
enum Edit {
    None,
    /// Changed, still being edited (text being typed, a slider being dragged).
    Draft,
    /// Changed and finished: write it.
    Commit,
    /// Back to the default: remove the file's `value`.
    Reset,
    /// Open the path picker (for a list element when `Some`).
    Pick(Option<usize>),
}

impl Edit {
    /// A text or number input: a draft while it has focus or is dragged, written when left.
    fn of(response: &egui::Response) -> Edit {
        if response.lost_focus() || response.drag_stopped() {
            Edit::Commit
        } else if response.changed() {
            if response.has_focus() || response.dragged() { Edit::Draft } else { Edit::Commit }
        } else {
            Edit::None
        }
    }

    /// Keeps the first thing that happened.
    fn or(&mut self, other: Edit) {
        if matches!(self, Edit::None) {
            *self = other;
        }
    }
}

/// Opens the file or folder dialog of a path setting.
fn pick(kind: &PathKind, client: &Path, label: &str) -> Option<PathBuf> {
    let mut dialog = rfd::FileDialog::new().set_title(label);
    if client.is_dir() {
        dialog = dialog.set_directory(client);
    }
    if kind.directory {
        return dialog.pick_folder();
    }
    if !kind.filter.is_empty() {
        let extensions: Vec<&str> = kind.filter.iter().map(|f| f.rsplit('.').next().unwrap_or(f)).collect();
        dialog = dialog.add_filter(kind.filter.join(", "), &extensions);
    }
    dialog.pick_file()
}

fn text_field(ui: &mut Ui, text: &mut String, width: f32) -> egui::Response {
    ui.add(egui::TextEdit::singleline(text).margin(Margin::symmetric(8, 5)).desired_width(width))
}

/// A path input with its picker button.
fn path_field(ui: &mut Ui, text: &mut String, kind: &PathKind, width: f32) -> (egui::Response, bool) {
    let hint = if kind.directory { t!("path.choose_folder") } else { t!("path.choose_file") };
    let picked = ui.button(icon::FOLDER_OPEN).on_hover_cursor(CursorIcon::PointingHand).on_hover_text(hint).clicked();
    (text_field(ui, text, width - 44.0), picked)
}

fn int_field(ui: &mut Ui, value: &mut i64, min: Option<i64>, max: Option<i64>) -> egui::Response {
    let range = min.unwrap_or(i64::MIN)..=max.unwrap_or(i64::MAX);
    ui.add(egui::DragValue::new(value).range(range).speed(1.0))
}

/// One setting: label (with a marker when it differs from its default), description, the inline save result, a reset
/// button on changed rows, its control by type, and the error of an invalid value in the file.
fn setting_field(ui: &mut Ui, setting: &Setting, value: &mut Value, enabled: bool, feedback: Option<&Feedback>) -> Edit {
    let mut edit = Edit::None;
    let changed = setting.is_changed();
    egui::Frame::new().inner_margin(Margin::symmetric(16, 12)).show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.add_enabled_ui(enabled, |ui| {
            ui.horizontal(|ui| {
                ui.set_min_height(24.0);
                // The text wraps in what the controls on the right leave, so a long description never runs under them.
                let controls = match &setting.kind {
                    Kind::Bool => 40.0,
                    Kind::Float { min: Some(_), max: Some(_), .. } => 160.0 + 70.0,
                    Kind::Path(_) | Kind::String => 300.0,
                    Kind::List(_) | Kind::Enum(_) => 0.0,
                    _ => 120.0,
                } + 38.0;
                let text_width = (ui.available_width() - controls - 24.0).max(160.0);
                ui.vertical(|ui| {
                    ui.set_max_width(text_width);
                    ui.spacing_mut().item_spacing.y = 2.0;
                    ui.horizontal(|ui| {
                        ui.spacing_mut().item_spacing.x = 6.0;
                        ui.label(RichText::new(&setting.label).color(theme::TEXT)).on_hover_text(format!("{} ({})", setting.key, setting.kind.name()));
                        if changed {
                            ui.label(RichText::new(format!("{} {}", icon::DOT_OUTLINE, t!("setting.changed"))).size(11.5).color(theme::ACCENT))
                                .on_hover_text(t!("setting.changed.hint", default = setting.default));
                        }
                        if let Some(Feedback { error: None, at }) = feedback {
                            let alpha = saved_alpha(*at, Instant::now());
                            ui.label(RichText::new(format!("{}  {}", icon::CHECK, t!("common.saved"))).size(12.0).color(theme::GREEN.gamma_multiply(alpha)));
                        }
                    });
                    if !setting.description.is_empty() {
                        ui.add(egui::Label::new(RichText::new(&setting.description).size(12.5).color(theme::MUTED)).wrap());
                    }
                });
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    let control = match (&setting.kind, &mut *value) {
                        (Kind::Bool, Value::Bool(on)) => {
                            let (rect, _) = ui.allocate_exact_size(Vec2::new(40.0, 22.0), Sense::hover());
                            if theme::toggle(ui, rect, ui.id().with(("setting", &setting.key)), on, enabled).changed() { Edit::Commit } else { Edit::None }
                        }
                        (Kind::Int { min, max }, Value::Int(number)) => Edit::of(&int_field(ui, number, *min, *max)),
                        (Kind::Float { min: Some(min), max: Some(max), step }, Value::Float(number)) => {
                            // Right to left: the slider, then its number box on its left.
                            let (rect, _) = ui.allocate_exact_size(Vec2::new(160.0, 22.0), Sense::hover());
                            let id = ui.id().with(("setting", &setting.key));
                            let slid = theme::slider(ui, rect, id, number, *min..=*max, *step, enabled);
                            let decimals = if step.is_some_and(|s| s.fract() == 0.0) { 0 } else { 2 };
                            let typed = ui.add(egui::DragValue::new(number).range(*min..=*max).fixed_decimals(decimals).speed(step.unwrap_or(0.1)));
                            if slid.drag_stopped() || (slid.clicked() && slid.changed()) || (slid.changed() && !slid.dragged()) {
                                Edit::Commit
                            } else if slid.changed() {
                                Edit::Draft
                            } else {
                                Edit::of(&typed)
                            }
                        }
                        (Kind::Float { min, max, step }, Value::Float(number)) => {
                            let range = min.unwrap_or(f64::MIN)..=max.unwrap_or(f64::MAX);
                            Edit::of(&ui.add(egui::DragValue::new(number).range(range).speed(step.unwrap_or(0.1))))
                        }
                        (Kind::String, Value::Str(text)) => Edit::of(&text_field(ui, text, 260.0)),
                        (Kind::Path(kind), Value::Str(text)) => {
                            let (response, picked) = path_field(ui, text, kind, 300.0);
                            if picked { Edit::Pick(None) } else { Edit::of(&response) }
                        }
                        _ => Edit::None,
                    };
                    edit.or(control);
                    // Left of the control, in a slot that is always reserved, so the control never moves.
                    let slot = Vec2::new(30.0, 24.0);
                    if changed {
                        ui.allocate_ui(slot, |ui| {
                            if theme::quiet_button(ui, icon::ARROW_COUNTER_CLOCKWISE, theme::MUTED, theme::ACCENT).on_hover_text(t!("setting.reset.hint", default = setting.default)).clicked() {
                                edit = Edit::Reset;
                            }
                        });
                    } else if !matches!(setting.kind, Kind::List(_) | Kind::Enum(_)) {
                        ui.allocate_exact_size(slot, Sense::hover());
                    }
                });
            });
            if let (Kind::List(element), Value::List(items)) = (&setting.kind, &mut *value) {
                ui.add_space(6.0);
                edit.or(list_editor(ui, element, items));
            }
            if let (Kind::Enum(choices), Value::Str(chosen)) = (&setting.kind, &mut *value) {
                ui.add_space(8.0);
                edit.or(choice_list(ui, choices, chosen, enabled));
            }
            if let Some(why) = &setting.error {
                ui.add_space(4.0);
                let text = format!("{}  {}", icon::WARNING, t!("setting.invalid", why = why, default = setting.default));
                ui.add(egui::Label::new(RichText::new(text).size(12.5).color(theme::RED)).wrap());
            }
            if let Some(Feedback { error: Some(error), .. }) = feedback {
                ui.add_space(4.0);
                ui.add(egui::Label::new(RichText::new(format!("{}  {}", icon::WARNING_CIRCLE, t!("setting.not_saved", error = error))).size(12.5).color(theme::RED)).wrap());
            }
        });
    });
    edit
}

/// The choices of an enum, all visible at once so nothing has to be opened or remembered. With a description on any
/// choice: one card per choice, stacked, its label over its description, so each value sits next to what it does.
/// Without (short labels such as symbols or charsets): compact chips that wrap, which keep a long list in a glance.
fn choice_list(ui: &mut Ui, choices: &[Choice], chosen: &mut String, enabled: bool) -> Edit {
    let mut edit = Edit::None;
    let described = choices.iter().any(|choice| !choice.description.is_empty());
    let mut pick = |ui: &mut Ui, choice: &Choice, width: Option<f32>| {
        if choice_card(ui, choice, *chosen == choice.value, enabled, width).clicked() && *chosen != choice.value {
            *chosen = choice.value.clone();
            edit = Edit::Commit;
        }
    };
    if described {
        let width = ui.available_width().min(620.0);
        ui.spacing_mut().item_spacing.y = 6.0;
        for choice in choices {
            pick(ui, choice, Some(width));
        }
    } else {
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing = Vec2::splat(6.0);
            for choice in choices {
                pick(ui, choice, None);
            }
        });
    }
    edit
}

/// One choice: a radio mark, the label and, when it has one, the description under it in muted text, in a bordered
/// card that takes the accent when selected. `width` fixes the card's width (the description wraps in it); `None`
/// fits the label.
fn choice_card(ui: &mut Ui, choice: &Choice, selected: bool, enabled: bool, width: Option<f32>) -> egui::Response {
    let padding = Vec2::new(12.0, 8.0);
    let mark = 14.0;
    let gap = 10.0;
    let painter = ui.painter().clone();
    let label_font = FontId::new(13.5, if selected { theme::semibold() } else { egui::FontFamily::Proportional });
    let label = painter.layout_no_wrap(choice.label.clone(), label_font, if selected { theme::TEXT } else { theme::SOFT });
    let text_width = width.map_or(label.size().x, |width| width - padding.x * 2.0 - mark - gap);
    let description = (!choice.description.is_empty())
        .then(|| painter.layout(choice.description.clone(), FontId::proportional(12.5), theme::MUTED, text_width.max(80.0)));
    let text_height = label.size().y + description.as_ref().map_or(0.0, |galley| 2.0 + galley.size().y);
    let size = Vec2::new(width.unwrap_or(padding.x * 2.0 + mark + gap + label.size().x), padding.y * 2.0 + text_height);
    let (rect, response) = ui.allocate_exact_size(size, if enabled { Sense::click() } else { Sense::hover() });
    let response = response.on_hover_text(&choice.value);
    response.widget_info(|| egui::WidgetInfo::selected(egui::WidgetType::RadioButton, enabled, selected, &choice.label));
    if ui.is_rect_visible(rect) {
        let hover = ui.ctx().animate_bool_with_time(response.id.with("hover"), enabled && response.hovered(), 0.12);
        let fill = if selected { theme::ACCENT_FILL.gamma_multiply(0.16) } else { theme::lerp_color(theme::FIELD, theme::SURFACE_HOVER, hover) };
        let border = if selected { theme::ACCENT } else { theme::lerp_color(theme::DIVIDER, theme::FAINT, hover) };
        let dim = |color: Color32| if enabled { color } else { color.gamma_multiply(0.5) };
        painter.rect(rect, CornerRadius::same(8), dim(fill), Stroke::new(1.0, dim(border)), StrokeKind::Inside);
        let center = egui::pos2(rect.left() + padding.x + mark / 2.0, rect.top() + padding.y + label.size().y / 2.0);
        painter.circle_stroke(center, mark / 2.0 - 0.75, Stroke::new(1.5, dim(if selected { theme::ACCENT } else { theme::MUTED })));
        if selected {
            painter.circle_filled(center, 3.5, dim(theme::ACCENT));
        }
        let text_left = rect.left() + padding.x + mark + gap;
        let label_height = label.size().y;
        painter.galley(egui::pos2(text_left, rect.top() + padding.y), label, dim(theme::TEXT));
        if let Some(galley) = description {
            painter.galley(egui::pos2(text_left, rect.top() + padding.y + label_height + 2.0), galley, dim(theme::MUTED));
        }
    }
    if enabled { response.on_hover_cursor(CursorIcon::PointingHand) } else { response }
}

/// The rows of a list setting: each element's control, move up/down and remove, then Add.
fn list_editor(ui: &mut Ui, element: &Element, items: &mut Vec<Value>) -> Edit {
    let mut edit = Edit::None;
    let mut moved: Option<(usize, usize)> = None;
    let mut removed = None;
    let count = items.len();
    if count == 0 {
        ui.label(RichText::new(t!("listedit.empty")).size(12.5).color(theme::FAINT));
    }
    for (index, item) in items.iter_mut().enumerate() {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 4.0;
            ui.label(RichText::new(format!("{}.", index + 1)).size(12.5).color(theme::FAINT));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::quiet_button(ui, icon::X, theme::MUTED, theme::RED).on_hover_text(t!("listedit.remove")).clicked() {
                    removed = Some(index);
                }
                if index + 1 < count && theme::quiet_button(ui, icon::ARROW_DOWN, theme::MUTED, theme::TEXT).on_hover_text(t!("listedit.down")).clicked() {
                    moved = Some((index, index + 1));
                }
                if index > 0 && theme::quiet_button(ui, icon::ARROW_UP, theme::MUTED, theme::TEXT).on_hover_text(t!("listedit.up")).clicked() {
                    moved = Some((index, index - 1));
                }
                let width = ui.available_width();
                edit.or(match (element, item) {
                    (Element::Int, Value::Int(number)) => Edit::of(&int_field(ui, number, None, None)),
                    (Element::String, Value::Str(text)) => Edit::of(&text_field(ui, text, width)),
                    (Element::Path(kind), Value::Str(text)) => {
                        let (response, picked) = path_field(ui, text, kind, width);
                        if picked { Edit::Pick(Some(index)) } else { Edit::of(&response) }
                    }
                    _ => Edit::None,
                });
            });
        });
    }
    if let Some((from, to)) = moved {
        items.swap(from, to);
        edit = Edit::Commit;
    }
    if let Some(index) = removed {
        items.remove(index);
        edit = Edit::Commit;
    }
    ui.add_space(2.0);
    if theme::quiet_button(ui, &format!("{}  {}", icon::PLUS, t!("listedit.add")), theme::ACCENT, theme::ACCENT).clicked() {
        edit = match element {
            Element::Path(_) => Edit::Pick(Some(count)),
            Element::Int => {
                items.push(Value::Int(0));
                Edit::Commit
            }
            Element::String => {
                items.push(Value::Str(String::new()));
                Edit::Commit
            }
        };
    }
    edit
}
