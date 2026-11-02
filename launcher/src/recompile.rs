//! recompiling the game with 3dsrecomp, following what it prints, and the
//! two programs the launcher runs: 3dsrecomp, and the runtime's player.

use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use crate::config;
use crate::port;

#[derive(Debug, Clone, PartialEq)]
pub enum Stage {
    Starting,
    /// Zig, when there is no C compiler: bytes come of how many.
    Downloading { done: u64, total: u64 },
    /// finding the code and writing it as C.
    Generating,
    Compiling { done: usize, total: usize },
    Linking,
    Done,
    Failed(String),
}

impl Stage {
    /// how far along it is, 0 to 1, going by the part that takes longest.
    pub fn fraction(&self) -> f32 {
        match self {
            Stage::Starting => 0.0,
            // a bar of its own
            Stage::Downloading { done, total } => *done as f32 / (*total).max(1) as f32,
            Stage::Generating => 0.02,
            Stage::Compiling { done, total } => 0.05 + 0.85 * *done as f32 / (*total).max(1) as f32,
            Stage::Linking => 0.9,
            Stage::Done | Stage::Failed(_) => 1.0,
        }
    }

    pub fn describe(&self) -> String {
        match self {
            Stage::Starting => "Starting…".to_owned(),
            Stage::Downloading { done, total } if done >= total => "Unpacking the C compiler (Zig)…".to_owned(),
            Stage::Downloading { done, total } => {
                format!("Downloading the C compiler (Zig), {} of {} MB", done >> 20, total >> 20)
            }
            Stage::Generating => "Finding the game's code and writing it as C…".to_owned(),
            Stage::Compiling { done, total } => format!("Compiling, {done} of {total} files"),
            Stage::Linking => "Linking the library…".to_owned(),
            Stage::Done => "Done.".to_owned(),
            Stage::Failed(error) => format!("It failed: {error}"),
        }
    }

    pub fn finished(&self) -> bool {
        matches!(self, Stage::Done | Stage::Failed(_))
    }

    /// where a line 3dsrecomp prints leaves it, none for one that changes
    /// nothing.
    fn after(line: &str) -> Option<Stage> {
        if let Some((done, total)) = line.strip_prefix("compiled ").and_then(|rest| rest.split_once(" of ")) {
            let (done, total) = (done.trim().parse().ok()?, total.trim().parse().ok()?);
            return Some(if done == total { Stage::Linking } else { Stage::Compiling { done, total } });
        }
        if line.starts_with("wrote ") && line.contains(" C files") {
            return Some(Stage::Generating);
        }
        None
    }
}

/// the programs the launcher drives.
#[derive(Debug, Clone)]
pub struct Tools {
    pub recomp: PathBuf,
    pub player: PathBuf,
}

impl Tools {
    /// both found, where the config says or where the package puts them.
    pub fn find(library: &config::Library) -> Option<Tools> {
        let recomp = library.recomp.clone().filter(|path| path.is_file()).or_else(|| places("3dsrecomp").into_iter().find(|p| p.is_file()))?;
        let player = library.player.clone().filter(|path| path.is_file()).or_else(|| game_places().into_iter().find(|p| p.is_file()))?;
        Some(Tools { recomp, player })
    }
}

/// where a program may be: where LAUNCHER_<NAME> says while developing, in
/// a folder of its name beside the launcher as the package lays it out, or
/// beside it.
pub fn places(name: &str) -> Vec<PathBuf> {
    let file = if cfg!(windows) { format!("{name}.exe") } else { name.to_owned() };
    let dir = std::env::current_exe().ok().and_then(|exe| exe.parent().map(Path::to_owned));
    let beside = dir.map(|dir| [dir.join(name).join(&file), dir.join(&file)]).into_iter().flatten();
    let named = std::env::var_os(format!("LAUNCHER_{}", name.to_uppercase())).map(PathBuf::from);
    named.into_iter().chain(beside).collect()
}

/// where the game is: the runtime's player as the package names it (the
/// port's file says, game\SolsticeGame.exe), or as the runtime builds it
/// (player.exe)
pub fn game_places() -> Vec<PathBuf> {
    let name = port::get().map(|port| port.player.clone()).unwrap_or_else(|_| "player".to_owned());
    let mut found = places(&name);
    if let Some(dir) = std::env::current_exe().ok().and_then(|exe| exe.parent().map(Path::to_owned)) {
        let file = if cfg!(windows) { format!("{name}.exe") } else { name.clone() };
        found.insert(found.len().min(1), dir.join("game").join(file));
    }
    found.extend(places("player"));
    found
}

/// the program a job runs, the compiler's download or 3dsrecomp, and
/// whether it was cancelled.
#[derive(Default, Clone)]
pub struct Running {
    pub child: Arc<Mutex<Option<Child>>>,
    pub cancelled: Arc<AtomicBool>,
}

/// a recompile running in 3dsrecomp, after the compiler is found.
pub struct Job {
    running: Running,
    stage: Arc<Mutex<Stage>>,
}

impl Job {
    /// with hot, the game's heat, only its hottest functions are built.
    pub fn start(tools: &Tools, config: &config::Config, hot: Option<&Path>) -> Result<Job, String> {
        let mut command = command(tools, config)?;
        if let Some(hot) = hot {
            command.arg("--hot").arg(hot);
        }
        let recomp = tools.recomp.clone();
        let job = Job { running: Running::default(), stage: Arc::new(Mutex::new(Stage::Starting)) };
        let (running, stage) = (job.running.clone(), job.stage.clone());
        std::thread::spawn(move || {
            // a cancelled job stays so
            let set = |next| {
                let mut stage = stage.lock().unwrap();
                if !stage.finished() {
                    *stage = next;
                }
            };
            let downloaded = |done, total| set(Stage::Downloading { done, total });
            let started = crate::compiler::prepare(&mut command, &downloaded, &running).and_then(|()| {
                set(Stage::Starting);
                follow(&mut command, &recomp, &running, &stage)
            });
            if let Err(error) = started {
                set(Stage::Failed(error));
            }
        });
        Ok(job)
    }

    /// a job that could not start, failed with error.
    pub fn failed(error: String) -> Job {
        Job { running: Running::default(), stage: Arc::new(Mutex::new(Stage::Failed(error))) }
    }

    pub fn stage(&self) -> Stage {
        self.stage.lock().unwrap().clone()
    }

    pub fn cancel(&mut self) {
        self.running.cancelled.store(true, Ordering::Relaxed);
        if let Some(child) = self.running.child.lock().unwrap().as_mut() {
            let _ = child.kill();
        }
        *self.stage.lock().unwrap() = Stage::Failed("cancelled".to_owned());
    }
}

/// starts 3dsrecomp, moving stage along with what it prints.
fn follow(command: &mut Command, recomp: &Path, running: &Running, stage: &Arc<Mutex<Stage>>) -> Result<(), String> {
    hide_console(command);
    let mut child = command
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("could not start {}, {error}", recomp.display()))?;
    // the last error said is kept for a failure
    let error = Arc::new(Mutex::new(String::new()));
    let follow = |stream: Box<dyn std::io::Read + Send>| {
        let (stage, error) = (stage.clone(), error.clone());
        std::thread::spawn(move || {
            for line in BufReader::new(stream).lines().map_while(Result::ok) {
                if line.starts_with("error") || line.contains(" ERROR ") {
                    *error.lock().unwrap() = line.clone();
                }
                if let Some(next) = Stage::after(&line) {
                    let mut stage = stage.lock().unwrap();
                    // a cancelled job stays so
                    if !stage.finished() {
                        *stage = next;
                    }
                }
            }
        })
    };
    let out = follow(Box::new(child.stdout.take().expect("piped")));
    let err = follow(Box::new(child.stderr.take().expect("piped")));
    // cancelled while it started
    if running.cancelled.load(Ordering::Relaxed) {
        let _ = child.kill();
    }
    *running.child.lock().unwrap() = Some(child);
    // the end, once both streams close
    let _ = (out.join(), err.join());
    let mut stage = stage.lock().unwrap();
    if !stage.finished() {
        let error = error.lock().unwrap().clone();
        *stage = if error.is_empty() { Stage::Done } else { Stage::Failed(error) };
    }
    Ok(())
}

/// 3dsrecomp recompiling a game into its build folder, with its update
/// (laid out for it first) and the port's code built for the game's ARM11
/// (the folder the port's file names for it: widescreen and the like), with
/// the decomp's when there is one.
pub fn command(tools: &Tools, config: &config::Config) -> Result<Command, String> {
    let game = config.game.as_deref().ok_or("no game was chosen")?;
    let build = config::build_dir(&config.title).ok_or("there is no APPDATA folder to build in")?;
    let mut command = Command::new(&tools.recomp);
    command.arg("build").arg(game).arg(&build);
    // the update laid out for 3dsrecomp, which reads no CIA
    if let Some(update) = &config.update {
        let dir = config::update_dir(&config.title).ok_or("there is no APPDATA folder to read the update into")?;
        crate::unpack::unpack(update, &dir)?;
        command.arg("--mods").arg(dir);
    }
    // the port's code recompiled along, as a module of its own
    if let Some(decomp) = config.decomp_dir() {
        command.arg("--decomp").arg(decomp);
    }
    Ok(command)
}

/// a console program, whose window should not flash up.
pub fn hide_console(command: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        const CREATE_NO_WINDOW: u32 = 0x0800_0000;
        command.creation_flags(CREATE_NO_WINDOW);
    }
    #[cfg(not(windows))]
    let _ = command;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recomps_lines_move_the_stage_on() {
        assert_eq!(Stage::after("compiled 100 of 891"), Some(Stage::Compiling { done: 100, total: 891 }));
        assert_eq!(Stage::after("compiled 891 of 891"), Some(Stage::Linking));
        assert_eq!(Stage::after("wrote 891 C files, 830 MiB, 0 overrides"), Some(Stage::Generating));
        assert_eq!(Stage::after("built G:/x/00040000001B5000.dll in 648.1s"), None);
    }
}
