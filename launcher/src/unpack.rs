//! the update's CIA laid out as 3dsrecomp's `--mods` takes it (Luma's
//! layout): `exheader.bin`, `code.bin` decompressed, and its RomFS files
//! under `romfs/`. 3dsrecomp reads no CIA itself; Zakuro used to do this.

use std::fs;
use std::path::Path;

use zakuro_fs::romfs::{DirEntry, RomFs};
use zakuro_fs::Update;

/// what an exheader file holds: the program's description and its access
/// descriptor, as ctrtool writes it.
const EXHEADER_FILE_SIZE: usize = 0x800;
/// an NCCH's header, before its exheader.
const NCCH_HEADER_SIZE: usize = 0x200;

/// writes the update at cia into dir: its program and its modules, which
/// 3dsrecomp needs (the rest the game reads from the dumps themselves).
pub fn unpack(cia: &Path, dir: &Path) -> Result<(), String> {
    fs::create_dir_all(dir).map_err(|error| error.to_string())?;
    let update = Update::load(cia).map_err(|error| format!("{}: {error}", cia.display()))?;
    let title = &update.title;
    let data = title.image().data();
    let start = title.ncch_offset() as usize + NCCH_HEADER_SIZE;
    let exheader = data.get(start..start + EXHEADER_FILE_SIZE).ok_or("the exheader is cut short")?;
    fs::write(dir.join("exheader.bin"), exheader).map_err(|error| error.to_string())?;
    let code = title.code().map_err(|error| error.to_string())?;
    fs::write(dir.join("code.bin"), code).map_err(|error| error.to_string())?;
    let romfs = title.romfs.as_ref().ok_or("the update has no RomFS")?;
    let root = romfs.root().map_err(|error| error.to_string())?;
    write_modules(title, romfs, &root, &dir.join("romfs"))
}

/// the modules (`.cro`, `.crr`, `static.crs`) under dir in the RomFS, at
/// the same paths under out.
fn write_modules(title: &zakuro_fs::Title, romfs: &RomFs, dir: &DirEntry, out: &Path) -> Result<(), String> {
    for (_, file) in romfs.files(dir) {
        let module = file.name.ends_with(".cro") || file.name.ends_with(".crr") || file.name == "static.crs";
        if !module {
            continue;
        }
        let bytes = title.read_romfs(&file, 0, file.data_size as usize).ok_or("a module cut short")?;
        fs::create_dir_all(out).map_err(|error| error.to_string())?;
        fs::write(out.join(&file.name), bytes).map_err(|error| error.to_string())?;
    }
    for (_, sub) in romfs.subdirs(dir) {
        write_modules(title, romfs, &sub, &out.join(&sub.name))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    /// with LAUNCHER_TEST_UPDATE and LAUNCHER_TEST_UNPACKED
    /// (ctrtool's extraction of that update): the same exheader and code.
    #[test]
    #[ignore]
    fn the_same_as_ctrtool() {
        let var = |name| std::path::PathBuf::from(std::env::var_os(name).expect(name));
        let (update, known) = (var("LAUNCHER_TEST_UPDATE"), var("LAUNCHER_TEST_UNPACKED"));
        let out = std::env::temp_dir().join(format!("solstice-unpack-{}", std::process::id()));
        super::unpack(&update, &out).unwrap();
        for name in ["exheader.bin", "code.bin"] {
            assert!(std::fs::read(out.join(name)).unwrap() == std::fs::read(known.join(name)).or_else(|_| std::fs::read(known.join("exefs").join(name))).unwrap(), "{name}");
        }
        assert!(out.join("romfs").read_dir().unwrap().count() > 0);
        std::fs::remove_dir_all(&out).unwrap();
    }
}
