//! whether the files the player picks are one of the port's games and its
//! update, said in words a player can act on.

use std::path::Path;

use crate::port::{self, Port};

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Game {
    pub program_id: u64,
    pub name: &'static str,
    /// the update the port is tested with (the TMD's version), and its name
    pub tested: Option<u16>,
    pub tested_name: Option<&'static str>,
}

impl Game {
    /// the title id as the files name it
    pub fn title(self) -> String {
        format!("{:016X}", self.program_id)
    }

    fn of(port: &'static Port, program_id: u64) -> Option<Game> {
        let info = port.game(program_id)?;
        Some(Game { program_id, name: &info.name, tested: info.update, tested_name: info.update_name.as_deref() })
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Update {
    pub version: u16,
}

impl Update {
    /// the version as players know it: the port's name for the one it is
    /// tested with (v1.2 for Ultra Sun's 2048), else the TMD's
    pub fn name(self, game: Game) -> String {
        match (game.tested, game.tested_name) {
            (Some(tested), Some(name)) if tested == self.version => name.to_owned(),
            _ => format!("version {}", zakuro_fs::version_name(self.version)),
        }
    }

    pub fn tested(self, game: Game) -> bool {
        game.tested.is_none_or(|tested| tested == self.version)
    }
}

pub fn game(path: &Path) -> Result<Game, String> {
    game_of(port::port(), path)
}

pub fn game_of(port: &'static Port, path: &Path) -> Result<Game, String> {
    let title = zakuro_fs::Title::load(path).map_err(|error| match error {
        zakuro_fs::FsError::Encrypted(_) | zakuro_fs::FsError::EncryptedCia => {
            format!("This dump is encrypted. {} needs a decrypted one.", port.name)
        }
        zakuro_fs::FsError::UnknownFormat => "This is not a 3DS game (.3ds, .cci, .cxi or .cia).".to_owned(),
        other => format!("This file can't be read as a 3DS game: {other}."),
    })?;
    let id = title.program_id();
    Game::of(port, id).ok_or_else(|| format!("This is title {id:016X}. {} is for {}.", port.name, port.names()))
}

pub fn update(path: &Path, game: Game) -> Result<Update, String> {
    let name = port::get().map(|port| port.name.as_str()).unwrap_or("The launcher");
    let update = zakuro_fs::Update::load(path).map_err(|error| match error {
        zakuro_fs::FsError::Encrypted(_) | zakuro_fs::FsError::EncryptedCia => {
            format!("This update is encrypted. {name} needs a decrypted one.")
        }
        other => format!("This file can't be read as an update: {other}."),
    })?;
    // an update's title id is the game's with 000E where the game has 0000
    if update.title_id != game.program_id | 0x0000_000E_0000_0000 {
        return Err(format!("This update is for title {:016X}, not {}.", update.title_id, game.name));
    }
    Ok(Update { version: update.version })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sun() -> Game {
        Game { program_id: 0x0004_0000_001B_5000, name: "Pokémon Ultra Sun", tested: Some(2048), tested_name: Some("v1.2") }
    }

    #[test]
    fn versions_read_as_players_know_them() {
        assert_eq!(Update { version: 2048 }.name(sun()), "v1.2");
        assert_eq!(Update { version: 1040 }.name(sun()), "version 1.1.0");
        assert!(Update { version: 2048 }.tested(sun()) && !Update { version: 1040 }.tested(sun()));
        let untested = Game { tested: None, tested_name: None, ..sun() };
        assert!(Update { version: 1040 }.tested(untested));
    }
}

/// checks real dumps, which can't live in the repository:
/// LAUNCHER_GAMES=<folder with the dumps> cargo test -- --ignored
#[cfg(test)]
mod dumps {
    use super::*;

    #[test]
    #[ignore]
    fn the_dumps_in_launcher_games() {
        let port: &'static Port = Box::leak(Box::new(
            Port::parse(
                "name = \"Solstice\"\nplayer = \"SolsticeGame\"\n[[game]]\ntitle = \"00040000001B5000\"\nname = \"Pokémon Ultra Sun\"\nupdate = 2048\nupdate_name = \"v1.2\"\n[[game]]\ntitle = \"00040000001B5100\"\nname = \"Pokémon Ultra Moon\"\nupdate = 2048\nupdate_name = \"v1.2\"\n",
            )
            .unwrap(),
        ));
        let dir = std::path::PathBuf::from(std::env::var_os("LAUNCHER_GAMES").expect("LAUNCHER_GAMES"));
        let found = |part: &str| {
            std::fs::read_dir(&dir).unwrap().map(|entry| entry.unwrap().path()).find(|path| path.to_string_lossy().contains(part)).unwrap()
        };
        let sun = game_of(port, &found("Ultra Sun (USA)")).unwrap();
        let moon = game_of(port, &found("Ultra Moon (USA)")).unwrap();
        assert_eq!((sun.program_id, moon.program_id), (0x0004_0000_001B_5000, 0x0004_0000_001B_5100));
        assert_eq!(update(&found("Ultra Sun (World) (v1.2, v2080) (Update) (Patch)-decrypted"), sun).unwrap().version, 2048);
        let encrypted = update(&found("Ultra Sun (World) (v1.2, v2080) (Update).cia"), sun).unwrap_err();
        assert!(encrypted.contains("encrypted"), "{encrypted}");
        let other = update(&found("Ultra Moon (World) (v1.2, v2080) (Update) (Patch)-decrypted"), sun).unwrap_err();
        assert!(other.contains("not Pokémon Ultra Sun"), "{other}");
        let wrong = game_of(port, &found("Pokemon Y (USA)")).unwrap_err();
        assert!(wrong.contains("0004000000055E00"), "{wrong}");
        for message in [encrypted, other, wrong] {
            println!("{message}");
        }
    }
}
