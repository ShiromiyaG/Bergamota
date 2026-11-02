//! the player's settings, from the launcher: player.toml in the port's folder,
//! the same file the player's F1 menu changes (its [game] table is the
//! launcher's, written before each start). The Graphics and Controls pages
//! save as soon as something changes; what is not set keeps the player's own
//! default (frontend/settings.h).

use std::path::PathBuf;

use eframe::egui::{self, Color32, RichText};

use crate::config;
use crate::look::{self, row, section};

/// the 3DS's buttons as the settings name them, and as players know them
pub const BUTTONS: [(&str, &str); 16] = [
    ("a", "A"),
    ("b", "B"),
    ("x", "X"),
    ("y", "Y"),
    ("l", "L"),
    ("r", "R"),
    ("start", "START"),
    ("select", "SELECT"),
    ("up", "D-Pad up"),
    ("down", "D-Pad down"),
    ("left", "D-Pad left"),
    ("right", "D-Pad right"),
    ("circle_up", "Circle Pad up"),
    ("circle_down", "Circle Pad down"),
    ("circle_left", "Circle Pad left"),
    ("circle_right", "Circle Pad right"),
];

/// the player's keys when the file names none (SDL's scancode names)
const DEFAULT_KEYS: [(&str, &str); 16] = [
    ("a", "X"),
    ("b", "Z"),
    ("x", "S"),
    ("y", "A"),
    ("l", "Q"),
    ("r", "W"),
    ("start", "Return"),
    ("select", "Backspace"),
    ("up", "Up"),
    ("down", "Down"),
    ("left", "Left"),
    ("right", "Right"),
    ("circle_up", "I"),
    ("circle_down", "K"),
    ("circle_left", "J"),
    ("circle_right", "L"),
];

/// and its gamepad buttons (SDL's names), by position: the 3DS's A is on
/// the right, B at the bottom; the Circle Pad is the left stick
const DEFAULT_PAD: [(&str, &str); 12] = [
    ("a", "b"),
    ("b", "a"),
    ("x", "y"),
    ("y", "x"),
    ("l", "leftshoulder"),
    ("r", "rightshoulder"),
    ("start", "start"),
    ("select", "back"),
    ("up", "dpup"),
    ("down", "dpdown"),
    ("left", "dpleft"),
    ("right", "dpright"),
];

/// the gamepad's buttons a 3DS button can be, and their names
const PAD_BUTTONS: [(&str, &str); 16] = [
    ("", "None"),
    ("a", "South (A, Cross)"),
    ("b", "East (B, Circle)"),
    ("x", "West (X, Square)"),
    ("y", "North (Y, Triangle)"),
    ("leftshoulder", "Left bumper (LB, L1)"),
    ("rightshoulder", "Right bumper (RB, R1)"),
    ("start", "Start, Menu, Options"),
    ("back", "Back, View, Share"),
    ("guide", "Guide, Home, PS"),
    ("leftstick", "Left stick press (L3)"),
    ("rightstick", "Right stick press (R3)"),
    ("dpup", "D-Pad up"),
    ("dpdown", "D-Pad down"),
    ("dpleft", "D-Pad left"),
    ("dpright", "D-Pad right"),
];

#[derive(Default)]
pub struct PlayerSettings {
    table: toml::Table,
    /// the key being chosen: a 3DS button's name
    capture: Option<&'static str>,
    /// the Controls page on the gamepad's
    pad: bool,
    pub note: Option<String>,
}

fn path() -> Option<PathBuf> {
    Some(config::dir()?.join("player.toml"))
}

impl PlayerSettings {
    pub fn load() -> PlayerSettings {
        let table = path().and_then(|path| std::fs::read_to_string(path).ok()).and_then(|text| text.parse().ok()).unwrap_or_default();
        PlayerSettings { table, ..PlayerSettings::default() }
    }

    fn value(&self, section: &str, key: &str) -> Option<&toml::Value> {
        self.table.get(section)?.as_table()?.get(key)
    }

    pub fn flag(&self, section: &str, key: &str, default: bool) -> bool {
        self.value(section, key).and_then(toml::Value::as_bool).unwrap_or(default)
    }

    pub fn text(&self, section: &str, key: &str, default: &str) -> String {
        self.value(section, key).and_then(toml::Value::as_str).unwrap_or(default).to_owned()
    }

    fn number(&self, section: &str, key: &str, default: f64) -> f64 {
        match self.value(section, key) {
            Some(toml::Value::Float(value)) => *value,
            Some(toml::Value::Integer(value)) => *value as f64,
            _ => default,
        }
    }

    /// a setting changed, and the file saved with it
    fn set(&mut self, section: &str, key: &str, value: toml::Value) {
        // the file as it is now, the player may have changed it since
        let mut fresh = PlayerSettings::load();
        if fresh.table.is_empty() {
            fresh.table = std::mem::take(&mut self.table);
        }
        let entry = fresh.table.entry(section).or_insert_with(|| toml::Value::Table(toml::Table::new()));
        if let Some(table) = entry.as_table_mut() {
            if value.as_str() == Some("") {
                table.remove(key);
            } else {
                table.insert(key.to_owned(), value);
            }
        }
        self.table = fresh.table;
        self.note = self.save().err();
    }

    pub fn set_flag(&mut self, section: &str, key: &str, on: bool) {
        self.set(section, key, toml::Value::Boolean(on));
    }

    fn save(&self) -> Result<(), String> {
        let path = path().ok_or("there is no APPDATA folder")?;
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|error| error.to_string())?;
        }
        let text = toml::to_string(&self.table).map_err(|error| error.to_string())?;
        std::fs::write(&path, text).map_err(|error| format!("could not write {}, {error}", path.display()))
    }

    fn check(&mut self, ui: &mut egui::Ui, section: &str, key: &str, default: bool) {
        let mut on = self.flag(section, key, default);
        if ui.checkbox(&mut on, "").changed() {
            self.set(section, key, toml::Value::Boolean(on));
        }
    }

    fn choice(&mut self, ui: &mut egui::Ui, section: &str, key: &str, default: &str, options: &[(&str, &str)]) {
        let now = self.text(section, key, default);
        let shown = options.iter().find(|(value, _)| *value == now).map_or(now.as_str(), |(_, name)| *name).to_owned();
        let mut chosen = None;
        egui::ComboBox::from_id_salt((section, key)).width(230.0).selected_text(shown).show_ui(ui, |ui| {
            for (value, name) in options {
                if ui.selectable_label(*value == now, *name).clicked() {
                    chosen = Some(value.to_string());
                }
            }
        });
        if let Some(value) = chosen {
            self.set(section, key, toml::Value::String(value));
        }
    }

    fn slider(&mut self, ui: &mut egui::Ui, section: &str, key: &str, default: f64, range: std::ops::RangeInclusive<f64>, integer: bool) {
        let mut value = self.number(section, key, default);
        let slider = egui::Slider::new(&mut value, range).fixed_decimals(if integer { 0 } else { 2 });
        let response = ui.add(slider);
        if response.drag_stopped() || (response.changed() && !response.dragged()) {
            self.set(section, key, if integer { toml::Value::Integer(value.round() as i64) } else { toml::Value::Float(value) });
        } else if response.changed() {
            // kept while dragging, saved once let go
            if let Some(table) = self.table.entry(section).or_insert_with(|| toml::Value::Table(toml::Table::new())).as_table_mut() {
                table.insert(key.to_owned(), if integer { toml::Value::Integer(value.round() as i64) } else { toml::Value::Float(value) });
            }
        }
    }

    /// a short line of what the game will look like: for the home page
    pub fn summary(&self) -> String {
        let layout = match self.text("video", "layout", "both").as_str() {
            "single" => "Single screen",
            "top" => "Top screen only",
            _ => "Both screens",
        };
        let resolution = match self.text("video", "resolution", "native").as_str() {
            "native" => "Console resolution".to_owned(),
            "window" => "Window resolution".to_owned(),
            other => other.replace('x', " × "),
        };
        let fps = if self.flag("video", "between", false) { "60 FPS" } else { "Original frame rate" };
        let window = if self.flag("video", "fullscreen", false) { "Fullscreen" } else { "Windowed" };
        format!("{resolution} · {layout} · {fps} · {window}")
    }

    pub fn graphics_page(&mut self, ui: &mut egui::Ui, accent: Color32) {
        look::page_title(ui, "Graphics", "Saved for the next start. In the game, F1 opens the same settings.");
        section(ui, "Window", accent);
        row(ui, "Fullscreen", "Covers the screen; F11 switches in the game.", |ui| self.check(ui, "video", "fullscreen", false));
        row(ui, "VSync", "Waits for the display: no tearing, a little more latency.", |ui| self.check(ui, "video", "vsync", false));

        section(ui, "Screens", accent);
        row(ui, "Layout", "Single screen: the top screen, the bottom one as a panel (Tab, L3). L2 swaps them.", |ui| {
            self.choice(ui, "video", "layout", "both", &[("single", "Single screen"), ("both", "Both screens, stacked"), ("top", "Top screen only")])
        });
        if self.text("video", "layout", "both") == "single" {
            row(ui, "Panel size", "The bottom screen's panel, against the top screen's height.", |ui| {
                self.slider(ui, "video", "panel_size", 0.35, 0.2..=0.6, false)
            });
            row(ui, "Panel corner", "", |ui| {
                self.choice(
                    ui,
                    "video",
                    "panel_corner",
                    "bottom-right",
                    &[("bottom-right", "Bottom right"), ("bottom-left", "Bottom left"), ("top-right", "Top right"), ("top-left", "Top left")],
                )
            });
            row(ui, "Panel out after cutscenes", "Shows the bottom screen once a movie shown on both is past.", |ui| {
                self.check(ui, "video", "panel_shown", true)
            });
        }

        section(ui, "Picture", accent);
        row(ui, "Resolution", "What the 3D is drawn at; the screens scale to the window.", |ui| {
            self.choice(
                ui,
                "video",
                "resolution",
                "native",
                &[
                    ("native", "Console (400 × 240)"),
                    ("window", "The window's"),
                    ("1280x720", "1280 × 720"),
                    ("1920x1080", "1920 × 1080"),
                    ("2560x1440", "2560 × 1440"),
                    ("3840x2160", "3840 × 2160"),
                ],
            )
        });
        row(ui, "Aspect ratio", "Wider than the console's where the port's code frames a wider scene.", |ui| {
            self.choice(ui, "video", "aspect", "original", &[("original", "Original (5:3)"), ("16:9", "16:9"), ("21:9", "21:9")])
        });
        row(ui, "Outlines", "Auto: the game's at the console's resolution, thin above it.", |ui| {
            self.choice(ui, "video", "outlines", "auto", &[("auto", "Auto"), ("original", "Original"), ("thin", "Thin"), ("none", "None")])
        });
        row(ui, "Smooth scaling", "Off: sharp pixels.", |ui| self.check(ui, "video", "smooth", true));
        row(ui, "Anti-aliasing (FXAA)", "", |ui| self.check(ui, "video", "antialias", false));
        row(ui, "Sharpen", "", |ui| self.slider(ui, "video", "sharpen", 0.0, 0.0..=1.0, false));
        row(ui, "No bloom ghosting", "Leaves out the trail the bloom draws behind what moves.", |ui| {
            self.check(ui, "video", "no_ghosting", true)
        });

        section(ui, "Motion", accent);
        row(ui, "60 FPS", "A frame drawn between each two of the game's, for the scenes it draws at 30.", |ui| {
            self.check(ui, "video", "between", false)
        });
        row(ui, "Up to the display's rate", "More frames between on 120 and 144 Hz displays.", |ui| {
            self.check(ui, "video", "between_display", false)
        });

        section(ui, "Sound", accent);
        row(ui, "Volume", "", |ui| self.slider(ui, "audio", "volume", 100.0, 0.0..=100.0, true));
        row(ui, "Mute", "", |ui| self.check(ui, "audio", "mute", false));
        row(ui, "Frame times", "A corner of the screen with how long each frame takes.", |ui| self.check(ui, "overlay", "stats", false));
        if let Some(note) = &self.note {
            ui.colored_label(look::BAD, note);
        }
        ui.add_space(20.0);
    }

    fn key(&self, button: &str) -> String {
        let default = DEFAULT_KEYS.iter().find(|(name, _)| *name == button).map_or("", |(_, key)| *key);
        self.text("keyboard", button, default)
    }

    fn pad_button(&self, button: &str) -> String {
        let default = DEFAULT_PAD.iter().find(|(name, _)| *name == button).map_or("", |(_, pad)| *pad);
        self.text("gamepad", button, default)
    }

    /// the whole of a binding table written out, so that clearing one keeps the
    /// others' defaults
    fn bind(&mut self, section: &str, button: &str, value: &str) {
        let mut table = toml::Table::new();
        for (name, _) in BUTTONS {
            let now = if section == "keyboard" { self.key(name) } else { self.pad_button(name) };
            let now = if name == button { value.to_owned() } else { now };
            table.insert(name.to_owned(), toml::Value::String(now));
        }
        let mut fresh = PlayerSettings::load();
        fresh.table.insert(section.to_owned(), toml::Value::Table(table));
        self.table = fresh.table;
        self.note = self.save().err();
    }

    fn reset(&mut self, section: &str) {
        let mut fresh = PlayerSettings::load();
        fresh.table.remove(section);
        self.table = fresh.table;
        self.note = self.save().err();
    }

    pub fn controls_page(&mut self, ui: &mut egui::Ui, accent: Color32) {
        look::page_title(ui, "Controls", "Which key or gamepad button presses each of the 3DS's buttons.");
        ui.horizontal(|ui| {
            ui.label("Device");
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.selectable_label(self.pad, "Gamepad").clicked() {
                    self.pad = true;
                    self.capture = None;
                }
                if ui.selectable_label(!self.pad, "Keyboard").clicked() {
                    self.pad = false;
                }
            });
        });
        ui.label(
            RichText::new(
                "On a gamepad the left stick is the Circle Pad. The right stick moves a pointer over the bottom screen, \
                 R2 or R3 touches, L2 swaps the screens.",
            )
            .size(12.5)
            .color(look::WEAK),
        );

        if self.pad {
            section(ui, "Gamepad", accent);
            for (button, name) in &BUTTONS[..12] {
                let now = self.pad_button(button);
                let mut chosen = None;
                row(ui, name, "", |ui| {
                    let shown = PAD_BUTTONS.iter().find(|(value, _)| *value == now).map_or(now.as_str(), |(_, label)| *label).to_owned();
                    egui::ComboBox::from_id_salt(("pad", *button)).width(230.0).selected_text(shown).show_ui(ui, |ui| {
                        for (value, label) in PAD_BUTTONS {
                            if ui.selectable_label(value == now, label).clicked() {
                                chosen = Some(value);
                            }
                        }
                    });
                });
                if let Some(value) = chosen {
                    self.bind("gamepad", button, value);
                }
            }
            ui.add_space(8.0);
            if ui.button("Gamepad defaults").clicked() {
                self.reset("gamepad");
            }
        } else {
            section(ui, "Keyboard", accent);
            // the key pressed while one is being chosen; Escape gives up
            if let Some(button) = self.capture {
                let pressed = ui.input(|input| {
                    input.events.iter().find_map(|event| match event {
                        egui::Event::Key { key, pressed: true, modifiers, .. } => Some(sdl_key(*key, *modifiers)),
                        _ => None,
                    })
                });
                let modifier = ui.input(|input| {
                    let m = input.modifiers;
                    if m.shift { Some("Left Shift") } else if m.ctrl { Some("Left Ctrl") } else if m.alt { Some("Left Alt") } else { None }
                });
                match (pressed, modifier) {
                    (Some(Some(name)), _) if name == "Escape" => self.capture = None,
                    (Some(Some(name)), _) => {
                        self.capture = None;
                        self.bind("keyboard", button, &name);
                    }
                    (_, Some(name)) => {
                        self.capture = None;
                        self.bind("keyboard", button, name);
                    }
                    _ => {}
                }
            }
            for (button, name) in BUTTONS {
                let now = self.key(button);
                let choosing = self.capture == Some(button);
                let mut clicked = false;
                let mut cleared = false;
                row(ui, name, "", |ui| {
                    if ui.button("Clear").clicked() {
                        cleared = true;
                    }
                    let label = if choosing { RichText::new("Press a key…").color(accent) } else { RichText::new(if now.is_empty() { "—" } else { now.as_str() }) };
                    if ui.add_sized([170.0, 28.0], egui::Button::new(label).selected(choosing)).clicked() {
                        clicked = true;
                    }
                });
                if clicked {
                    self.capture = if choosing { None } else { Some(button) };
                }
                if cleared {
                    self.bind("keyboard", button, "");
                }
            }
            ui.add_space(8.0);
            if ui.button("Keyboard defaults").clicked() {
                self.capture = None;
                self.reset("keyboard");
            }
        }
        if let Some(note) = &self.note {
            ui.colored_label(look::BAD, note);
        }
        ui.add_space(20.0);
    }
}

/// a key as SDL names its scancode, which the player reads; none for a key
/// it has no name for
fn sdl_key(key: egui::Key, _modifiers: egui::Modifiers) -> Option<String> {
    use egui::Key;
    let name = match key {
        Key::Enter => "Return",
        Key::Escape => "Escape",
        Key::Backspace => "Backspace",
        Key::Tab => "Tab",
        Key::Space => "Space",
        Key::ArrowUp => "Up",
        Key::ArrowDown => "Down",
        Key::ArrowLeft => "Left",
        Key::ArrowRight => "Right",
        Key::Insert => "Insert",
        Key::Delete => "Delete",
        Key::Home => "Home",
        Key::End => "End",
        Key::PageUp => "PageUp",
        Key::PageDown => "PageDown",
        Key::Minus => "-",
        Key::Equals => "=",
        Key::OpenBracket => "[",
        Key::CloseBracket => "]",
        Key::Semicolon => ";",
        Key::Quote => "'",
        Key::Comma => ",",
        Key::Period => ".",
        Key::Slash => "/",
        Key::Backslash => "\\",
        Key::Backtick => "`",
        _ => {
            // letters, digits and F keys are named alike
            let name = key.name();
            let simple = (name.len() == 1 && name.chars().all(|c| c.is_ascii_alphanumeric()))
                || (name.starts_with('F') && name[1..].parse::<u32>().is_ok_and(|n| (1..=12).contains(&n)));
            return simple.then(|| name.to_uppercase());
        }
    };
    Some(name.to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keys_have_sdl_names() {
        let none = egui::Modifiers::NONE;
        assert_eq!(sdl_key(egui::Key::A, none).as_deref(), Some("A"));
        assert_eq!(sdl_key(egui::Key::Num1, none).as_deref(), Some("1"));
        assert_eq!(sdl_key(egui::Key::Enter, none).as_deref(), Some("Return"));
        assert_eq!(sdl_key(egui::Key::F5, none).as_deref(), Some("F5"));
        assert_eq!(sdl_key(egui::Key::ArrowLeft, none).as_deref(), Some("Left"));
    }

    #[test]
    fn the_defaults_are_the_players() {
        let settings = PlayerSettings::default();
        assert_eq!(settings.key("a"), "X");
        assert_eq!(settings.key("circle_left"), "J");
        assert_eq!(settings.pad_button("a"), "b");
        assert_eq!(settings.pad_button("circle_up"), "");
        assert!(settings.summary().contains("Both screens"));
    }
}
