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
mod look;
mod port;
mod recompile;
mod settings;
mod unpack;

use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use eframe::egui::{self, RichText};

use check::{Game, Update};
use config::{Config, Library};
use recompile::{Job, Stage, Tools};
use settings::PlayerSettings;

/// wgpu, on Direct3D 12 on Windows: NVIDIA's OpenGL and Vulkan driver has
/// been seen to crash presenting the launcher's window (WGPU_BACKEND, when
/// set, still chooses)
const RENDERER: eframe::Renderer = eframe::Renderer::Wgpu;

fn prefer_direct3d() {
    if cfg!(windows) && std::env::var_os("WGPU_BACKEND").is_none() {
        std::env::set_var("WGPU_BACKEND", "dx12");
    }
}

/// the value after an argument
fn after(name: &str) -> Option<String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let at = args.iter().position(|arg| arg == name)?;
    args.get(at + 1).filter(|value| !value.starts_with("--")).cloned()
}

fn main() -> eframe::Result {
    prefer_direct3d();
    let port = match port::get() {
        Ok(port) => port,
        Err(error) => {
            // without its port.toml the launcher knows no game
            let error = error.to_owned();
            let options = eframe::NativeOptions {
                viewport: egui::ViewportBuilder::default().with_title("Launcher").with_inner_size([520.0, 200.0]),
                renderer: RENDERER,
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
        viewport: egui::ViewportBuilder::default().with_title(&port.name).with_inner_size([1180.0, 720.0]).with_min_inner_size([960.0, 600.0]),
        renderer: RENDERER,
        ..Default::default()
    };
    eframe::run_native(
        &port.name,
        options,
        Box::new(move |creation| {
            look::install(&creation.egui_ctx);
            Ok(Box::new(App::new(library, tools)))
        }),
    )
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

/// a line of the home page, from the right, so high
fn line(ui: &mut egui::Ui, height: f32, add: impl FnOnce(&mut egui::Ui)) {
    ui.allocate_ui_with_layout(egui::vec2(ui.available_width(), height), egui::Layout::right_to_left(egui::Align::Center), add);
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

/// the bar along the bottom
#[derive(Debug, Clone, Copy, PartialEq)]
enum Tab {
    Home,
    Graphics,
    Controls,
    Advanced,
}

struct App {
    tab: Tab,
    page: Page,
    /// the game the home page shows, by title id
    selected: String,
    library: Library,
    tools: Option<Tools>,
    settings: PlayerSettings,
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
        let port = port::port();
        let selected = library.last.clone().or_else(|| port.games.first().map(|game| game.title.to_uppercase())).unwrap_or_default();
        let mut app = App {
            tab: Tab::Home,
            page: Page::Home,
            selected,
            library,
            tools,
            settings: PlayerSettings::load(),
            game_path: None,
            game: None,
            update_path: None,
            update: None,
            chosen_decomp: None,
            job: None,
            note: None,
        };
        // the first time, its questions
        if app.library.ready().is_empty() {
            app.set_up(None);
        }
        app
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
        self.tab = Tab::Home;
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

    /// the color of what is on screen: the game being set up, else the one
    /// chosen on the home page
    fn accent(&self) -> egui::Color32 {
        match (self.page, self.valid_game()) {
            (Page::Home, _) | (_, None) => look::accent(&self.selected),
            (_, Some(game)) => look::accent(&game.title()),
        }
    }

    /// the home page: the port's name over the glow, the game chosen, Play
    fn home(&mut self, ui: &mut egui::Ui) {
        let port = port::port();
        let accent = self.accent();
        let config = self.library.games.get(&self.selected).cloned();
        let ready = config.as_ref().is_some_and(Config::ready);
        let info = port.games.iter().find(|game| game.title.eq_ignore_ascii_case(&self.selected));

        ui.with_layout(egui::Layout::bottom_up(egui::Align::Max), |ui| {
            ui.add_space(26.0);
            ui.label(RichText::new("In the game: F1 opens the player's menu, F11 switches fullscreen.").size(12.5).color(look::WEAK));
            ui.label(RichText::new(self.settings.summary()).color(look::TEXT));
            ui.add_space(2.0);
            line(ui, 30.0, |ui| {
                {
                    match &config {
                        Some(config) if config.built => {
                            let building = config::lock(&config.title).is_some_and(|lock| lock.is_file());
                            if config.full {
                                look::chip(ui, "✔ Fully built", look::GOOD);
                            } else if building {
                                look::chip(ui, "• The rest building in the background", look::WARN);
                            } else {
                                look::chip(ui, "• The rest builds while you play", look::WARN);
                            }
                            match (&config.update, info.and_then(|info| info.update_name.as_deref())) {
                                (Some(_), Some(name)) => look::chip(ui, &format!("✔ Update {name}"), look::GOOD),
                                (Some(_), None) => look::chip(ui, "✔ Update", look::GOOD),
                                (None, _) => look::chip(ui, "No update", look::WARN),
                            }
                            if !ready {
                                look::chip(ui, "✖ Its files moved, or the port's code changed", look::BAD);
                            }
                        }
                        _ => look::chip(ui, "• Not set up yet", look::WEAK),
                    }
                    if config.as_ref().and_then(|config| config.decomp_dir()).is_some() {
                        look::chip(ui, "✔ The port's own code", look::GOOD);
                    }
                }
            });
            ui.add_space(6.0);
            line(ui, 64.0, |ui| {
                {
                    if ready {
                        if look::big_button(ui, "PLAY", accent).clicked() {
                            let config = config.clone().unwrap_or_default();
                            match self.tools.as_ref().ok_or("3dsrecomp or the game's player is missing".to_owned()).and_then(|tools| play(tools, &config)) {
                                Ok(()) => ui.ctx().send_viewport_cmd(egui::ViewportCommand::Close),
                                Err(error) => self.note = Some(error),
                            }
                        }
                        if ui.button("Change files").clicked() {
                            self.set_up(config.as_ref());
                        }
                    } else if look::big_button(ui, "SET UP", accent).clicked() {
                        self.set_up(config.as_ref());
                    }
                }
            });
            if let Some(note) = &self.note {
                ui.colored_label(look::BAD, note);
            }
            ui.add_space(8.0);
            // the games of the port, the chosen one in its color
            line(ui, 36.0, |ui| {
                {
                    for game in port.games.iter().rev() {
                        let title = game.title.to_uppercase();
                        let chosen = title == self.selected;
                        let color = look::accent(&title);
                        let text = RichText::new(&game.name).size(16.0).color(if chosen { egui::Color32::WHITE } else { look::WEAK });
                        let button = egui::Button::new(text)
                            .fill(if chosen { color.gamma_multiply(0.85) } else { look::INK.gamma_multiply(0.6) })
                            .stroke(egui::Stroke::new(1.0, if chosen { color } else { look::LINE }))
                            .corner_radius(16.0);
                        if ui.add(button).clicked() {
                            self.selected = title;
                            self.note = None;
                        }
                    }
                }
            });
            ui.add_space(4.0);
            ui.label(RichText::new(port.name.to_uppercase()).font(look::title_font(76.0)).color(look::TEXT));
        });
        // the port's name small, top left
        let corner = ui.max_rect().left_top() + egui::vec2(28.0, 22.0);
        ui.painter().text(corner, egui::Align2::LEFT_TOP, &port.name, look::title_font(22.0), accent);
    }

    /// the fast compile's switch, kept at once
    fn fast_option(&mut self, ui: &mut egui::Ui) {
        let mut fast = self.library.fast;
        look::row(
            ui,
            "Fast compile",
            "For modest processors. The game still plays after about a minute; what is built in the background \
             then takes about a third of the time, the code the game nearly never runs left unoptimized.",
            |ui| {
                if ui.checkbox(&mut fast, "").changed() {
                    self.library.fast = fast;
                    self.note = self.library.save().err();
                }
            },
        );
    }

    fn advanced(&mut self, ui: &mut egui::Ui) {
        let port = port::port();
        let accent = self.accent();
        look::page_title(ui, "Advanced", "Building the games, recording, and where things are kept.");
        look::section(ui, "Building", accent);
        self.fast_option(ui);
        let config = self.library.games.get(&self.selected).cloned();
        let name = port.games.iter().find(|game| game.title.eq_ignore_ascii_case(&self.selected)).map_or("the game", |game| game.name.as_str());
        if let Some(config) = config.filter(|config| config.built) {
            if !config.full {
                look::row(ui, &format!("Build all of {name} now"), "Rather than while you play: takes a while, the computer stays usable.", |ui| {
                    if ui.button("Build").clicked() {
                        finish_in_background(&config);
                        self.note = Some("Building in the background.".to_owned());
                    }
                });
            }
            look::row(ui, &format!("{name}'s files"), "Another dump or update, and the game recompiled for them.", |ui| {
                if ui.button("Change").clicked() {
                    self.set_up(Some(&config));
                }
            });
        }

        look::section(ui, "Recording", accent);
        let mut record = self.settings.flag("game", "record", false);
        look::row(
            ui,
            "Record my input",
            "Each start records what you press, with a copy of the saves as they were, into recordings\\ in the \
             port's folder: for reporting a problem the way it happened.",
            |ui| {
                if ui.checkbox(&mut record, "").changed() {
                    self.settings.set_flag("game", "record", record);
                }
            },
        );

        look::section(ui, "Files", accent);
        let folder = config::dir();
        look::row(
            ui,
            "The port's folder",
            "Your saves (user\\), the games as built, the settings. A new version of the port, unzipped over this \
             one or anywhere else, keeps them.",
            |ui| {
                if ui.button("Open").clicked() {
                    if let Some(folder) = &folder {
                        let _ = std::fs::create_dir_all(folder);
                        let _ = Command::new("explorer").arg(folder).spawn();
                    }
                }
            },
        );
        look::row(ui, "Desktop shortcut", "", |ui| {
            if ui.button("Add").clicked() {
                self.note = Some(match desktop_shortcut() {
                    Ok(()) => "The shortcut is on the desktop.".to_owned(),
                    Err(error) => error,
                });
            }
        });
        if let Some(note) = &self.note {
            ui.label(RichText::new(note).color(look::WEAK));
        }

        look::section(ui, "About", accent);
        if !port.about.is_empty() {
            ui.label(&port.about);
        }
        ui.label(RichText::new(format!("{} {} · {}", port.name, env!("CARGO_PKG_VERSION"), std::env::consts::OS)).color(look::WEAK));
        ui.add_space(20.0);
    }

    fn steps(&self, ui: &mut egui::Ui) {
        let steps = ["Game", "Update", "Recompile"];
        let current = self.page as usize - 1;
        ui.add_space(18.0);
        ui.horizontal(|ui| {
            for (index, step) in steps.iter().enumerate() {
                let text = RichText::new(format!("{} {step}", index + 1)).size(14.0);
                ui.label(if index == current { text.color(self.accent()) } else { text.color(look::WEAK) });
                if index + 1 < steps.len() {
                    ui.label(RichText::new("›").color(look::WEAK));
                }
            }
        });
    }

    fn game_page(&mut self, ui: &mut egui::Ui) {
        let port = port::port();
        look::page_title(
            ui,
            "Your game",
            &format!(
                "{} runs your own copy of {}, recompiled for this computer. Pick your decrypted dump: .3ds, .cci, .cxi or .cia.",
                port.name,
                port.games.iter().map(|game| game.name.as_str()).collect::<Vec<_>>().join(" or ")
            ),
        );
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
                if let Some(game) = self.valid_game() {
                    self.selected = game.title();
                }
            }
        }
        if let Some(path) = &self.game_path {
            ui.label(RichText::new(path.display().to_string()).color(look::WEAK));
        }
        match &self.game {
            Some(Ok(game)) => {
                ui.colored_label(look::GOOD, format!("✔ {}", game.name));
            }
            Some(Err(error)) => {
                ui.colored_label(look::BAD, error);
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
        look::page_title(
            ui,
            "Its update",
            &match game.tested_name {
                Some(tested) => format!("Pick the decrypted update for {} (.cia). {} is tested with {tested}.", game.name, port.name),
                None => format!("Pick the decrypted update for {} (.cia).", game.name),
            },
        );
        ui.add_space(12.0);
        if ui.button("Choose the update…").clicked() {
            if let Some(path) = rfd::FileDialog::new().add_filter("3DS update", &["cia"]).pick_file() {
                self.update = Some(check::update(&path, game));
                self.update_path = Some(path);
            }
        }
        if let Some(path) = &self.update_path {
            ui.label(RichText::new(path.display().to_string()).color(look::WEAK));
        }
        match &self.update {
            Some(Ok(update)) if update.tested(game) => {
                ui.colored_label(look::GOOD, format!("✔ {}", update.name(game)));
            }
            Some(Ok(update)) => {
                ui.colored_label(
                    look::WARN,
                    format!(
                        "This is {}. It should work, but {} is tested with {}.",
                        update.name(game),
                        port.name,
                        game.tested_name.unwrap_or("another")
                    ),
                );
            }
            Some(Err(error)) => {
                ui.colored_label(look::BAD, error);
            }
            None => {
                ui.colored_label(look::WARN, "Without the update the game runs as it came on the cartridge.");
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
        look::page_title(
            ui,
            "Recompiling",
            &if hot.is_some() {
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
            },
        );
        ui.add_space(8.0);
        self.fast_option(ui);
        ui.add_space(8.0);
        let Some(tools) = self.tools.clone() else {
            ui.colored_label(look::BAD, format!("{} can't find 3dsrecomp and {}. It looked in:", port.name, port.player));
            for place in recompile::places("3dsrecomp").into_iter().chain(recompile::game_places()) {
                ui.label(RichText::new(place.display().to_string()).color(look::WEAK));
            }
            ui.label("Keep the 3dsrecomp and game folders from the download beside the launcher.");
            ui.add_space(8.0);
            if ui.button("Back").clicked() {
                self.page = Page::Update;
            }
            return;
        };
        let accent = self.accent();
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
                ui.add(egui::ProgressBar::new(stage.fraction()).show_percentage().fill(accent));
                ui.label(stage.describe());
                ui.add_space(12.0);
                match stage {
                    Stage::Done => {
                        let fast = self.library.fast;
                        self.library = Library::load();
                        self.library.fast = fast;
                        let config = self.config(true);
                        self.selected = config.title.clone();
                        self.library.put(config);
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

    fn tabs(&mut self, ui: &mut egui::Ui) {
        let accent = self.accent();
        ui.horizontal(|ui| {
            ui.add_space(12.0);
            for (tab, name) in [(Tab::Home, "Home"), (Tab::Graphics, "Graphics"), (Tab::Controls, "Controls"), (Tab::Advanced, "Advanced")] {
                let chosen = self.tab == tab;
                let text = RichText::new(name).font(look::title_font(17.0)).color(if chosen { accent } else { look::WEAK });
                let response = ui.add(egui::Button::new(text).frame(false).min_size(egui::vec2(96.0, 46.0)));
                if chosen {
                    let rect = response.rect;
                    ui.painter().rect_filled(egui::Rect::from_min_max(egui::pos2(rect.left(), rect.bottom() - 3.0), rect.right_bottom()), 0.0, accent);
                }
                if response.clicked() && !chosen {
                    self.tab = tab;
                    // the player may have changed them in the game
                    self.settings = PlayerSettings::load();
                    if self.page != Page::Home && self.job.is_none() && tab != Tab::Home {
                        self.page = Page::Home;
                    }
                }
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.add_space(14.0);
                ui.label(RichText::new(format!("{} · v{}", port::port().name, env!("CARGO_PKG_VERSION"))).size(12.5).color(look::WEAK));
            });
        });
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        let accent = self.accent();
        let partner = look::partner(&self.selected);
        look::background(ui.painter(), ui.max_rect(), accent, partner);

        egui::Panel::bottom("tabs")
            .frame(egui::Frame::NONE.fill(look::INK.gamma_multiply(0.92)).stroke(egui::Stroke::new(1.0, accent.gamma_multiply(0.7))))
            .show(ui, |ui| self.tabs(ui));

        let paged = self.page != Page::Home || self.tab != Tab::Home;
        if paged {
            let width = (ui.available_width() * 0.5).clamp(480.0, 640.0);
            egui::Panel::left("page")
                .exact_size(width)
                .resizable(false)
                .frame(egui::Frame::NONE.fill(look::PANEL.gamma_multiply(0.97)).inner_margin(egui::Margin::symmetric(28, 0)).stroke(egui::Stroke::new(1.0, accent.gamma_multiply(0.6))))
                .show(ui, |ui| {
                    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
                        if self.page != Page::Home {
                            self.steps(ui);
                            match self.page {
                                Page::Game => self.game_page(ui),
                                Page::Update => self.update_page(ui),
                                Page::Build => self.build_page(ui),
                                Page::Home => {}
                            }
                        } else {
                            match self.tab {
                                Tab::Graphics => self.settings.graphics_page(ui, accent),
                                Tab::Controls => self.settings.controls_page(ui, accent),
                                Tab::Advanced => self.advanced(ui),
                                Tab::Home => {}
                            }
                        }
                    });
                });
        }
        egui::CentralPanel::default()
            .frame(egui::Frame::NONE.inner_margin(egui::Margin { left: 28, right: 36, top: 0, bottom: 0 }))
            .show(ui, |ui| {
                if paged {
                    // the port's name and the game's, over the glow
                    ui.with_layout(egui::Layout::bottom_up(egui::Align::Max), |ui| {
                        ui.add_space(30.0);
                        if let Some(game) = port::port().games.iter().find(|game| game.title.eq_ignore_ascii_case(&self.selected)) {
                            ui.label(RichText::new(&game.name).size(18.0).color(accent));
                        }
                        ui.label(RichText::new(port::port().name.to_uppercase()).font(look::title_font(56.0)).color(look::TEXT));
                    });
                } else {
                    self.home(ui);
                }
            });
    }
}
