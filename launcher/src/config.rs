//! what the player chose, kept in %APPDATA%\<port>\config.toml so that later
//! launches go straight to the game: for each of the port's games they set
//! up, its dump, its update and how far it is built (each in a folder of its
//! own, by title id), the one played last, and the tools when they had to
//! point at them.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::port;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Library {
    /// the title id of the game played last
    pub last: Option<String>,
    /// 3dsrecomp and the runtime's player, when the player had to point at
    /// them
    pub recomp: Option<PathBuf>,
    pub player: Option<PathBuf>,
    /// each game set up, by title id
    pub games: BTreeMap<String, Config>,
}

/// one game, as set up
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(default)]
pub struct Config {
    /// its title id (16 hex digits), empty before a game is chosen
    pub title: String,
    /// the player's own dump of the game.
    pub game: Option<PathBuf>,
    /// its update, none to play without one.
    pub update: Option<PathBuf>,
    /// the game was recompiled with that game and update, at least its
    /// hottest functions, enough to play.
    pub built: bool,
    /// all of it was, not only the hottest functions.
    pub full: bool,
    /// the port's code for the game's ARM11 (decomp.bin and decomp.txt),
    /// when not the folder the port's file names for the game.
    pub decomp: Option<PathBuf>,
    /// the fingerprint of the decomp the game was recompiled with, none
    /// without one.
    pub decomp_built: Option<u64>,
}

/// a config from before the port's file: one game, at the top
#[derive(Deserialize)]
struct Before {
    game: Option<PathBuf>,
    update: Option<PathBuf>,
    #[serde(default)]
    built: bool,
    #[serde(default)]
    full: bool,
    recomp: Option<PathBuf>,
    player: Option<PathBuf>,
    decomp: Option<PathBuf>,
    decomp_built: Option<u64>,
}

impl Library {
    pub fn load() -> Library {
        let Some(text) = path().and_then(|path| std::fs::read_to_string(path).ok()) else { return Library::default() };
        if let Ok(library) = toml::from_str::<Library>(&text) {
            if !library.games.is_empty() || library.last.is_some() {
                return library;
            }
        }
        // the one game of a launcher from before, moved into its folder
        match toml::from_str::<Before>(&text) {
            Ok(before) => {
                let mut library = Library { recomp: before.recomp, player: before.player, ..Library::default() };
                let title = before.game.as_deref().and_then(|game| crate::check::game(game).ok()).map(|game| game.title());
                if let Some(title) = title {
                    for name in ["build", "update"] {
                        if let (Some(old), Some(new)) = (dir().map(|dir| dir.join(name)), game_dir(&title).map(|dir| dir.join(name))) {
                            if old.is_dir() && !new.exists() {
                                let _ = std::fs::create_dir_all(new.parent().unwrap_or(&new));
                                let _ = std::fs::rename(old, new);
                            }
                        }
                    }
                    let config = Config {
                        title: title.clone(),
                        game: before.game,
                        update: before.update,
                        built: before.built,
                        full: before.full,
                        decomp: before.decomp,
                        decomp_built: before.decomp_built,
                    };
                    library.games.insert(title.clone(), config);
                    library.last = Some(title);
                }
                library
            }
            Err(_) => Library::default(),
        }
    }

    pub fn save(&self) -> Result<(), String> {
        let path = path().ok_or("there is no APPDATA folder to keep the settings in")?;
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|error| format!("could not create {}, {error}", dir.display()))?;
        }
        let text = toml::to_string(self).map_err(|error| error.to_string())?;
        std::fs::write(&path, text).map_err(|error| format!("could not write {}, {error}", path.display()))
    }

    /// the game played last, else the first set up, else none chosen yet
    pub fn current(&self) -> Config {
        self.last.as_ref().and_then(|title| self.games.get(title)).or_else(|| self.games.values().next()).cloned().unwrap_or_default()
    }

    /// a game set up (or set up again), now the one played last
    pub fn put(&mut self, config: Config) {
        if config.title.is_empty() {
            return;
        }
        self.last = Some(config.title.clone());
        self.games.insert(config.title.clone(), config);
    }

    /// the games ready to play, in the port's order
    pub fn ready(&self) -> Vec<Config> {
        let order = |config: &Config| port::get().ok().and_then(|port| port.games.iter().position(|game| game.title.eq_ignore_ascii_case(&config.title)));
        let mut ready: Vec<Config> = self.games.values().filter(|config| config.ready()).cloned().collect();
        ready.sort_by_key(|config| order(config).unwrap_or(usize::MAX));
        ready
    }
}

impl Config {
    /// everything is in place to play: a recompiled game whose files are
    /// still where they were, recompiled with the port's code there is now,
    /// if any.
    pub fn ready(&self) -> bool {
        self.built
            && self.game.as_ref().is_some_and(|game| game.is_file())
            && self.update.as_ref().is_none_or(|update| update.is_file())
            && self.decomp_built == self.decomp_dir().as_deref().and_then(fingerprint)
    }

    /// the port's code the game is recompiled and played with: the folder
    /// chosen, else the one the port's file names for the game, when it has
    /// one built.
    pub fn decomp_dir(&self) -> Option<PathBuf> {
        let named = || {
            let port = port::get().ok()?;
            let info = port.games.iter().find(|game| game.title.eq_ignore_ascii_case(&self.title))?;
            port::folder(info.decomp.as_deref()?)
        };
        self.decomp.clone().or_else(named).filter(|dir| dir.join("decomp.txt").is_file() && dir.join("decomp.bin").is_file())
    }
}

/// 64-bit FNV-1a over a decomp's decomp.txt and decomp.bin: another build of
/// it, another fingerprint, and the game has to be recompiled.
pub fn fingerprint(dir: &Path) -> Option<u64> {
    let mut hash = 0xCBF2_9CE4_8422_2325u64;
    for name in ["decomp.txt", "decomp.bin"] {
        for byte in std::fs::read(dir.join(name)).ok()? {
            hash = (hash ^ u64::from(byte)).wrapping_mul(0x0000_0100_0000_01B3);
        }
    }
    Some(hash)
}

/// the port's folder in %APPDATA%.
pub fn dir() -> Option<PathBuf> {
    let dir = std::env::var_os("APPDATA").filter(|dir| !dir.is_empty())?;
    Some(PathBuf::from(dir).join(&port::get().ok()?.name))
}

fn path() -> Option<PathBuf> {
    Some(dir()?.join("config.toml"))
}

/// a game's own folder, by its title id
pub fn game_dir(title: &str) -> Option<PathBuf> {
    if title.is_empty() {
        return None;
    }
    Some(dir()?.join(title.to_uppercase()))
}

/// there while a full build of the game runs in the background, so that only
/// one does.
pub fn lock(title: &str) -> Option<PathBuf> {
    Some(game_dir(title)?.join("building.lock"))
}

/// where 3dsrecomp builds the game: the library and its C.
pub fn build_dir(title: &str) -> Option<PathBuf> {
    Some(game_dir(title)?.join("build"))
}

/// the update laid out for 3dsrecomp's --mods.
pub fn update_dir(title: &str) -> Option<PathBuf> {
    Some(game_dir(title)?.join("update"))
}

/// the saves and extra data of all the port's games, laid out as Zakuro lays
/// them (each game's by its title id), so Zakuro's saves work when copied in.
pub fn user_dir() -> Option<PathBuf> {
    Some(dir()?.join("user"))
}

/// the player's settings file: its [game] table written from this config
/// before each start, everything else (video, sound, controls) the player's,
/// the same for all the port's games.
pub fn player_settings(config: &Config) -> Result<PathBuf, String> {
    let path = dir().ok_or("there is no APPDATA folder")?.join("player.toml");
    let mut table: toml::Table = std::fs::read_to_string(&path).ok().and_then(|text| text.parse().ok()).unwrap_or_default();
    let game = config.game.as_ref().ok_or("no game was chosen")?;
    let library = std::fs::read_dir(build_dir(&config.title).ok_or("no build folder")?)
        .map_err(|error| format!("the game was not built: {error}"))?
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .find(|path| path.extension().is_some_and(|ext| ext == std::env::consts::DLL_EXTENSION))
        .ok_or("the game was not built: no library")?;
    let text = |path: &Path| toml::Value::String(path.display().to_string());
    // the whole table anew: the player reads the program (exheader, code)
    // from the dumps itself; what else it had (the UI's font and art, the
    // single screen's scenes) kept
    let mut section = table.get("game").and_then(|game| game.as_table()).cloned().unwrap_or_default();
    for key in ["game", "library", "user", "update", "decomp", "exheader", "code"] {
        section.remove(key);
    }
    section.insert("game".into(), text(game));
    section.insert("library".into(), text(&library));
    section.insert("user".into(), text(&user_dir().ok_or("no user folder")?));
    if let Some(update) = &config.update {
        section.insert("update".into(), text(update));
    }
    if let Some(decomp) = config.decomp_dir() {
        section.insert("decomp".into(), text(&decomp));
    }
    table.insert("game".into(), toml::Value::Table(section));
    std::fs::create_dir_all(user_dir().ok_or("no user folder")?).map_err(|error| error.to_string())?;
    std::fs::write(&path, toml::to_string(&table).map_err(|error| error.to_string())?)
        .map_err(|error| format!("could not write {}, {error}", path.display()))?;
    Ok(path)
}

#[cfg(test)]
mod tests {
    /// a game's config round trips, the decomp's fields too.
    #[test]
    fn the_decomp_round_trips() {
        let config = super::Config {
            title: "00040000001B5000".into(),
            decomp: Some("K:/decomp".into()),
            decomp_built: Some(0x1234_5678_9ABC_DEF0),
            built: true,
            ..Default::default()
        };
        let mut library = super::Library::default();
        library.put(config.clone());
        let back: super::Library = toml::from_str(&toml::to_string(&library).unwrap()).unwrap();
        assert_eq!(back.current(), config);
        assert_eq!(back.last.as_deref(), Some("00040000001B5000"));
    }

    /// the fingerprint follows both files.
    #[test]
    fn the_fingerprint_follows_the_build() {
        let dir = std::env::temp_dir().join(format!("launcher-decomp-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        assert_eq!(super::fingerprint(&dir), None);
        std::fs::write(dir.join("decomp.txt"), "base 0x07800000\n").unwrap();
        std::fs::write(dir.join("decomp.bin"), [1, 2, 3, 4]).unwrap();
        let first = super::fingerprint(&dir).unwrap();
        std::fs::write(dir.join("decomp.bin"), [1, 2, 3, 5]).unwrap();
        assert_ne!(super::fingerprint(&dir), Some(first));
        let config = super::Config { decomp: Some(dir.clone()), ..Default::default() };
        assert_eq!(config.decomp_dir(), Some(dir.clone()));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
