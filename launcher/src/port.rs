//! the port this launcher is for, from its port.toml: its name, its games
//! and where its pieces are. One launcher for the ports on the runtime
//! (Solstice, Duality, Tidefall), each packaged with its own port.toml
//! beside the launcher; `--port <file>` names another while developing.
//!
//! ```toml
//! name = "Solstice"          # the window, the folder in %APPDATA%, the shortcut
//! player = "SolsticeGame"    # the runtime's player as the package names it (game\<player>.exe)
//! about = "..."              # what the first page says the port is
//!
//! [[game]]
//! title = "00040000001B5000" # its title id, as the cartridge has it
//! name = "Pokémon Ultra Sun"
//! update = 2048              # the update it is tested with (the TMD's version), if any
//! update_name = "v1.2"       # and as players know it
//! decomp = "decomp"          # the port's code for it (decomp.bin, decomp.txt), a folder beside the launcher
//! ```

use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use serde::Deserialize;

#[derive(Debug, Clone, Deserialize)]
pub struct Port {
    pub name: String,
    pub player: String,
    #[serde(default)]
    pub about: String,
    #[serde(rename = "game", default)]
    pub games: Vec<GameInfo>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct GameInfo {
    pub title: String,
    pub name: String,
    pub update: Option<u16>,
    pub update_name: Option<String>,
    pub decomp: Option<String>,
}

impl GameInfo {
    pub fn program_id(&self) -> Option<u64> {
        u64::from_str_radix(self.title.trim_start_matches("0x"), 16).ok()
    }
}

impl Port {
    pub fn parse(text: &str) -> Result<Port, String> {
        let port: Port = toml::from_str(text).map_err(|error| error.to_string())?;
        if let Some(bad) = port.games.iter().find(|game| game.program_id().is_none()) {
            return Err(format!("the title id {} is not hexadecimal", bad.title));
        }
        Ok(port)
    }

    pub fn game(&self, program_id: u64) -> Option<&GameInfo> {
        self.games.iter().find(|game| game.program_id() == Some(program_id))
    }

    /// the games' names, for saying which ones the port is for
    pub fn names(&self) -> String {
        let names: Vec<String> = self.games.iter().map(|game| format!("{} ({})", game.name, game.title)).collect();
        match names.len() {
            0 => String::new(),
            1 => names[0].clone(),
            n => format!("{} or {}", names[..n - 1].join(", "), names[n - 1]),
        }
    }
}

/// where the port's file is: --port, else port.toml beside the launcher
fn file() -> Option<PathBuf> {
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--port" {
            return args.next().map(PathBuf::from);
        }
    }
    Some(beside("port.toml")?)
}

/// a file beside the launcher
pub fn beside(name: &str) -> Option<PathBuf> {
    Some(std::env::current_exe().ok()?.parent()?.join(name))
}

static PORT: OnceLock<Result<Port, String>> = OnceLock::new();

/// the port, read once; why not, when it can't be
pub fn get() -> Result<&'static Port, &'static str> {
    PORT.get_or_init(|| {
        let file = file().ok_or("the launcher's folder is unknown")?;
        let text = std::fs::read_to_string(&file).map_err(|error| format!("{} could not be read, {error}", file.display()))?;
        Port::parse(&text).map_err(|error| format!("{}: {error}", file.display()))
    })
    .as_ref()
    .map_err(String::as_str)
}

/// the port, when the launcher could not start without it
pub fn port() -> &'static Port {
    get().expect("the port's file was read at the start")
}

/// a folder the port's file names, beside the launcher
pub fn folder(name: &str) -> Option<PathBuf> {
    let dir = file()?.parent().map(Path::to_owned).filter(|dir| !dir.as_os_str().is_empty()).or_else(|| beside(""))?;
    Some(dir.join(name))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_port_reads() {
        let port = Port::parse(
            "name = \"Solstice\"\nplayer = \"SolsticeGame\"\n[[game]]\ntitle = \"00040000001B5000\"\nname = \"Pokémon Ultra Sun\"\nupdate = 2048\nupdate_name = \"v1.2\"\ndecomp = \"decomp\"\n[[game]]\ntitle = \"00040000001B5100\"\nname = \"Pokémon Ultra Moon\"\n",
        )
        .unwrap();
        assert_eq!(port.games.len(), 2);
        assert_eq!(port.game(0x0004_0000_001B_5100).unwrap().name, "Pokémon Ultra Moon");
        assert!(port.game(0x0004_0000_0005_5D00).is_none());
        assert_eq!(port.names(), "Pokémon Ultra Sun (00040000001B5000) or Pokémon Ultra Moon (00040000001B5100)");
        assert!(Port::parse("name = \"x\"\nplayer = \"y\"\n[[game]]\ntitle = \"zz\"\nname = \"n\"\n").is_err());
    }
}
