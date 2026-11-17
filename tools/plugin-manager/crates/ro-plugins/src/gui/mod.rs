//! The window: a quiet top bar (game folder, one state chip, install, settings), the plugin list on the left and the
//! selected plugin on the right. Each plugin shows one status in plain words; what the host recorded and the raw
//! files sit under "Technical details". Changes go through the same core rules as the CLI. It repaints only on
//! input, file events and process changes.

mod model;
mod theme;

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use eframe::egui::{
    self, Align, Align2, Color32, CornerRadius, CursorIcon, FontId, Id, Key, Layout, Margin, Modifiers, Rect, RichText,
    ScrollArea, Sense, Shadow, Stroke, StrokeKind, Ui, Vec2,
};
use egui_phosphor::regular as icon;
use ro_plugins_core::manifest::{self, PluginJson};
use ro_plugins_core::ops::{Change, Client, EXIT_RUNNING, Outcome};
use ro_plugins_core::resolve::{self, State};
use ro_plugins_core::status::Processes;

use crate::paths;
use model::{BuildListing, FileWatch, GameState, Health, ProcessWatch, Snapshot, Tone};

const KEY_CLIENT: &str = "client";
const KEY_BUILD: &str = "build";
const LIST_WIDTH: f32 = 288.0;
const CONTENT_WIDTH: f32 = 720.0;

pub fn run(client: Option<PathBuf>, build: Option<PathBuf>) -> Result<(), String> {
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
            // A path on the command line wins over the stored setting, which wins over the defaults.
            let absolute = |p: PathBuf| if p.as_os_str().is_empty() { p } else { std::path::absolute(&p).unwrap_or(p) };
            let client = absolute(client.or_else(|| stored(KEY_CLIENT)).or_else(paths::default_client).unwrap_or_default());
            let build = absolute(build.or_else(|| stored(KEY_BUILD)).or_else(paths::default_build).unwrap_or_default());
            Ok(Box::new(App::new(cc.egui_ctx.clone(), client, build)))
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

struct UninstallDialog {
    name: String,
    dependents: Vec<String>,
}

struct Toast {
    text: String,
    tone: Tone,
    /// disabled.txt as it was before the change.
    undo: Option<Vec<String>>,
    until: Instant,
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
    PickClient,
    PickBuild,
    Reload,
}

struct App {
    ctx: egui::Context,
    client: PathBuf,
    build: PathBuf,
    snapshot: Option<Snapshot>,
    build_listing: Option<BuildListing>,
    descriptions: BTreeMap<String, PluginJson>,
    game: GameState,
    processes: ProcessWatch,
    files: FileWatch,
    toast: Option<Toast>,
    selected: Option<String>,
    search: String,
    focus_search: bool,
    page: Page,
    uninstall: Option<UninstallDialog>,
    actions: Vec<Action>,
}

fn search_id() -> Id {
    Id::new("plugin-search")
}

fn folder_name(path: &Path) -> String {
    path.file_name().map_or_else(|| path.display().to_string(), |name| name.to_string_lossy().into_owned())
}

/// `first, second and third`.
fn list(names: &[String]) -> String {
    match names {
        [] => String::new(),
        [one] => one.clone(),
        [head @ .., last] => format!("{} and {last}", head.join(", ")),
    }
}

fn plugins(count: usize) -> String {
    if count == 1 { "1 plugin".to_owned() } else { format!("{count} plugins") }
}

/// A core refusal as a sentence for the window.
fn sentence(message: &str) -> String {
    let text = message.strip_prefix("refused: ").unwrap_or(message);
    let mut chars = text.chars();
    chars.next().map_or_else(String::new, |first| first.to_uppercase().chain(chars).collect())
}

impl App {
    fn new(ctx: egui::Context, client: PathBuf, build: PathBuf) -> App {
        let processes = ProcessWatch::start(ctx.clone(), client.clone());
        let build_plugins = paths::build_plugins(&build);
        let files = FileWatch::start(ctx.clone(), &client, Some(&build_plugins));
        let mut app = App {
            ctx,
            client,
            build,
            snapshot: None,
            build_listing: None,
            descriptions: model::descriptions(paths::plugin_sources().as_deref()),
            game: processes.state(),
            processes,
            files,
            toast: None,
            selected: None,
            search: String::new(),
            focus_search: false,
            page: Page::Plugins,
            uninstall: None,
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

    fn reload(&mut self) {
        self.snapshot = self.client_ok().then(|| Snapshot::read(&self.client, self.game.recorded_image.as_deref()));
        let installed = self.snapshot.as_ref().map(|s| s.found.clone()).unwrap_or_default();
        let build_plugins = paths::build_plugins(&self.build);
        self.build_listing =
            (!self.build.as_os_str().is_empty() && build_plugins.is_dir()).then(|| BuildListing::read(&build_plugins, &installed));
        if let (Some(snapshot), Some(selected)) = (&self.snapshot, &self.selected) {
            if !snapshot.names.contains(selected) {
                self.selected = None;
            }
        }
        if self.selected.is_none() {
            self.selected = self.visible().into_iter().flatten().next();
        }
    }

    fn set_client(&mut self, client: PathBuf) {
        self.client = client;
        self.selected = None;
        self.page = Page::Plugins;
        self.processes.set_client(self.client.clone());
        self.game = self.processes.state();
        self.files = FileWatch::start(self.ctx.clone(), &self.client, Some(&paths::build_plugins(&self.build)));
        self.reload();
    }

    fn set_build(&mut self, build: PathBuf) {
        self.build = build;
        self.files = FileWatch::start(self.ctx.clone(), &self.client, Some(&paths::build_plugins(&self.build)));
        self.reload();
    }

    /// The list as shown: plugins that need attention first, then the rest, both filtered by the search.
    fn visible(&self) -> [Vec<String>; 2] {
        let Some(snapshot) = &self.snapshot else { return [Vec::new(), Vec::new()] };
        let needle = self.search.trim().to_lowercase();
        let (attention, rest) = snapshot
            .names
            .iter()
            .filter(|name| needle.is_empty() || name.to_lowercase().contains(&needle))
            .cloned()
            .partition(|name| snapshot.health[name].needs_attention());
        [attention, rest]
    }

    fn notify(&mut self, tone: Tone, text: impl Into<String>, undo: Option<Vec<String>>) {
        let seconds = if tone == Tone::Bad { 8 } else { 6 };
        self.toast = Some(Toast { text: text.into(), tone, undo, until: Instant::now() + Duration::from_secs(seconds) });
    }

    /// Runs a core operation against a fresh process table and reloads; a refusal comes back as a sentence.
    fn run_op(&mut self, op: impl FnOnce(&Client, &Processes, &mut Vec<String>) -> Outcome) -> Result<(), String> {
        let Some(snapshot) = &self.snapshot else { return Err("Choose a game folder first.".to_owned()) };
        let client = snapshot.client.clone();
        let mut out = Vec::new();
        let outcome = {
            let processes = self.processes.fresh();
            op(&client, &processes, &mut out)
        };
        self.reload();
        outcome.map(drop).map_err(|refused| {
            if refused.code == EXIT_RUNNING {
                "Close the game first: plugins can't change while it runs.".to_owned()
            } else {
                sentence(&refused.message)
            }
        })
    }

    fn set_off(&mut self, name: String, off: bool) {
        let Some(snapshot) = &self.snapshot else { return };
        let before = snapshot.disabled.clone();
        let text = if off {
            let blocked: Vec<String> = snapshot.dependents(&name).into_iter().filter(|user| !snapshot.is_off(user)).collect();
            if blocked.is_empty() { format!("{name} turned off") } else { format!("{name} turned off. {} can't load without it.", list(&blocked)) }
        } else {
            let also: Vec<String> =
                resolve::requirements(&snapshot.found, &name).into_iter().filter(|needed| *needed != name && snapshot.is_off(needed)).collect();
            if also.is_empty() { format!("{name} turned on") } else { format!("{name} turned on, along with {}", list(&also)) }
        };
        let result = self.run_op(|client, processes, out| if off { client.disable(&name, processes, out) } else { client.enable(&name, processes, out) });
        match result {
            Ok(()) => self.notify(Tone::Good, text, Some(before)),
            Err(error) => self.notify(Tone::Bad, error, None),
        }
    }

    fn apply(&mut self, action: Action) {
        match action {
            Action::Select(name) => {
                self.selected = Some(name);
                self.page = Page::Plugins;
            }
            Action::SetOff(name, off) => self.set_off(name, off),
            Action::RestoreDisabled(names) => {
                let result = if self.running() {
                    Err("Close the game first: plugins can't change while it runs.".to_owned())
                } else {
                    manifest::write_disabled(&Client::new(&self.client).plugins, &names).map_err(|error| format!("Could not undo: {error}"))
                };
                self.reload();
                match result {
                    Ok(()) => self.notify(Tone::Good, "Change undone", None),
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
                let source = paths::build_plugins(&self.build);
                let planned = self.snapshot.as_ref().and_then(|s| s.client.plan_install(&source, &names).ok()).map_or(names.len(), |plan| plan.items.len());
                match self.run_op(|client, processes, out| client.install(&source, &names, force, processes, out)) {
                    Ok(()) => {
                        self.page = Page::Plugins;
                        self.selected = names.first().cloned();
                        let text = if planned == 1 { format!("{} installed", names[0]) } else { format!("{} installed", plugins(planned)) };
                        self.notify(Tone::Good, text, None);
                    }
                    Err(error) => self.notify(Tone::Bad, error, None),
                }
            }
            Action::AskUninstall(name) => {
                let dependents = self.snapshot.as_ref().map(|s| s.dependents(&name)).unwrap_or_default();
                self.uninstall = Some(UninstallDialog { name, dependents });
            }
            Action::Uninstall(name) => {
                let others = self.snapshot.as_ref().map(|s| s.dependents(&name)).unwrap_or_default();
                match self.run_op(|client, processes, out| client.uninstall(&name, !others.is_empty(), processes, out)) {
                    Ok(()) if others.is_empty() => self.notify(Tone::Good, format!("{name} uninstalled"), None),
                    Ok(()) => self.notify(Tone::Good, format!("{name} uninstalled, along with {}", list(&others)), None),
                    Err(error) => self.notify(Tone::Bad, error, None),
                }
            }
            Action::OpenFolder(path) => {
                if let Err(error) = crate::open::open_folder(&path) {
                    self.notify(Tone::Bad, format!("Could not open the folder: {error}"), None);
                }
            }
            Action::PickClient => {
                let mut dialog = rfd::FileDialog::new().set_title("Choose the game folder (the one holding the exe)");
                if self.client.is_dir() {
                    dialog = dialog.set_directory(&self.client);
                }
                if let Some(dir) = dialog.pick_folder() {
                    self.set_client(dir);
                }
            }
            Action::PickBuild => {
                let mut dialog = rfd::FileDialog::new().set_title("Choose the build output (build/ or build/plugins)");
                if self.build.is_dir() {
                    dialog = dialog.set_directory(&self.build);
                }
                if let Some(dir) = dialog.pick_folder() {
                    self.set_build(dir);
                }
            }
            Action::Reload => {
                self.descriptions = model::descriptions(paths::plugin_sources().as_deref());
                self.reload();
            }
        }
    }

    /// Up/Down move through the list, Space toggles, Ctrl+F searches, Esc backs out.
    fn keyboard(&mut self, ctx: &egui::Context) {
        if self.uninstall.is_some() {
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
            let names: Vec<String> = self.visible().into_iter().flatten().collect();
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
        if !typing && ctx.input_mut(|input| input.consume_key(Modifiers::NONE, Key::Space)) {
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
            ui.label(RichText::new("Plugins").font(FontId::new(14.5, theme::semibold())).color(theme::TEXT));
            ui.add_space(12.0);

            let text = if self.client.as_os_str().is_empty() {
                RichText::new(format!("{}  Choose game folder", icon::FOLDER)).color(theme::ACCENT)
            } else {
                RichText::new(format!("{}  {}  {}", icon::FOLDER, folder_name(&self.client), icon::CARET_DOWN)).color(theme::SOFT)
            };
            let button = egui::Button::new(text).fill(theme::SURFACE).corner_radius(CornerRadius::same(8));
            let response = ui.add(button).on_hover_cursor(CursorIcon::PointingHand);
            let response = if self.client.as_os_str().is_empty() {
                response.on_hover_text("The folder that holds the game exe and its plugins folder")
            } else {
                response.on_hover_text(format!("{}\nClick to choose another game folder", self.client.display()))
            };
            if response.clicked() {
                self.actions.push(Action::PickClient);
            }

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 8.0;
                self.settings_menu(ui);
                let Some(snapshot) = &self.snapshot else { return };
                if matches!(self.page, Page::Plugins) {
                    let button = theme::primary(format!("{}  Install", icon::PLUS));
                    if ui.add(button).on_hover_cursor(CursorIcon::PointingHand).on_hover_text("Install plugins from the build output").clicked() {
                        self.actions.push(Action::OpenInstall(None));
                    }
                }
                // One state at a time, and only when it matters.
                let chip = if let Some(process) = self.game.running.first() {
                    Some((format!("{}  Game running · changes locked", icon::LOCK_SIMPLE), theme::AMBER, format!("{} is running (pid {})", process.name(), process.pid)))
                } else if snapshot.pending() > 0 {
                    Some(("Changes apply at the next game start".to_owned(), theme::BLUE, "The game has not started since these plugins changed.".to_owned()))
                } else {
                    None
                };
                if let Some((text, color, hint)) = chip {
                    theme::chip(ui, &text, color).on_hover_ui(|ui| {
                        ui.label(hint);
                        for note in &snapshot.freshness.notes {
                            ui.label(RichText::new(note).size(12.0).color(theme::MUTED));
                        }
                    });
                }
            });
        });
    }

    fn settings_menu(&mut self, ui: &mut Ui) {
        let gear = RichText::new(icon::GEAR_SIX).size(16.0).color(theme::MUTED);
        ui.menu_button(gear, |ui| {
            ui.set_min_width(320.0);
            ui.spacing_mut().item_spacing.y = 4.0;
            let mut folder = |ui: &mut Ui, title: &str, path: &Path, action: Action| {
                ui.label(RichText::new(title).font(FontId::new(12.5, theme::semibold())).color(theme::MUTED));
                ui.horizontal(|ui| {
                    let shown = if path.as_os_str().is_empty() { "Not set".to_owned() } else { path.display().to_string() };
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if theme::quiet_button(ui, "Change", theme::ACCENT, theme::ACCENT).clicked() {
                            self.actions.push(action);
                            ui.close();
                        }
                        ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                            ui.add(egui::Label::new(RichText::new(shown).color(theme::SOFT)).truncate());
                        });
                    });
                });
            };
            folder(ui, "Game folder", &self.client, Action::PickClient);
            ui.add_space(6.0);
            folder(ui, "Build output", &self.build, Action::PickBuild);
            ui.add_space(6.0);
            ui.separator();
            if theme::quiet_button(ui, &format!("{}  Reload from disk", icon::ARROWS_CLOCKWISE), theme::SOFT, theme::TEXT).clicked() {
                self.actions.push(Action::Reload);
                ui.close();
            }
            ui.label(RichText::new(format!("ro-plugins {}", env!("CARGO_PKG_VERSION"))).size(11.5).color(theme::FAINT));
        })
        .response
        .on_hover_text("Settings");
    }

    // ---- left: plugin list ----------------------------------------------------------------------------------

    fn plugin_list(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else { return };
        let search = egui::TextEdit::singleline(&mut self.search)
            .id(search_id())
            .hint_text(RichText::new(format!("{}  Search", icon::MAGNIFYING_GLASS)).color(theme::FAINT))
            .margin(Margin::symmetric(10, 8))
            .desired_width(f32::INFINITY);
        ui.add(search);
        if std::mem::take(&mut self.focus_search) {
            ui.memory_mut(|memory| memory.request_focus(search_id()));
        }
        ui.add_space(8.0);

        let running = !self.game.running.is_empty();
        let needle = self.search.trim().to_lowercase();
        let shown = |name: &&String| needle.is_empty() || name.to_lowercase().contains(&needle);
        let (attention, rest): (Vec<&String>, Vec<&String>) =
            snapshot.names.iter().filter(shown).partition(|name| snapshot.health[*name].needs_attention());
        ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 2.0;
            let grouped = !attention.is_empty();
            for (title, names) in [("Needs attention", &attention), ("Everything else", &rest)] {
                if names.is_empty() {
                    continue;
                }
                if grouped {
                    ui.add_space(6.0);
                    ui.horizontal(|ui| {
                        ui.add_space(10.0);
                        ui.label(RichText::new(title).size(11.5).color(theme::FAINT));
                    });
                    ui.add_space(2.0);
                }
                for name in names {
                    let selected = matches!(self.page, Page::Plugins) && self.selected.as_ref() == Some(*name);
                    if let Some(action) = plugin_row(ui, snapshot, name, selected, running) {
                        self.actions.push(action);
                    }
                }
            }
            if attention.is_empty() && rest.is_empty() {
                ui.add_space(28.0);
                ui.vertical_centered(|ui| {
                    let text = if snapshot.names.is_empty() { "No plugins yet" } else { "No plugin matches" };
                    ui.label(RichText::new(text).color(theme::MUTED));
                });
            }
            let problems = snapshot.errors.iter().map(|error| format!("Unreadable plugin file: {error}")).chain(self.files.error.clone());
            for problem in problems {
                ui.add_space(8.0);
                notice(ui, icon::WARNING, theme::AMBER, &problem);
            }
        });
    }

    // ---- right: the selected plugin ---------------------------------------------------------------------------

    fn details(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else { return };
        let running = !self.game.running.is_empty();
        let Some(name) = self.selected.clone().filter(|name| snapshot.health.contains_key(name)) else {
            let can_install = self.build_listing.as_ref().is_some_and(|listing| !listing.plugins.is_empty());
            if snapshot.names.is_empty() {
                empty_state(ui, icon::PUZZLE_PIECE, "No plugins installed", "Plugins you install into this game folder show up here.");
                if can_install {
                    ui.vertical_centered(|ui| {
                        if ui.add(theme::primary("Install plugins")).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                            self.actions.push(Action::OpenInstall(None));
                        }
                    });
                }
            } else {
                empty_state(ui, icon::PUZZLE_PIECE, "Select a plugin", "Pick one from the list to see what it needs and whether it is running.");
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
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 2.0;
                if manifest.is_some() {
                    if running {
                        theme::quiet_button(ui, "Uninstall", theme::FAINT, theme::FAINT).on_hover_text("Close the game to uninstall plugins");
                    } else if theme::quiet_button(ui, "Uninstall", theme::MUTED, theme::RED).clicked() {
                        self.actions.push(Action::AskUninstall(name.clone()));
                    }
                }
                if theme::quiet_button(ui, "Open folder", theme::MUTED, theme::TEXT).clicked() {
                    self.actions.push(Action::OpenFolder(snapshot.client.plugins.clone()));
                }
            });
        });
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing.x = 8.0;
            let (dot, _) = ui.allocate_exact_size(Vec2::new(8.0, 18.0), Sense::hover());
            status_dot(ui.painter(), dot.center(), health.tone);
            ui.label(RichText::new(health.label).font(FontId::new(13.5, theme::semibold())).color(theme::lerp_color(color, theme::TEXT, 0.3)));
            if let Some(detail) = &health.detail {
                ui.label(RichText::new(detail).color(theme::MUTED));
            }
        });
        if let Some(text) = self.descriptions.get(&name).map(|json| json.description.as_str()).filter(|text| !text.is_empty()) {
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
                        ui.label(RichText::new("Enabled").color(theme::TEXT));
                        let hint = if running {
                            "Locked while the game is running."
                        } else if !on {
                            "Stays installed, but the game skips it."
                        } else if health.needs_attention() {
                            "On, but it can't load until the problem above is fixed."
                        } else {
                            "Loads when the game starts."
                        };
                        ui.label(RichText::new(hint).size(12.5).color(theme::MUTED));
                    });
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        let (rect, _) = ui.allocate_exact_size(Vec2::new(40.0, 22.0), Sense::hover());
                        if theme::toggle(ui, rect, ui.id().with("enabled"), &mut on, !running).changed() {
                            self.actions.push(Action::SetOff(name.clone(), !on));
                        }
                    });
                });
            });
        }

        let depends = manifest.map(|m| m.depends.as_slice()).unwrap_or_default();
        if !depends.is_empty() {
            ui.add_space(22.0);
            theme::section(ui, "Requires");
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
                        None => Some(("Not installed".to_owned(), built.map(|_| ("Install", install())))),
                        Some(target) if !dependency.range.satisfied_by(&target.version) => {
                            let fix = built.filter(|a| dependency.range.satisfied_by(&a.manifest.version)).map(|_| ("Update", install()));
                            Some((format!("{} installed", target.version), fix))
                        }
                        Some(_) if state == Some(State::Off) => {
                            Some(("Turned off".to_owned(), (!running).then(|| ("Turn on", Action::SetOff(dependency.name.clone(), false)))))
                        }
                        Some(_) if matches!(state, Some(State::Disabled | State::FailedLoad)) => Some(("Can't load".to_owned(), None)),
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
            theme::section(ui, "Used by");
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
        egui::CollapsingHeader::new(RichText::new("Technical details").size(13.0).color(theme::MUTED)).id_salt("technical").show(ui, |ui| {
            ui.add_space(4.0);
            let prediction = &snapshot.predicted[&name];
            let with_reason = |state: &str, reason: &str| if reason.is_empty() { state.to_owned() } else { format!("{state} · {reason}") };
            let recorded = snapshot.status.plugins.get(&name).map_or_else(|| "no record".to_owned(), |r| with_reason(&r.state, &r.reason));
            let files = crate::cli::plugin_files(&snapshot.client.plugins, &name)
                .into_iter()
                .map(|file| {
                    let size = std::fs::metadata(snapshot.client.plugins.join(&file)).map(|m| m.len()).unwrap_or(0);
                    format!("{file} ({})", human_size(size))
                })
                .collect::<Vec<_>>();
            egui::Grid::new("technical").num_columns(2).spacing([20.0, 6.0]).show(ui, |ui| {
                for (key, value) in [
                    ("Last game run", recorded),
                    ("Installed files", with_reason(prediction.state.as_str(), &prediction.reason)),
                    ("Files", if files.is_empty() { "none".to_owned() } else { files.join("   ") }),
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

    // ---- install ----------------------------------------------------------------------------------------------

    fn install_page(&mut self, ui: &mut Ui) {
        let Page::Install(form) = &mut self.page else { return };
        let Some(snapshot) = &self.snapshot else { return };
        let running = !self.game.running.is_empty();
        ui.label(RichText::new("Install plugins").font(FontId::new(22.0, theme::semibold())).color(theme::TEXT));
        ui.label(RichText::new("Copies built plugins into this game folder. Installed plugins are updated in place.").color(theme::MUTED));
        ui.add_space(18.0);

        let Some(listing) = self.build_listing.as_ref().filter(|listing| !listing.plugins.is_empty()) else {
            empty_state(ui, icon::PACKAGE, "Nothing built yet", "Build the plugins first, or point to another build output.");
            ui.vertical_centered(|ui| {
                if ui.add(theme::primary("Choose build output")).on_hover_cursor(CursorIcon::PointingHand).clicked() {
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
                    Change::New => ("New", theme::BLUE),
                    Change::Upgrade => ("Update", theme::GREEN),
                    Change::Downgrade => ("Downgrade", theme::AMBER),
                    Change::Same => ("Reinstall", theme::MUTED),
                };
                let version = match &available.installed {
                    Some(installed) if *installed != available.manifest.version => format!("{installed}  {}  {}", icon::ARROW_RIGHT, available.manifest.version),
                    _ => available.manifest.version.to_string(),
                };
                let sub = (!requires.is_empty()).then(|| format!("Requires {}", requires.join(", ")));
                if install_row(ui, checked, name, sub.as_deref(), &version, tag, color) {
                    if checked {
                        form.checked.remove(name);
                    } else {
                        form.checked.insert(name.clone());
                    }
                }
                for extra in pulled.get(name.as_str()).into_iter().flatten() {
                    let (rect, _) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 26.0), Sense::hover());
                    let text = format!("{}  also installs {extra}", icon::ARROW_ELBOW_DOWN_RIGHT);
                    ui.painter().text(rect.left_center() + Vec2::new(48.0, -4.0), Align2::LEFT_CENTER, text, FontId::proportional(12.5), theme::MUTED);
                }
            }
        });
        for error in &listing.errors {
            ui.add_space(8.0);
            notice(ui, icon::WARNING, theme::AMBER, &format!("Unreadable build file: {error}"));
        }

        let mut blocked = wanted.is_empty() || running;
        match &plan {
            Some(Ok(plan)) if !plan.problems.is_empty() => {
                for problem in &plan.problems {
                    ui.add_space(8.0);
                    notice(ui, icon::WARNING, theme::AMBER, &format!("{}.", sentence(problem)));
                }
                ui.add_space(6.0);
                ui.checkbox(&mut form.force, "Install anyway. The affected plugins won't load until this is fixed.");
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
                ui.label(RichText::new(format!("{}  Close the game to install plugins.", icon::LOCK_SIMPLE)).color(theme::AMBER));
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let label = if count == 0 { "Install".to_owned() } else { format!("Install {}", plugins(count)) };
                if ui.add_enabled(!blocked, theme::primary(label)).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                    self.actions.push(Action::Install(wanted.clone(), form.force));
                }
                if theme::quiet_button(ui, "Cancel", theme::MUTED, theme::TEXT).clicked() {
                    self.actions.push(Action::Select(self.selected.clone().unwrap_or_default()));
                }
            });
        });
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
            ui.label(RichText::new(format!("Uninstall {}?", dialog.name)).font(FontId::new(17.0, theme::semibold())).color(theme::TEXT));
            ui.add_space(2.0);
            ui.label(RichText::new("Its files are removed from this game folder. You can install it again from the build output.").color(theme::MUTED));
            if !dialog.dependents.is_empty() {
                ui.add_space(10.0);
                ui.label(RichText::new("These plugins need it and are uninstalled too:").color(theme::SOFT));
                for name in &dialog.dependents {
                    ui.label(RichText::new(format!("•  {name}")).color(theme::TEXT));
                }
            }
            ui.add_space(16.0);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let label = if dialog.dependents.is_empty() { "Uninstall".to_owned() } else { format!("Uninstall {}", plugins(dialog.dependents.len() + 1)) };
                if ui.add(theme::destructive(label)).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                    confirm = true;
                }
                if theme::quiet_button(ui, "Cancel", theme::MUTED, theme::TEXT).clicked() {
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
                        if toast.undo.is_some() && theme::quiet_button(ui, "Undo", theme::ACCENT, theme::ACCENT).clicked() {
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
        self.keyboard(&ctx);

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
                    ui.set_max_width(CONTENT_WIDTH);
                    if self.snapshot.is_none() {
                        empty_state(ui, icon::FOLDER, "Choose your game folder", "Pick the folder that holds the game exe to see and manage its plugins.");
                        ui.vertical_centered(|ui| {
                            if ui.add(theme::primary("Choose game folder")).on_hover_cursor(CursorIcon::PointingHand).clicked() {
                                self.actions.push(Action::PickClient);
                            }
                        });
                    } else if matches!(self.page, Page::Install(_)) {
                        self.install_page(ui);
                    } else {
                        self.details(ui);
                    }
                });
            });
        });

        self.uninstall_dialog(&ctx);
        self.show_toast(&ctx);

        for action in std::mem::take(&mut self.actions) {
            self.apply(action);
            ctx.request_repaint();
        }
    }

    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        storage.set_string(KEY_CLIENT, self.client.to_string_lossy().into_owned());
        storage.set_string(KEY_BUILD, self.build.to_string_lossy().into_owned());
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
    match &health.detail {
        Some(detail) => format!("{} · {detail}", health.label),
        None => health.label.to_owned(),
    }
}

/// One list row: status dot, name, version, switch. The switch is registered after the row, so it gets its click.
fn plugin_row(ui: &mut Ui, snapshot: &Snapshot, name: &str, selected: bool, running: bool) -> Option<Action> {
    let health = &snapshot.health[name];
    let (rect, response) = ui.allocate_exact_size(Vec2::new(ui.available_width(), 44.0), Sense::click());
    let hover = ui.ctx().animate_bool_with_time(response.id.with("hover"), response.hovered(), 0.12);
    let active = ui.ctx().animate_bool_with_time(response.id.with("selected"), selected, 0.12);
    let has_switch = snapshot.found.contains_key(name);
    let switch = Rect::from_center_size(egui::pos2(rect.right() - 28.0, rect.center().y), Vec2::new(32.0, 18.0));

    if ui.is_rect_visible(rect) {
        let painter = ui.painter();
        let fill = theme::lerp_color(theme::lerp_color(theme::CHROME, theme::ROW_HOVER, hover), theme::SURFACE, active);
        painter.rect_filled(rect, CornerRadius::same(8), fill);
        if active > 0.0 {
            let bar = Rect::from_center_size(egui::pos2(rect.left() + 1.5, rect.center().y), Vec2::new(3.0, 18.0 * active));
            painter.rect_filled(bar, CornerRadius::same(2), theme::ACCENT);
        }
        status_dot(painter, egui::pos2(rect.left() + 18.0, rect.center().y), health.tone);
        let ink = if health.tone == Tone::Off { theme::MUTED } else { theme::TEXT };
        let title = painter.layout_no_wrap(name.to_owned(), FontId::new(13.5, theme::semibold()), ink);
        let text_right = if has_switch { switch.left() - 10.0 } else { rect.right() - 12.0 };
        let clipped = painter.with_clip_rect(Rect::from_min_max(rect.min, egui::pos2(text_right, rect.bottom())));
        let origin = egui::pos2(rect.left() + 32.0, rect.center().y - title.size().y / 2.0);
        let after = origin.x + title.size().x + 8.0;
        clipped.galley(origin, title, ink);
        if let Some(manifest) = snapshot.found.get(name) {
            clipped.text(egui::pos2(after, rect.center().y + 0.5), Align2::LEFT_CENTER, manifest.version.to_string(), FontId::proportional(12.0), theme::FAINT);
        }
    }

    let mut action = None;
    if has_switch {
        let mut on = !snapshot.is_off(name);
        let toggled = theme::toggle(ui, switch, ui.id().with(("switch", name)), &mut on, !running);
        let toggled = if running { toggled.on_hover_text("Close the game to change plugins") } else { toggled };
        if toggled.changed() {
            action = Some(Action::SetOff(name.to_owned(), !on));
        }
    }
    let response = response.on_hover_cursor(CursorIcon::PointingHand).on_hover_text(status_hint(health));
    if response.clicked() && action.is_none() {
        action = Some(Action::Select(name.to_owned()));
    }
    action
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
