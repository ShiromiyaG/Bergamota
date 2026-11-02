//! the C compiler 3dsrecomp builds the game with: the one CC names, clang
//! when it can make a DLL on this computer, or else Zig, downloaded once into
//! the port's folder. the official clang needs Visual Studio's libraries to
//! link, which most players don't have; Zig brings its own C library and
//! linker. gcc only when CC names it.

use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::Ordering;
use std::time::Duration;

use sha2::{Digest, Sha256};

use crate::config;
use crate::recompile::{hide_console, Running};

const ZIG: &str = "zig-x86_64-windows-0.17.0";
const ZIG_URL: &str = "https://ziglang.org/download/0.17.0/zig-x86_64-windows-0.17.0.zip";
/// from ziglang.org/download/index.json.
const ZIG_SHA256: &str = "b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428";
const ZIG_SIZE: u64 = 100_268_137;

/// sets command's compiler, downloading Zig first when there is no other,
/// telling downloaded how many bytes of how many have come.
pub fn prepare(command: &mut Command, downloaded: &dyn Fn(u64, u64), running: &Running) -> Result<(), String> {
    if !cfg!(windows) || std::env::var("CC").is_ok_and(|named| !named.trim().is_empty()) {
        return Ok(());
    }
    if clang_links() {
        command.env("CC", "clang");
        return Ok(());
    }
    let dir = zig_dir().ok_or("there is no APPDATA folder to keep the compiler in")?;
    if !dir.join(ZIG).join("zig.exe").is_file() {
        download_zig(&dir, downloaded, running)?;
    }
    // zig's folder first on the PATH, so CC needs no path, which could hold spaces
    let path = std::env::var_os("PATH").unwrap_or_default();
    let path = std::env::join_paths(std::iter::once(dir.join(ZIG)).chain(std::env::split_paths(&path)))
        .map_err(|error| format!("could not add Zig to the PATH, {error}"))?;
    command.env("CC", "zig cc").env("PATH", path);
    Ok(())
}

/// where Zig is kept.
fn zig_dir() -> Option<PathBuf> {
    Some(config::dir()?.join("zig"))
}

/// whether clang is here and links a DLL.
fn clang_links() -> bool {
    let dir = std::env::temp_dir().join(format!("solstice-probe-{}", std::process::id()));
    let source = dir.join("probe.c");
    let links = std::fs::create_dir_all(&dir).is_ok()
        && std::fs::write(&source, "__declspec(dllexport) int probe(void) { return 1; }\n").is_ok()
        && {
            let mut command = Command::new("clang");
            command.arg("-shared").arg("-o").arg(dir.join("probe.dll")).arg(&source);
            hide_console(&mut command);
            command.stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|status| status.success())
        };
    let _ = std::fs::remove_dir_all(&dir);
    links
}

/// Zig downloaded into dir with Windows' own curl and tar, checked against
/// its SHA-256 first.
fn download_zig(dir: &Path, downloaded: &dyn Fn(u64, u64), running: &Running) -> Result<(), String> {
    let system = PathBuf::from(std::env::var_os("SystemRoot").unwrap_or_else(|| "C:\\Windows".into())).join("System32");
    std::fs::create_dir_all(dir).map_err(|error| format!("could not make {}, {error}", dir.display()))?;
    let zip = dir.join(format!("{ZIG}.zip"));
    let _ = std::fs::remove_file(&zip);
    let mut curl = Command::new(system.join("curl.exe"));
    curl.args(["-L", "-f", "-s", "-o"]).arg(&zip).arg(ZIG_URL);
    run(&mut curl, running, || downloaded(std::fs::metadata(&zip).map_or(0, |meta| meta.len()), ZIG_SIZE))
        .map_err(|error| format!("could not download Zig, {error}"))?;
    downloaded(ZIG_SIZE, ZIG_SIZE);

    let sum = sha256(&zip).map_err(|error| format!("could not read {}, {error}", zip.display()))?;
    if sum != ZIG_SHA256 {
        let _ = std::fs::remove_file(&zip);
        return Err("the Zig downloaded is not the one expected (its SHA-256 differs)".to_owned());
    }
    // unpacked beside, then moved in, so that a half unpacked Zig is never taken for one
    let unpacking = dir.join("unpacking");
    let _ = std::fs::remove_dir_all(&unpacking);
    std::fs::create_dir_all(&unpacking).map_err(|error| format!("could not make {}, {error}", unpacking.display()))?;
    let mut tar = Command::new(system.join("tar.exe"));
    tar.arg("-xf").arg(&zip).arg("-C").arg(&unpacking);
    run(&mut tar, running, || {}).map_err(|error| format!("could not unpack Zig, {error}"))?;
    let _ = std::fs::remove_dir_all(dir.join(ZIG));
    std::fs::rename(unpacking.join(ZIG), dir.join(ZIG)).map_err(|error| format!("could not move Zig into place, {error}"))?;
    let _ = std::fs::remove_dir_all(&unpacking);
    let _ = std::fs::remove_file(&zip);
    Ok(())
}

/// runs command as the job's, which cancelling kills, calling on while it
/// runs.
fn run(command: &mut Command, running: &Running, on: impl Fn()) -> Result<(), String> {
    hide_console(command);
    let child = command.stdout(Stdio::null()).stderr(Stdio::null()).spawn().map_err(|error| error.to_string())?;
    *running.child.lock().unwrap() = Some(child);
    loop {
        if running.cancelled.load(Ordering::Relaxed) {
            return Err("cancelled".to_owned());
        }
        let status = running.child.lock().unwrap().as_mut().map(|child| child.try_wait());
        match status {
            Some(Ok(Some(status))) if status.success() => return Ok(()),
            Some(Ok(Some(status))) => return Err(format!("it ended with {status}")),
            Some(Err(error)) => return Err(error.to_string()),
            Some(Ok(None)) => on(),
            None => return Err("cancelled".to_owned()),
        }
        std::thread::sleep(Duration::from_millis(200));
    }
}

fn sha256(path: &Path) -> std::io::Result<String> {
    let mut file = std::fs::File::open(path)?;
    let mut hasher = Sha256::new();
    let mut buffer = vec![0; 1 << 20];
    loop {
        let read = file.read(&mut buffer)?;
        if read == 0 {
            break;
        }
        hasher.update(&buffer[..read]);
    }
    Ok(hasher.finalize().iter().map(|byte| format!("{byte:02x}")).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// downloads Zig (about 100 MB) into a folder of its own and builds a DLL
    /// with it.
    #[test]
    #[ignore]
    fn zig_downloads_and_links() {
        let dir = std::env::temp_dir().join(format!("solstice-zig-{}", std::process::id()));
        download_zig(&dir, &|_, _| {}, &Running::default()).unwrap();
        let source = dir.join("probe.c");
        std::fs::write(&source, "__declspec(dllexport) int probe(void) { return 1; }\n").unwrap();
        let status = Command::new(dir.join(ZIG).join("zig.exe")).args(["cc", "-shared", "-o"]).arg(dir.join("probe.dll")).arg(&source).status();
        let _ = std::fs::remove_dir_all(&dir);
        assert!(status.unwrap().success());
    }
}
