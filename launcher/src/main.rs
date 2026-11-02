//! A port's launcher (Solstice, Duality, Tidefall: which, and its games, its
//! port.toml says). It opens on the port's games: one set up plays, one not
//! yet asks for the player's own dump and its update, checks them, and has
//! 3dsrecomp recompile the game on this computer, to run on the runtime's
//! player; nothing made from the game ships with the port. `--play [title]`
//! starts the game played last (or that one) straight away.
//!
//! The first build only takes the functions the game spends nearly all its
//! time in, from the list in `hot/` (addresses, no game code), so the game is
//! playable in about a minute. The rest is built in the background, by the
//! launcher run again with `--build-full <title>`, and is used from the next
//! start.

#![cfg_attr(not(test), windows_subsystem = "windows")]

mod check;
mod config;
mod compiler;
mod port;
mod recompile;
mod unpack;

use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use eframe::egui;

use check::{Game, Update};
use config::{Config, Library};
use recompile::{Job, Stage, Tools};

/// the value after an argument
fn after(name: &str) -> Option<String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let at = args.iter().position(|arg| arg == name)?;
    args.get(at + 1).filter(|value| !value.starts_with("--")).cloned()
}

fn main() -> eframe::Result {
    let port = match port::get() {
        Ok(port) => port,
        Err(error) => {
            // without its port.toml the launcher knows no game
            let error = error.to_owned();
            let options = eframe::NativeOptions {
                viewport: egui::ViewportBuilder::default().with_title("Launcher").with_inner_size([520.0, 200.0]),
                ..Default::default()
            };
            return eframe::run_native("Launcher", options, Box::new(move |_| Ok(Box::new(Missing(error)))));
        }
    };
    let library = Library::load();
    let tools = Tools::find(&library);
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.iter().any(|arg| arg == "--build-full") {
        if let (Some(tools), Some(title)) = (&tools, after("--build-full")) {
            build_full(tools, &title);
        }
        return Ok(());
    }
    // straight into the game, from a terminal or a shortcut: the launcher
    // otherwise always opens first
    if args.iter().any(|arg| arg == "--play") {
        let config = after("--play").and_then(|title| library.games.get(&title.to_uppercase()).cloned()).unwrap_or_else(|| library.current());
        if let (true, Some(tools)) = (config.ready(), &tools) {
            if play(tools, &config).is_ok() {
                return Ok(());
            }
        }
    }
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_title(&port.name).with_inner_size([640.0, 480.0]).with_resizable(false),
        ..Default::default()
    };
    eframe::run_native(&port.name, options, Box::new(move |_| Ok(Box::new(App::new(library, tools)))))
}

/// the list of the game's hottest functions, in a hot folder beside the
/// launcher, none for a game it has none for.
fn hot_list(game: Game) -> Option<PathBuf> {
    Some(port::folder("hot")?.join(format!("{}.txt", game.title()))).filter(|path| path.is_file())
}

/// builds all of a game, below normal priority, while it is played, and
/// notes it in the config once done. one at a time: a lock younger than a
/// day says another is running.
fn build_full(tools: &Tools, title: &str) {
    let title = title.to_uppercase();
    let Some(config) = Library::load().games.get(&title).cloned() else { return };
    let Some(lock) = config::lock(&title) else { return };
    let recent = std::fs::metadata(&lock)
        .and_then(|meta| meta.modified())
        .is_ok_and(|at| at.elapsed().is_ok_and(|age| age < Duration::from_secs(24 * 3600)));
    if recent || std::fs::write(&lock, std::process::id().to_string()).is_err() {
        return;
    }
    // a fast compile: the hot code optimized, the rest not
    let fast = Library::load().fast;
    let hot = crate::check::game(config.game.as_deref().unwrap_or(Path::new(""))).ok().and_then(hot_list);
    let built = recompile::command(tools, &config).is_ok_and(|mut command| {
        if let (true, Some(hot)) = (fast, &hot) {
            command.arg("--hot").arg(hot).arg("--cold-fast");
        }
        // Zig is there already, downloaded for the first build
        if compiler::prepare(&mut command, &|_, _| {}, &recompile::Running::default()).is_err() {
            return false;
        }
        recompile::hide_console(&mut command);
        command.status().is_ok_and(|status| status.success())
    });
    // the player may have chosen other files, or the port's code was built
    // again, meanwhile
    let mut library = Library::load();
    if let Some(now) = library.games.get_mut(&title) {
        if built && now.game == config.game && now.update == config.update && now.decomp_built == config.decomp_built {
            now.full = true;
            let _ = library.save();
        }
    }
    let _ = std::fs::remove_file(lock);
}

/// starts building the rest of the game in the background, when only its
/// hottest functions are built.
fn finish_in_background(config: &Config) {
    if config.full {
        return;
    }
    if let Ok(exe) = std::env::current_exe() {
        let mut command = Command::new(exe);
        if let Some(file) = after("--port") {
            command.arg("--port").arg(file);
        }
        let _ = command.arg("--build-full").arg(&config.title).spawn();
    }
}

/// starts the game in the runtime's player, which carries on once the
/// launcher closes, and the rest of its build behind it.
fn play(tools: &Tools, config: &Config) -> Result<(), String> {
    let settings = config::player_settings(config)?;
    let mut library = Library::load();
    library.put(config.clone());
    let _ = library.save();
    finish_in_background(config);
    let mut command = Command::new(&tools.player);
    command.arg("--config").arg(settings);
    command.spawn().map(drop).map_err(|error| format!("could not start {}, {error}", tools.player.display()))
}

/// a shortcut to the launcher on the desktop, named after the port, made
/// through PowerShell, which every Windows the launcher runs on has.
fn desktop_shortcut() -> Result<(), String> {
    let exe = std::env::current_exe().map_err(|error| error.to_string())?;
    let quote = |path: &Path| path.display().to_string().replace('\'', "''");
    let dir = exe.parent().map(quote).unwrap_or_default();
    let name = port::port().name.replace('\'', "''");
    let script = format!(
        "$s = (New-Object -ComObject WScript.Shell).CreateShortcut([Environment]::GetFolderPath('Desktop') + '\\{name}.lnk'); \
         $s.TargetPath = '{}'; $s.WorkingDirectory = '{dir}'; $s.Save()",
        quote(&exe)
    );
    let status = Command::new("powershell")
        .args(["-NoProfile", "-NonInteractive", "-Command", &script])
        .status()
        .map_err(|error| format!("could not run PowerShell, {error}"))?;
    if status.success() { Ok(()) } else { Err("PowerShell could not make the shortcut".to_owned()) }
}

/// the window a launcher without its port.toml shows: why
struct Missing(String);

impl eframe::App for Missing {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ui, |ui| {
            ui.heading("This launcher has no port.toml");
            ui.label(&self.0);
            ui.label("Keep port.toml from the download beside the launcher.");
        });
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum Page {
    Home,
    Game,
    Update,
    Build,
}

struct App {
    page: Page,
    library: Library,
    tools: Option<Tools>,
    // the game being set up
    game_path: Option<PathBuf>,
    game: Option<Result<Game, String>>,
    update_path: Option<PathBuf>,
    update: Option<Result<Update, String>>,
    /// the port's code folder the config named for the game, kept as it was
    chosen_decomp: Option<PathBuf>,
    job: Option<Job>,
    /// what the last action said.
    note: Option<String>,
}

impl App {
    fn new(library: Library, tools: Option<Tools>) -> App {
        // with a game set up, the games; the first time, its questions
        let page = if library.ready().is_empty() { Page::Game } else { Page::Home };
        App {
            page,
            library,
            tools,
            game_path: None,
            game: None,
            update_path: None,
            update: None,
            chosen_decomp: None,
            job: None,
            note: None,
        }
    }

    /// the questions again, for a game set up or about to be
    fn set_up(&mut self, config: Option<&Config>) {
        self.game_path = config.and_then(|config| config.game.clone());
        self.game = self.game_path.as_deref().map(check::game);
        self.update_path = config.and_then(|config| config.update.clone());
        self.update = match (&self.update_path, &self.game) {
            (Some(path), Some(Ok(game))) => Some(check::update(path, *game)),
            _ => None,
        };
        self.chosen_decomp = config.and_then(|config| config.decomp.clone());
        self.job = None;
        self.note = None;
        self.page = Page::Game;
    }

    fn valid_game(&self) -> Option<Game> {
        self.game.as_ref().and_then(|game| game.as_ref().ok().copied())
    }

    /// the update to use, none when it was skipped or isn't valid.
    fn valid_update(&self) -> Option<&Path> {
        self.update.as_ref().filter(|update| update.is_ok()).and(self.update_path.as_deref())
    }

    /// the game being set up, as a config: built, or not yet
    fn config(&self, built: bool) -> Config {
        let game = self.valid_game();
        let mut config = Config {
            title: game.map(Game::title).unwrap_or_default(),
            game: self.game_path.clone(),
            update: self.valid_update().map(Path::to_owned),
            built,
            full: built && game.and_then(hot_list).is_none(),
            decomp: self.chosen_decomp.clone(),
            decomp_built: None,
        };
        if built {
            config.decomp_built = config.decomp_dir().as_deref().and_then(config::fingerprint);
        }
        config
    }

    fn home_page(&mut self, ui: &mut egui::Ui) {
        let port = port::port();
        ui.heading(&port.name);
        if !port.about.is_empty() {
            ui.label(&port.about);
        }
        ui.add_space(12.0);
        let mut chosen = None;
        let mut set_up = None;
        for info in &port.games {
            let config = self.library.games.get(&info.title.to_uppercase()).cloned();
            let ready = config.as_ref().is_some_and(Config::ready);
            egui::Frame::group(ui.style()).show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.horizontal(|ui| {
                    ui.vertical(|ui| {
                        ui.label(egui::RichText::new(&info.name).strong().size(18.0));
                        ui.label(egui::RichText::new(if ready {
                            "Ready to play"
                        } else if config.is_some() {
                            "Its files moved, or its build is out of date: set it up again"
                        } else {
                            "Not set up"
                        })
                        .weak());
                    });
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if ready {
                            if ui.add(egui::Button::new(egui::RichText::new("Play").strong().size(16.0)).min_size([90.0, 32.0].into())).clicked() {
                                chosen = config.clone();
                            }
                            if ui.button("Change files").clicked() {
                                set_up = Some(config.clone());
                            }
                        } else if ui.button("Set up").clicked() {
                            set_up = Some(config.clone());
                        }
                    });
                });
            });
            ui.add_space(6.0);
        }
        if let Some(config) = chosen {
            match self.tools.as_ref().ok_or("3dsrecomp or the game's player is missing".to_owned()).and_then(|tools| play(tools, &config)) {
                Ok(()) => ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close),
                Err(error) => self.note = Some(error),
            }
        }
        if let Some(config) = set_up {
            self.set_up(config.as_ref());
        }
        ui.add_space(8.0);
        self.fast_option(ui);
        if ui.button("Add a desktop shortcut").clicked() {
            self.note = Some(match desktop_shortcut() {
                Ok(()) => "The shortcut is on the desktop.".to_owned(),
                Err(error) => error,
            });
        }
        if let Some(note) = &self.note {
            ui.label(note);
        }
    }

    /// the fast compile's switch, kept at once
    fn fast_option(&mut self, ui: &mut egui::Ui) {
        if ui
            .checkbox(&mut self.library.fast, "Fast compile (for modest processors)")
            .on_hover_text(
                "The game still plays after about a minute. What follows in the background compiles about three \
                 times faster, the code the game nearly never runs left unoptimized; the code it runs, and what it \
                 was seen to run, stays optimized.",
            )
            .changed()
        {
            self.note = self.library.save().err();
        }
    }

    fn game_page(&mut self, ui: &mut egui::Ui) {
        let port = port::port();
        ui.heading("Your game");
        ui.label(format!(
            "{} runs your own copy of {}, recompiled for this computer. Pick your decrypted dump: .3ds, .cci, .cxi or .cia.",
            port.name,
            port.games.iter().map(|game| game.name.as_str()).collect::<Vec<_>>().join(" or ")
        ));
        ui.add_space(12.0);
        if ui.button("Choose the game…").clicked() {
            if let Some(path) = rfd::FileDialog::new().add_filter("3DS game", &["3ds", "cci", "cxi", "cia"]).pick_file() {
                self.game = Some(check::game(&path));
                self.game_path = Some(path);
                // an update chosen for another game no longer fits; the
                // game's own set up before comes back
                let known = self.valid_game().and_then(|game| self.library.games.get(&game.title()).cloned());
                self.update_path = known.as_ref().and_then(|config| config.update.clone());
                self.update = match (&self.update_path, self.valid_game()) {
                    (Some(path), Some(game)) => Some(check::update(path, game)),
                    _ => None,
                };
                self.chosen_decomp = known.and_then(|config| config.decomp);
            }
        }
        if let Some(path) = &self.game_path {
            ui.label(egui::RichText::new(path.display().to_string()).weak());
        }
        match &self.game {
            Some(Ok(game)) => {
                ui.colored_label(ui.visuals().widgets.active.fg_stroke.color, format!("✔ {}", game.name));
            }
            Some(Err(error)) => {
                ui.colored_label(ui.visuals().error_fg_color, error);
            }
            None => {}
        }
        ui.add_space(16.0);
        ui.horizontal(|ui| {
            if !self.library.ready().is_empty() && ui.button("Back").clicked() {
                self.page = Page::Home;
            }
            if ui.add_enabled(self.valid_game().is_some(), egui::Button::new("Next")).clicked() {
                self.page = Page::Update;
            }
        });
    }

    fn update_page(&mut self, ui: &mut egui::Ui) {
        let Some(game) = self.valid_game() else {
            self.page = Page::Game;
            return;
        };
        let port = port::port();
        ui.heading("Its update");
        ui.label(match game.tested_name {
            Some(tested) => format!("Pick the decrypted update for {} (.cia). {} is tested with {tested}.", game.name, port.name),
            None => format!("Pick the decrypted update for {} (.cia).", game.name),
        });
        ui.add_space(12.0);
        if ui.button("Choose the update…").clicked() {
            if let Some(path) = rfd::FileDialog::new().add_filter("3DS update", &["cia"]).pick_file() {
                self.update = Some(check::update(&path, game));
                self.update_path = Some(path);
            }
        }
        if let Some(path) = &self.update_path {
            ui.label(egui::RichText::new(path.display().to_string()).weak());
        }
        match &self.update {
            Some(Ok(update)) if update.tested(game) => {
                ui.colored_label(ui.visuals().widgets.active.fg_stroke.color, format!("✔ {}", update.name(game)));
            }
            Some(Ok(update)) => {
                ui.colored_label(
                    ui.visuals().warn_fg_color,
                    format!(
                        "This is {}. It should work, but {} is tested with {}.",
                        update.name(game),
                        port.name,
                        game.tested_name.unwrap_or("another")
                    ),
                );
            }
            Some(Err(error)) => {
                ui.colored_label(ui.visuals().error_fg_color, error);
            }
            None => {
                ui.colored_label(ui.visuals().warn_fg_color, "Without the update the game runs as it came on the cartridge.");
            }
        }
        ui.add_space(16.0);
        ui.horizontal(|ui| {
            if ui.button("Back").clicked() {
                self.page = Page::Game;
            }
            let label = if self.valid_update().is_some() { "Next" } else { "Continue without it" };
            // an invalid update has to be replaced or cleared first
            let usable = !matches!(self.update, Some(Err(_)));
            if ui.add_enabled(usable, egui::Button::new(label)).clicked() {
                self.page = Page::Build;
            }
            if matches!(self.update, Some(Err(_))) && ui.button("Clear").clicked() {
                self.update = None;
                self.update_path = None;
            }
        });
    }

    fn build_page(&mut self, ui: &mut egui::Ui) {
        let port = port::port();
        let hot = self.valid_game().and_then(hot_list);
        ui.heading("Recompiling");
        ui.label(if hot.is_some() {
            format!(
                "{} now turns the game's code into code for this computer, starting with the parts the game \
                 spends nearly all its time in: about a minute, longer on slower computers. The rest is built in the \
                 background while you play. If there is no C compiler here, one (Zig) is downloaded first.",
                port.name
            )
        } else {
            format!(
                "{} now turns the game's code into code for this computer. It takes around ten to fifteen \
                 minutes and only happens once. If there is no C compiler here, one (Zig) is downloaded first.",
                port.name
            )
        });
        ui.add_space(8.0);
        self.fast_option(ui);
        ui.add_space(8.0);
        let Some(tools) = self.tools.clone() else {
            ui.colored_label(ui.visuals().error_fg_color, format!("{} can't find 3dsrecomp and {}. It looked in:", port.name, port.player));
            for place in recompile::places("3dsrecomp").into_iter().chain(recompile::game_places()) {
                ui.label(egui::RichText::new(place.display().to_string()).weak());
            }
            ui.label("Keep the 3dsrecomp and game folders from the download beside the launcher.");
            ui.add_space(8.0);
            if ui.button("Back").clicked() {
                self.page = Page::Update;
            }
            return;
        };
        match &mut self.job {
            None => {
                ui.horizontal(|ui| {
                    if ui.button("Back").clicked() {
                        self.page = Page::Update;
                    }
                    if ui.button("Start").clicked() {
                        let config = self.config(false);
                        self.job = Some(Job::start(&tools, &config, hot.as_deref()).unwrap_or_else(|error| {
                            // a job that failed to start shows its error like one that failed later
                            Job::failed(error)
                        }));
                    }
                });
            }
            Some(job) => {
                let stage = job.stage();
                ui.add(egui::ProgressBar::new(stage.fraction()).show_percentage());
                ui.label(stage.describe());
                ui.add_space(12.0);
                match stage {
                    Stage::Done => {
                        let fast = self.library.fast;
                        self.library = Library::load();
                        self.library.fast = fast;
                        self.library.put(self.config(true));
                        self.note = self.library.save().err();
                        self.job = None;
                        self.page = Page::Home;
                    }
                    Stage::Failed(_) => {
                        if ui.button("Try again").clicked() {
                            self.job = None;
                        }
                    }
                    _ => {
                        if ui.button("Cancel").clicked() {
                            job.cancel();
                        }
                        ui.ctx().request_repaint_after(Duration::from_millis(250));
                    }
                }
            }
        }
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ui, |ui| {
            ui.add_space(8.0);
            if self.page != Page::Home {
                let steps = ["Game", "Update", "Recompile"];
                let current = self.page as usize - 1;
                ui.horizontal(|ui| {
                    for (index, step) in steps.iter().enumerate() {
                        let text = egui::RichText::new(*step);
                        ui.label(if index == current { text.strong() } else { text.weak() });
                        if index + 1 < steps.len() {
                            ui.label(egui::RichText::new("›").weak());
                        }
                    }
                });
                ui.separator();
                ui.add_space(8.0);
            }
            match self.page {
                Page::Home => self.home_page(ui),
                Page::Game => self.game_page(ui),
                Page::Update => self.update_page(ui),
                Page::Build => self.build_page(ui),
            }
        });
    }
}
