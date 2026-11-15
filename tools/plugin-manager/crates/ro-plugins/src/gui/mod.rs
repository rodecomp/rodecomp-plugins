//! The window: a top bar (client folder, game status, status.txt freshness), a searchable plugin list on the left,
//! and the selected plugin's details or the dependency graph on the right. Mutations go through the same core rules
//! as the CLI and report as toasts. It repaints only on input, file events and process changes.

mod graph;
mod model;
mod theme;

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::time::Duration;

use eframe::egui::{
    self, Align, Color32, CornerRadius, FontId, Layout, Margin, RichText, ScrollArea, Sense, Stroke, Ui, Vec2,
};
use egui_phosphor::regular as icon;
use ro_plugins_core::manifest::PluginJson;
use ro_plugins_core::ops::{Change, EXIT_RUNNING, Outcome};
use ro_plugins_core::resolve::State;

use crate::paths;
use model::{BuildListing, FileWatch, GameState, ProcessWatch, Snapshot};

const KEY_CLIENT: &str = "client";
const KEY_BUILD: &str = "build";

pub fn run(client: Option<PathBuf>, build: Option<PathBuf>) -> Result<(), String> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("RO Plugins")
            .with_app_id("ro-plugins")
            .with_inner_size([1180.0, 740.0])
            .with_min_inner_size([820.0, 520.0]),
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

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Details,
    Graph,
}

struct InstallDialog {
    checked: BTreeSet<String>,
    force: bool,
}

struct UninstallDialog {
    name: String,
    dependents: Vec<String>,
    force: bool,
}

enum Action {
    Select(String),
    SetOff(String, bool),
    OpenInstall,
    Install(Vec<String>, bool),
    AskUninstall(String),
    Uninstall(String, bool),
    OpenFolder(PathBuf),
    PickClient,
    PickBuild,
    Refresh,
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
    toasts: egui_notify::Toasts,
    selected: Option<String>,
    search: String,
    view: View,
    install: Option<InstallDialog>,
    uninstall: Option<UninstallDialog>,
    actions: Vec<Action>,
}

fn file_name(path: &Path) -> String {
    path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

impl App {
    fn new(ctx: egui::Context, client: PathBuf, build: PathBuf) -> App {
        let processes = ProcessWatch::start(ctx.clone(), client.clone());
        let build_plugins = paths::build_plugins(&build);
        let files = FileWatch::start(ctx.clone(), &client, Some(&build_plugins));
        let toasts = egui_notify::Toasts::default()
            .with_anchor(egui_notify::Anchor::BottomRight)
            .with_margin(Vec2::new(16.0, 16.0))
            .with_padding(Vec2::new(14.0, 12.0))
            .with_default_font(FontId::proportional(13.5));
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
            toasts,
            selected: None,
            search: String::new(),
            view: View::Details,
            install: None,
            uninstall: None,
            actions: Vec::new(),
        };
        app.reload();
        app
    }

    fn client_ok(&self) -> bool {
        !self.client.as_os_str().is_empty() && self.client.is_dir()
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
            self.selected = self.snapshot.as_ref().and_then(|s| s.names.first().cloned());
        }
    }

    fn set_client(&mut self, client: PathBuf) {
        self.client = client;
        self.selected = None;
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

    fn running(&self) -> bool {
        !self.game.running.is_empty()
    }

    /// Runs a mutation and reports it as a toast.
    fn report(&mut self, what: &str, run: impl FnOnce(&ro_plugins_core::ops::Client, &ro_plugins_core::status::Processes, &mut Vec<String>) -> Outcome) {
        let Some(snapshot) = &self.snapshot else { return };
        let client = snapshot.client.clone();
        let mut out = Vec::new();
        let outcome = {
            let processes = self.processes.fresh();
            run(&client, &processes, &mut out)
        };
        let detail = out.join("\n");
        let duration = Some(Duration::from_secs(6));
        match outcome {
            Ok(0) => {
                self.toasts.success(if detail.is_empty() { what.to_owned() } else { detail }).duration(duration);
            }
            Ok(_) => {
                self.toasts.warning(detail).duration(duration);
            }
            Err(refused) => {
                let text = if detail.is_empty() { refused.message } else { format!("{detail}\n{}", refused.message) };
                if refused.code == EXIT_RUNNING {
                    self.toasts.warning(text).duration(Some(Duration::from_secs(8)));
                } else {
                    self.toasts.error(text).duration(Some(Duration::from_secs(8)));
                }
            }
        }
        self.reload();
    }

    fn apply(&mut self, action: Action) {
        match action {
            Action::Select(name) => self.selected = Some(name),
            Action::SetOff(name, off) => {
                if off {
                    self.report("turned off", |c, p, o| c.disable(&name, p, o));
                } else {
                    self.report("turned on", |c, p, o| c.enable(&name, p, o));
                }
            }
            Action::OpenInstall => {
                let checked = self
                    .build_listing
                    .as_ref()
                    .map(|b| b.plugins.iter().filter(|a| a.change() != Change::Same).map(|a| a.manifest.name.clone()).collect())
                    .unwrap_or_default();
                self.install = Some(InstallDialog { checked, force: false });
            }
            Action::Install(names, force) => {
                let source = paths::build_plugins(&self.build);
                self.report("installed", |c, p, o| c.install(&source, &names, force, p, o));
            }
            Action::AskUninstall(name) => {
                let dependents = self.snapshot.as_ref().map(|s| s.dependents(&name)).unwrap_or_default();
                self.uninstall = Some(UninstallDialog { name, dependents, force: false });
            }
            Action::Uninstall(name, force) => self.report("uninstalled", |c, p, o| c.uninstall(&name, force, p, o)),
            Action::OpenFolder(path) => {
                if let Err(error) = open_folder(&path) {
                    self.toasts.error(format!("cannot open {}: {error}", path.display()));
                }
            }
            Action::PickClient => {
                let mut dialog = rfd::FileDialog::new().set_title("Choose the client folder (the one holding the exe)");
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
            Action::Refresh => {
                self.descriptions = model::descriptions(paths::plugin_sources().as_deref());
                self.reload();
            }
        }
    }

    // ---- top bar ---------------------------------------------------------------------------------------------

    fn top_bar(&mut self, ui: &mut Ui) {
        ui.horizontal(|ui| {
            ui.spacing_mut().item_spacing.x = 10.0;
            let (logo, _) = ui.allocate_exact_size(Vec2::splat(34.0), Sense::hover());
            ui.painter().rect_filled(logo, CornerRadius::same(9), theme::ACCENT_DIM);
            ui.painter().text(
                logo.center(),
                egui::Align2::CENTER_CENTER,
                icon::PUZZLE_PIECE,
                FontId::proportional(20.0),
                theme::ACCENT,
            );
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 0.0;
                ui.label(RichText::new("RO Plugins").font(FontId::new(16.0, theme::semibold())).color(theme::TEXT));
                ui.label(RichText::new("rodecomp plugin manager").size(11.5).color(theme::FAINT));
            });
            ui.add_space(12.0);

            // The client folder as a clickable path field.
            let text = if self.client.as_os_str().is_empty() {
                RichText::new(format!("{}  Choose a client folder…", icon::FOLDER_OPEN)).color(theme::AMBER)
            } else {
                RichText::new(format!("{}  {}", icon::FOLDER_OPEN, self.client.display())).color(theme::TEXT)
            };
            let path = egui::Button::new(text)
                .fill(theme::BG)
                .stroke(Stroke::new(1.0, theme::BORDER))
                .corner_radius(CornerRadius::same(8))
                .min_size(Vec2::new(300.0, 32.0))
                .truncate();
            if ui.add(path).on_hover_text("Client folder: the directory holding the game exe and plugins/. Click to change.").clicked() {
                self.actions.push(Action::PickClient);
            }

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.spacing_mut().item_spacing.x = 8.0;
                if ui.add(theme::ghost(icon::ARROWS_CLOCKWISE.to_owned())).on_hover_text("Reload from disk").clicked() {
                    self.actions.push(Action::Refresh);
                }
                if let Some(snapshot) = &self.snapshot {
                    let f = &snapshot.freshness;
                    let (label, color) = if !snapshot.status.exists {
                        ("No status".to_owned(), theme::GREY)
                    } else if f.is_stale() {
                        let why = if !f.changed.is_empty() { "files changed" } else { "game exited" };
                        (format!("Stale · {why}"), theme::AMBER)
                    } else {
                        ("Live".to_owned(), theme::GREEN)
                    };
                    let started = snapshot.status.header("started").unwrap_or("?");
                    theme::pill(ui, &label, color).on_hover_ui(|ui| {
                        ui.label(RichText::new("status.txt").strong());
                        for note in &f.notes {
                            ui.label(note);
                        }
                        if snapshot.status.exists {
                            ui.label(RichText::new(format!("run started {started}")).color(theme::MUTED));
                        }
                    });
                }
                let (label, color) = match self.game.running.first() {
                    Some(process) => (format!("Running · {} pid {}", process.name(), process.pid), theme::GREEN),
                    None => ("Not running".to_owned(), theme::GREY),
                };
                theme::pill(ui, &format!("{} {label}", icon::GAME_CONTROLLER), color).on_hover_text(
                    "The game counts as running when a process image is in the client folder: the exe named in \
                     status.txt or any *.exe. Changes are refused while it runs.",
                );
            });
        });
    }

    // ---- left: plugin list ----------------------------------------------------------------------------------

    fn plugin_list(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else {
            empty_state(ui, icon::FOLDER_OPEN, "No client folder", "Choose the folder that holds the game exe and its plugins/ directory.");
            if ui.add(theme::primary(format!("{}  Choose folder", icon::FOLDER_OPEN))).clicked() {
                self.actions.push(Action::PickClient);
            }
            return;
        };
        ui.horizontal(|ui| {
            ui.label(RichText::new("Plugins").font(FontId::new(15.0, theme::semibold())));
            theme::badge(ui, &snapshot.names.len().to_string(), theme::MUTED);
        });
        ui.add_space(4.0);
        let search = egui::TextEdit::singleline(&mut self.search)
            .hint_text(format!("{}  Search plugins", icon::MAGNIFYING_GLASS))
            .margin(Margin::symmetric(10, 7))
            .desired_width(f32::INFINITY);
        ui.add(search);
        ui.add_space(6.0);

        let needle = self.search.to_lowercase();
        let running = !self.game.running.is_empty();
        ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            let mut shown = 0;
            for name in &snapshot.names {
                if !needle.is_empty() && !name.to_lowercase().contains(&needle) {
                    continue;
                }
                shown += 1;
                let prediction = &snapshot.predicted[name];
                let version = snapshot.found.get(name).map(|m| m.version.to_string());
                let selected = self.selected.as_deref() == Some(name.as_str());
                if let Some(action) = plugin_row(ui, snapshot, name, version.as_deref(), prediction.state, selected, running) {
                    self.actions.push(action);
                }
            }
            if shown == 0 {
                ui.add_space(20.0);
                ui.vertical_centered(|ui| {
                    let text = if snapshot.names.is_empty() { "No plugins installed" } else { "No match" };
                    ui.label(RichText::new(text).color(theme::MUTED));
                });
            }
            for error in &snapshot.errors {
                ui.add_space(6.0);
                notice(ui, icon::WARNING, theme::RED, &format!("Invalid manifest: {error}"));
            }
        });
    }

    // ---- right: details ---------------------------------------------------------------------------------------

    fn details(&mut self, ui: &mut Ui) {
        let Some(snapshot) = &self.snapshot else { return };
        let Some(name) = self.selected.clone() else {
            empty_state(ui, icon::PUZZLE_PIECE, "Nothing selected", "Select a plugin on the left.");
            return;
        };
        let Some(prediction) = snapshot.predicted.get(&name) else { return };
        let manifest = snapshot.found.get(&name);
        let recorded = snapshot.status.plugins.get(&name);
        let running = self.running();

        // Header card.
        theme::card().show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.label(RichText::new(&name).font(FontId::new(22.0, theme::semibold())).color(theme::TEXT));
                if let Some(manifest) = manifest {
                    ui.label(RichText::new(format!("v{}", manifest.version)).size(15.0).color(theme::MUTED));
                }
                theme::badge(ui, prediction.state.as_str(), theme::state_color(prediction.state));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if manifest.is_some() {
                        let uninstall = ui.add_enabled(!running, theme::danger(format!("{}  Uninstall", icon::TRASH)));
                        if uninstall.on_disabled_hover_text("Close the game first").clicked() {
                            self.actions.push(Action::AskUninstall(name.clone()));
                        }
                    }
                    if ui.add(theme::ghost(format!("{}  Open folder", icon::FOLDER))).clicked() {
                        self.actions.push(Action::OpenFolder(snapshot.client.plugins.clone()));
                    }
                });
            });
            let description = self.descriptions.get(&name).map(|j| j.description.as_str()).filter(|d| !d.is_empty());
            ui.add_space(2.0);
            match description {
                Some(text) => ui.label(RichText::new(text).color(theme::MUTED)),
                None => ui.label(RichText::new("No description (Plugins/<Name>/plugin.json not found)").color(theme::FAINT).italics()),
            };
        });
        ui.add_space(10.0);

        // State card: recorded vs predicted.
        theme::card().show(ui, |ui| {
            ui.set_width(ui.available_width());
            theme::caption(ui, "State");
            ui.add_space(4.0);
            egui::Grid::new("state-grid").num_columns(3).spacing([14.0, 8.0]).show(ui, |ui| {
                ui.label(RichText::new("Predicted").color(theme::MUTED));
                theme::badge(ui, prediction.state.as_str(), theme::state_color(prediction.state));
                ui.label(if prediction.reason.is_empty() { "resolves; the host loads it at the next start".into() } else { prediction.reason.clone() });
                ui.end_row();
                ui.label(RichText::new("Recorded").color(theme::MUTED));
                match recorded {
                    Some(record) => {
                        theme::badge(ui, &record.state, theme::state_text_color(&record.state));
                        ui.label(if record.reason.is_empty() { "-".into() } else { record.reason.clone() });
                    }
                    None => {
                        theme::badge(ui, "-", theme::GREY);
                        ui.label(RichText::new("not in status.txt").color(theme::FAINT));
                    }
                }
                ui.end_row();
            });
            if recorded.is_some_and(|r| r.state != prediction.state.as_str()) {
                ui.add_space(6.0);
                notice(
                    ui,
                    icon::WARNING,
                    theme::AMBER,
                    "Recorded and predicted states differ: restart the game to apply the installed files and \
                     disabled.txt, or see RoPluginHost.log for load/init failures.",
                );
            }
        });
        ui.add_space(10.0);

        ui.columns(2, |columns| {
            // Dependencies.
            theme::card().show(&mut columns[0], |ui| {
                ui.set_width(ui.available_width());
                theme::caption(ui, "Dependencies");
                ui.add_space(4.0);
                let depends = manifest.map(|m| m.depends.as_slice()).unwrap_or_default();
                if depends.is_empty() {
                    ui.label(RichText::new("None").color(theme::FAINT));
                }
                for dependency in depends {
                    let target = snapshot.found.get(&dependency.name);
                    let ok = target.is_some_and(|t| dependency.range.satisfied_by(&t.version));
                    let resolved = recorded
                        .and_then(|r| r.resolved_pairs().into_iter().find(|(n, _)| *n == dependency.name).map(|(_, v)| v.to_owned()));
                    ui.horizontal(|ui| {
                        let (glyph, color) = if ok { (icon::CHECK_CIRCLE, theme::GREEN) } else { (icon::X_CIRCLE, theme::RED) };
                        ui.label(RichText::new(glyph).color(color).size(16.0));
                        if ui.link(RichText::new(&dependency.name).strong()).clicked() && target.is_some() {
                            self.actions.push(Action::Select(dependency.name.clone()));
                        }
                        ui.label(RichText::new(&dependency.text).monospace().color(theme::MUTED));
                    });
                    ui.horizontal(|ui| {
                        ui.add_space(26.0);
                        let installed = target.map_or("not installed".to_owned(), |t| format!("installed {}", t.version));
                        let text = match resolved {
                            Some(v) => format!("{installed} · host resolved {v}"),
                            None => installed,
                        };
                        ui.label(RichText::new(text).size(12.0).color(theme::FAINT));
                    });
                }
            });
            // Dependents and files.
            theme::card().show(&mut columns[1], |ui| {
                ui.set_width(ui.available_width());
                theme::caption(ui, "Dependents");
                ui.add_space(4.0);
                let users = snapshot.dependents(&name);
                if users.is_empty() {
                    ui.label(RichText::new("None").color(theme::FAINT));
                }
                ui.horizontal_wrapped(|ui| {
                    for user in &users {
                        let state = snapshot.predicted.get(user).map_or(State::Pending, |p| p.state);
                        let chip = egui::Button::new(RichText::new(user).color(theme::state_color(state)))
                            .fill(theme::state_color(state).gamma_multiply(0.10))
                            .stroke(Stroke::new(1.0, theme::state_color(state).gamma_multiply(0.3)))
                            .corner_radius(CornerRadius::same(255));
                        if ui.add(chip).clicked() {
                            self.actions.push(Action::Select(user.clone()));
                        }
                    }
                });
                ui.add_space(10.0);
                theme::caption(ui, "Files");
                ui.add_space(4.0);
                let files = crate::cli::plugin_files(&snapshot.client.plugins, &name);
                if files.is_empty() {
                    ui.label(RichText::new("None").color(theme::FAINT));
                }
                for file in files {
                    let size = std::fs::metadata(snapshot.client.plugins.join(&file)).map(|m| m.len()).unwrap_or(0);
                    ui.horizontal(|ui| {
                        ui.label(RichText::new(icon::FILE).color(theme::MUTED));
                        ui.label(&file);
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                            ui.label(RichText::new(human_size(size)).size(12.0).color(theme::FAINT));
                        });
                    });
                }
            });
        });
        if let Some(manifest) = manifest {
            ui.add_space(10.0);
            theme::card().show(ui, |ui| {
                ui.set_width(ui.available_width());
                theme::caption(ui, &format!("Manifest · {}", file_name(&manifest.path)));
                ui.add_space(4.0);
                egui::Frame::new().fill(theme::BG).corner_radius(CornerRadius::same(8)).inner_margin(Margin::same(10)).show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.label(RichText::new(manifest.text().trim_end()).monospace().color(theme::MUTED));
                });
            });
        }
    }

    fn right_pane(&mut self, ui: &mut Ui) {
        if self.snapshot.is_none() {
            return;
        }
        ui.horizontal(|ui| {
            segmented(ui, &mut self.view, &[(View::Details, icon::INFO, "Details"), (View::Graph, icon::TREE_STRUCTURE, "Graph")]);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let can_install = self.build_listing.as_ref().is_some_and(|b| !b.plugins.is_empty());
                let button = ui.add_enabled(can_install && !self.running(), theme::primary(format!("{}  Install from build", icon::DOWNLOAD_SIMPLE)));
                let button = if self.running() {
                    button.on_disabled_hover_text("Close the game first")
                } else {
                    button.on_disabled_hover_text(format!("No built plugins in {}", paths::build_plugins(&self.build).display()))
                };
                if button.clicked() {
                    self.actions.push(Action::OpenInstall);
                }
                let mismatches = self.snapshot.as_ref().map_or(0, Snapshot::mismatches);
                if mismatches > 0 {
                    theme::badge(ui, &format!("{} {mismatches} mismatch{}", icon::WARNING, if mismatches == 1 { "" } else { "es" }), theme::AMBER)
                        .on_hover_text("Recorded states differ from the prediction: restart the game to apply the files and disabled.txt.");
                }
            });
        });
        ui.add_space(8.0);
        ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| match self.view {
            View::Details => self.details(ui),
            View::Graph => {
                let snapshot = self.snapshot.as_ref().expect("checked above");
                theme::card().show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    theme::caption(ui, "Dependency graph");
                    ui.label(RichText::new("Dependencies on the left, dependents on the right; red edges are unsatisfied ranges.").size(12.0).color(theme::FAINT));
                    ui.add_space(10.0);
                    if snapshot.names.is_empty() {
                        ui.label(RichText::new("No plugins installed").color(theme::FAINT));
                    } else if let Some(name) = graph::show(ui, snapshot, self.selected.as_deref()) {
                        self.actions.push(Action::Select(name));
                    }
                    ui.add_space(10.0);
                    ui.horizontal_wrapped(|ui| {
                        for state in [State::Loaded, State::Off, State::Disabled, State::FailedLoad, State::Refused] {
                            theme::badge(ui, state.as_str(), theme::state_color(state));
                        }
                    });
                });
                if self.selected.is_some() {
                    ui.add_space(10.0);
                    self.details(ui);
                }
            }
        });
    }

    // ---- dialogs ----------------------------------------------------------------------------------------------

    fn install_dialog(&mut self, ctx: &egui::Context) {
        let Some(dialog) = &mut self.install else { return };
        let Some(listing) = &self.build_listing else {
            self.install = None;
            return;
        };
        let mut close = false;
        let mut install = None;
        let modal = egui::Modal::new(egui::Id::new("install")).frame(dialog_frame()).show(ctx, |ui| {
            ui.set_width(520.0);
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::DOWNLOAD_SIMPLE).size(20.0).color(theme::ACCENT));
                ui.label(RichText::new("Install from build").font(FontId::new(18.0, theme::semibold())));
            });
            ui.label(RichText::new(listing.dir.display().to_string()).size(12.0).color(theme::FAINT));
            ui.add_space(10.0);
            for available in &listing.plugins {
                let name = &available.manifest.name;
                let mut checked = dialog.checked.contains(name);
                let row = egui::Frame::new()
                    .fill(if checked { theme::ACCENT_DIM.gamma_multiply(0.6) } else { theme::BG })
                    .stroke(Stroke::new(1.0, if checked { theme::ACCENT.gamma_multiply(0.5) } else { theme::BORDER }))
                    .corner_radius(CornerRadius::same(9))
                    .inner_margin(Margin::symmetric(12, 9))
                    .show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        ui.horizontal(|ui| {
                            ui.checkbox(&mut checked, "");
                            ui.vertical(|ui| {
                                ui.spacing_mut().item_spacing.y = 2.0;
                                ui.label(RichText::new(name).strong());
                                let deps: Vec<String> = available.manifest.depends.iter().map(|d| format!("{} {}", d.name, d.text)).collect();
                                let deps = if deps.is_empty() { "no dependencies".to_owned() } else { format!("needs {}", deps.join(", ")) };
                                ui.label(RichText::new(deps).size(12.0).color(theme::FAINT));
                            });
                            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                let (text, color) = match available.change() {
                                    Change::New => ("new".to_owned(), theme::BLUE),
                                    Change::Upgrade => ("upgrade".to_owned(), theme::GREEN),
                                    Change::Downgrade => ("downgrade".to_owned(), theme::AMBER),
                                    Change::Same => ("reinstall".to_owned(), theme::GREY),
                                };
                                theme::badge(ui, &text, color);
                                let version = match &available.installed {
                                    Some(installed) if *installed != available.manifest.version => {
                                        format!("{installed}  {}  {}", icon::ARROW_RIGHT, available.manifest.version)
                                    }
                                    _ => available.manifest.version.to_string(),
                                };
                                ui.label(RichText::new(version).monospace().color(theme::MUTED));
                            });
                        });
                    });
                if row.response.interact(Sense::click()).clicked() {
                    checked = !checked;
                }
                if checked {
                    dialog.checked.insert(name.clone());
                } else {
                    dialog.checked.remove(name);
                }
                ui.add_space(4.0);
            }
            for error in &listing.errors {
                notice(ui, icon::WARNING, theme::RED, &format!("Invalid build manifest: {error}"));
            }

            // Preview the plan: auto-installed dependencies and incompatibilities.
            let wanted: Vec<String> = dialog.checked.iter().cloned().collect();
            let mut blocked = wanted.is_empty();
            if !wanted.is_empty() {
                if let Some(snapshot) = &self.snapshot {
                    match snapshot.client.plan_install(&listing.dir, &wanted) {
                        Ok(plan) => {
                            for note in plan.notes.iter().filter(|n| !n.starts_with("build: ")) {
                                notice(ui, icon::INFO, theme::BLUE, note);
                            }
                            for problem in &plan.problems {
                                notice(ui, icon::WARNING, if dialog.force { theme::AMBER } else { theme::RED }, &format!("Incompatible: {problem}"));
                            }
                            if !plan.problems.is_empty() {
                                ui.checkbox(&mut dialog.force, "Install anyway (--force)");
                                blocked = !dialog.force;
                            }
                        }
                        Err(refused) => {
                            notice(ui, icon::X_CIRCLE, theme::RED, &refused.message);
                            blocked = true;
                        }
                    }
                }
            }
            ui.add_space(10.0);
            ui.horizontal(|ui| {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    let label = format!("{}  Install {}", icon::DOWNLOAD_SIMPLE, wanted.len());
                    if ui.add_enabled(!blocked, theme::primary(label)).clicked() {
                        install = Some((wanted.clone(), dialog.force));
                    }
                    if ui.add(theme::ghost("Cancel")).clicked() {
                        close = true;
                    }
                });
            });
        });
        if let Some((names, force)) = install {
            self.actions.push(Action::Install(names, force));
            close = true;
        }
        if close || modal.should_close() {
            self.install = None;
        }
    }

    fn uninstall_dialog(&mut self, ctx: &egui::Context) {
        let Some(dialog) = &mut self.uninstall else { return };
        let mut close = false;
        let mut confirm = false;
        let modal = egui::Modal::new(egui::Id::new("uninstall")).frame(dialog_frame()).show(ctx, |ui| {
            ui.set_width(440.0);
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::TRASH).size(20.0).color(theme::RED));
                ui.label(RichText::new(format!("Uninstall {}?", dialog.name)).font(FontId::new(18.0, theme::semibold())));
            });
            ui.add_space(6.0);
            ui.label(RichText::new(format!("Removes {0}.dll, {0}.pdb and {0}.plugin from plugins/.", dialog.name)).color(theme::MUTED));
            if !dialog.dependents.is_empty() {
                ui.add_space(8.0);
                notice(
                    ui,
                    icon::WARNING,
                    theme::AMBER,
                    &format!("{} depend{} on {}.", dialog.dependents.join(", "), if dialog.dependents.len() == 1 { "s" } else { "" }, dialog.name),
                );
                ui.checkbox(&mut dialog.force, "Also uninstall the dependents (--force)");
                if dialog.force {
                    ui.add_space(4.0);
                    theme::caption(ui, "Will be removed");
                    for name in dialog.dependents.iter().rev().chain(std::iter::once(&dialog.name)) {
                        ui.label(format!("{}  {name}", icon::TRASH));
                    }
                }
            }
            ui.add_space(12.0);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let allowed = dialog.dependents.is_empty() || dialog.force;
                if ui.add_enabled(allowed, theme::danger(format!("{}  Uninstall", icon::TRASH))).clicked() {
                    confirm = true;
                }
                if ui.add(theme::ghost("Cancel")).clicked() {
                    close = true;
                }
            });
        });
        if confirm {
            self.actions.push(Action::Uninstall(dialog.name.clone(), dialog.force));
            close = true;
        }
        if close || modal.should_close() {
            self.uninstall = None;
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

        egui::Panel::top("top")
            .frame(egui::Frame::new().fill(theme::BG).inner_margin(Margin { left: 18, right: 18, top: 12, bottom: 12 }))
            .show_separator_line(false)
            .show(ui, |ui| self.top_bar(ui));
        egui::Panel::bottom("bottom")
            .frame(egui::Frame::new().fill(theme::BG).inner_margin(Margin::symmetric(18, 6)))
            .show_separator_line(false)
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    let build = paths::build_plugins(&self.build);
                    let text = if self.build.as_os_str().is_empty() {
                        format!("{}  Build: not set", icon::PACKAGE)
                    } else {
                        let count = self.build_listing.as_ref().map_or(0, |b| b.plugins.len());
                        format!("{}  Build: {} · {count} built", icon::PACKAGE, build.display())
                    };
                    let link = egui::Button::new(RichText::new(text).size(12.0).color(theme::MUTED)).frame(false);
                    if ui.add(link).on_hover_text("Click to choose the build output").clicked() {
                        self.actions.push(Action::PickBuild);
                    }
                    if let Some(error) = &self.files.error {
                        ui.label(RichText::new(format!("{}  {error}", icon::WARNING)).size(12.0).color(theme::AMBER));
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        ui.label(RichText::new(format!("ro-plugins {}", env!("CARGO_PKG_VERSION"))).size(12.0).color(theme::FAINT));
                    });
                });
            });
        egui::Panel::left("list")
            .frame(egui::Frame::new().fill(theme::PANEL).inner_margin(Margin { left: 14, right: 12, top: 14, bottom: 10 }))
            .resizable(true)
            .default_size(340.0)
            .size_range(260.0..=520.0)
            .show_separator_line(false)
            .show(ui, |ui| self.plugin_list(ui));
        egui::CentralPanel::no_frame()
            .frame(egui::Frame::new().fill(theme::BG).inner_margin(Margin { left: 18, right: 18, top: 14, bottom: 10 }))
            .show(ui, |ui| {
                if self.snapshot.is_none() {
                    ui.add_space(80.0);
                    empty_state(
                        ui,
                        icon::PLUGS_CONNECTED,
                        "Choose a client",
                        "Point RO Plugins at a client folder to see its installed plugins, what the host loaded and why.",
                    );
                    ui.vertical_centered(|ui| {
                        if ui.add(theme::primary(format!("{}  Choose client folder", icon::FOLDER_OPEN))).clicked() {
                            self.actions.push(Action::PickClient);
                        }
                    });
                } else {
                    self.right_pane(ui);
                }
            });

        self.install_dialog(&ctx);
        self.uninstall_dialog(&ctx);
        self.toasts.show(&ctx);

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

fn plugin_row(
    ui: &mut Ui,
    snapshot: &Snapshot,
    name: &str,
    version: Option<&str>,
    state: State,
    selected: bool,
    running: bool,
) -> Option<Action> {
    let mut action = None;
    let id = ui.id().with(("row", name));
    let hovered = ui.ctx().read_response(id).is_some_and(|r| r.hovered());
    let t = ui.ctx().animate_bool_with_time(id.with("hover"), hovered || selected, 0.12);
    let fill = if selected { theme::ACCENT_DIM.gamma_multiply(0.7) } else { theme::lerp_color(theme::PANEL, theme::CARD_HOVER, t) };
    let stroke = if selected { Stroke::new(1.0, theme::ACCENT.gamma_multiply(0.6)) } else { Stroke::new(1.0, theme::lerp_color(theme::PANEL, theme::BORDER, t)) };
    let frame = egui::Frame::new().fill(fill).stroke(stroke).corner_radius(CornerRadius::same(10)).inner_margin(Margin::symmetric(12, 10));
    let inner = frame.show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.horizontal(|ui| {
            let color = theme::state_color(state);
            let (dot, _) = ui.allocate_exact_size(Vec2::new(8.0, 30.0), Sense::hover());
            ui.painter().circle_filled(dot.center(), 4.0, color);
            ui.vertical(|ui| {
                ui.spacing_mut().item_spacing.y = 3.0;
                ui.horizontal(|ui| {
                    ui.label(RichText::new(name).font(FontId::new(14.0, theme::semibold())).color(theme::TEXT));
                    if let Some(version) = version {
                        ui.label(RichText::new(version).size(12.0).color(theme::MUTED));
                    }
                });
                theme::badge(ui, state.as_str(), color);
            });
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if snapshot.found.contains_key(name) {
                    let mut on = !snapshot.is_off(name);
                    let response = theme::toggle(ui, &mut on, !running);
                    let response = if running {
                        response.on_hover_text("The game is running from this client: close it to change plugins")
                    } else if on {
                        response.on_hover_text("Enabled: click to turn off in disabled.txt")
                    } else {
                        response.on_hover_text("Turned off in disabled.txt: click to turn on (with its dependencies)")
                    };
                    if response.changed() {
                        action = Some(Action::SetOff(name.to_owned(), !on));
                    }
                }
            });
        });
    });
    let response = ui.interact(inner.response.rect, id, Sense::click());
    if response.clicked() && action.is_none() {
        action = Some(Action::Select(name.to_owned()));
    }
    if response.hovered() {
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }
    ui.add_space(4.0);
    action
}

fn segmented(ui: &mut Ui, value: &mut View, items: &[(View, &str, &str)]) {
    egui::Frame::new().fill(theme::PANEL).stroke(Stroke::new(1.0, theme::BORDER)).corner_radius(CornerRadius::same(10)).inner_margin(Margin::same(3)).show(ui, |ui| {
        ui.spacing_mut().item_spacing.x = 2.0;
        ui.horizontal(|ui| {
            for (item, glyph, label) in items {
                let on = *value == *item;
                let text = RichText::new(format!("{glyph}  {label}")).color(if on { theme::TEXT } else { theme::MUTED });
                let button = egui::Button::new(text)
                    .fill(if on { theme::CARD_HOVER } else { Color32::TRANSPARENT })
                    .stroke(if on { Stroke::new(1.0, theme::BORDER) } else { Stroke::NONE })
                    .corner_radius(CornerRadius::same(8));
                if ui.add(button).clicked() {
                    *value = *item;
                }
            }
        });
    });
}

fn notice(ui: &mut Ui, glyph: &str, color: Color32, text: &str) {
    egui::Frame::new()
        .fill(color.gamma_multiply(0.08))
        .stroke(Stroke::new(1.0, color.gamma_multiply(0.3)))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(10, 8))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal_top(|ui| {
                ui.label(RichText::new(glyph).color(color));
                ui.add(egui::Label::new(RichText::new(text).color(theme::TEXT).size(13.0)).wrap());
            });
        });
}

fn empty_state(ui: &mut Ui, glyph: &str, title: &str, text: &str) {
    ui.vertical_centered(|ui| {
        ui.add_space(24.0);
        ui.label(RichText::new(glyph).size(40.0).color(theme::FAINT));
        ui.add_space(6.0);
        ui.label(RichText::new(title).font(FontId::new(16.0, theme::semibold())));
        ui.label(RichText::new(text).color(theme::MUTED));
        ui.add_space(12.0);
    });
}

fn dialog_frame() -> egui::Frame {
    egui::Frame::new()
        .fill(theme::CARD)
        .stroke(Stroke::new(1.0, theme::BORDER))
        .corner_radius(CornerRadius::same(14))
        .inner_margin(Margin::same(20))
        .shadow(egui::Shadow { offset: [0, 16], blur: 40, spread: 0, color: Color32::from_black_alpha(160) })
}

fn human_size(bytes: u64) -> String {
    match bytes {
        b if b >= 1 << 20 => format!("{:.1} MB", b as f64 / (1 << 20) as f64),
        b if b >= 1 << 10 => format!("{:.0} KB", b as f64 / 1024.0),
        b => format!("{b} B"),
    }
}

fn open_folder(path: &Path) -> std::io::Result<()> {
    let program = if cfg!(windows) {
        "explorer"
    } else if cfg!(target_os = "macos") {
        "open"
    } else {
        "xdg-open"
    };
    std::process::Command::new(program).arg(path).spawn().map(drop)
}
