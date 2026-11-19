//! Whether a game executable can load plugins: the plugin exe built by scripts/build.py is linked with
//! `/ENTRY:RoLoaderEntry` (loader/RoLoaderEntry.cpp, which loads RoPluginHost.dll before the CRT starts) and exports
//! `RoLoaderEntry` (scripts/plugin_exe.py adds it to Ragexe.def). It counts as a plugin build when its export table
//! names `RoLoaderEntry` AND the optional header's AddressOfEntryPoint is that export's RVA. rodecomp's plain exe and
//! Gravity's original have no such export.
//!
//! Read-only: only the PE headers, the section table and the export directory are read, never the whole file, and
//! nothing is executed. Every offset is bounds-checked against the file, so a truncated or hostile file is reported
//! as not an exe instead of misread.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

/// The export the loader's entry point is published under.
pub const LOADER_EXPORT: &str = "RoLoaderEntry";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ExeKind {
    /// Starts in the plugin loader: loads RoPluginHost.dll and the plugins.
    PluginBuild,
    /// A valid PE image without the loader (rodecomp's plain exe, Gravity's original).
    NoLoader,
    /// Missing, unreadable or not a PE image; the text says why.
    NotAnExe(String),
}

impl ExeKind {
    pub fn has_loader(&self) -> bool {
        matches!(self, ExeKind::PluginBuild)
    }

    /// Plain words for the CLI and the window.
    pub fn describe(&self) -> String {
        match self {
            ExeKind::PluginBuild => "plugin build (loads plugins)".to_owned(),
            ExeKind::NoLoader => "no plugin loader (plugins are not loaded)".to_owned(),
            ExeKind::NotAnExe(why) => format!("not a game executable: {why}"),
        }
    }
}

pub fn exe_kind(path: &Path) -> ExeKind {
    match File::open(path).map_err(|error| error.to_string()).and_then(|file| Pe::new(file)?.loader_entry()) {
        Ok(true) => ExeKind::PluginBuild,
        Ok(false) => ExeKind::NoLoader,
        Err(why) => ExeKind::NotAnExe(why),
    }
}

struct Section {
    virtual_address: u32,
    virtual_size: u32,
    raw_offset: u32,
    raw_size: u32,
}

struct Pe {
    file: File,
    len: u64,
    entry_rva: u32,
    /// The export data directory (RVA, size); size 0 when there is none.
    exports: (u32, u32),
    sections: Vec<Section>,
}

const IMAGE_FILE_EXECUTABLE_IMAGE: u16 = 0x0002;
const IMAGE_FILE_DLL: u16 = 0x2000;

fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

impl Pe {
    fn read(&mut self, offset: u64, buffer: &mut [u8]) -> Result<(), String> {
        if offset.checked_add(buffer.len() as u64).is_none_or(|end| end > self.len) {
            return Err("truncated PE image".to_owned());
        }
        self.file.seek(SeekFrom::Start(offset)).and_then(|_| self.file.read_exact(buffer)).map_err(|error| error.to_string())
    }

    fn new(file: File) -> Result<Pe, String> {
        let len = file.metadata().map_err(|error| error.to_string())?.len();
        let mut pe = Pe { file, len, entry_rva: 0, exports: (0, 0), sections: Vec::new() };
        let mut dos = [0u8; 64];
        pe.read(0, &mut dos).map_err(|_| "too small for an executable".to_owned())?;
        if &dos[..2] != b"MZ" {
            return Err("no MZ header".to_owned());
        }
        let nt = u64::from(u32_at(&dos, 0x3c));
        // Signature, COFF file header (20 bytes), then the optional header up to its data directories.
        let mut head = [0u8; 4 + 20];
        pe.read(nt, &mut head)?;
        if &head[..4] != b"PE\0\0" {
            return Err("no PE signature".to_owned());
        }
        let sections = usize::from(u16_at(&head, 4 + 2));
        let optional_size = usize::from(u16_at(&head, 4 + 16));
        let characteristics = u16_at(&head, 4 + 18);
        if characteristics & IMAGE_FILE_EXECUTABLE_IMAGE == 0 || characteristics & IMAGE_FILE_DLL != 0 {
            return Err("a PE image, but not an executable".to_owned());
        }
        let mut optional = vec![0u8; optional_size];
        pe.read(nt + 24, &mut optional)?;
        if optional.len() < 2 {
            return Err("no optional header".to_owned());
        }
        // PE32 and PE32+ differ in where the data directories start.
        let directories = match u16_at(&optional, 0) {
            0x10b => 96,
            0x20b => 112,
            magic => return Err(format!("unknown optional header magic {magic:#x}")),
        };
        if optional.len() < directories {
            return Err("truncated optional header".to_owned());
        }
        pe.entry_rva = u32_at(&optional, 16);
        let count = u32_at(&optional, directories - 4) as usize;
        if count >= 1 && optional.len() >= directories + 8 {
            pe.exports = (u32_at(&optional, directories), u32_at(&optional, directories + 4));
        }
        let mut table = vec![0u8; sections * 40];
        pe.read(nt + 24 + optional_size as u64, &mut table)?;
        pe.sections = table
            .chunks_exact(40)
            .map(|s| Section { virtual_size: u32_at(s, 8), virtual_address: u32_at(s, 12), raw_size: u32_at(s, 16), raw_offset: u32_at(s, 20) })
            .collect();
        Ok(pe)
    }

    /// The file offset of `len` bytes at `rva`, when they are all backed by one section's raw data.
    fn offset(&self, rva: u32, len: u32) -> Result<u64, String> {
        let end = rva.checked_add(len).ok_or("export directory out of range")?;
        self.sections
            .iter()
            .find(|s| {
                let size = s.virtual_size.max(s.raw_size);
                rva >= s.virtual_address && u64::from(end) <= u64::from(s.virtual_address) + u64::from(size)
            })
            .and_then(|s| {
                let into = rva - s.virtual_address;
                (u64::from(into) + u64::from(len) <= u64::from(s.raw_size)).then(|| u64::from(s.raw_offset) + u64::from(into))
            })
            .ok_or_else(|| format!("RVA {rva:#x} is not in the file"))
    }

    fn read_rva(&mut self, rva: u32, buffer: &mut [u8]) -> Result<(), String> {
        let offset = self.offset(rva, buffer.len() as u32)?;
        self.read(offset, buffer)
    }

    /// Whether the export table names `RoLoaderEntry` and the entry point is that export's code.
    fn loader_entry(mut self) -> Result<bool, String> {
        let (directory, size) = self.exports;
        if directory == 0 || size == 0 {
            return Ok(false);
        }
        let mut header = [0u8; 40];
        self.read_rva(directory, &mut header)?;
        let functions = u32_at(&header, 20);
        let names = u32_at(&header, 24) as usize;
        let functions_rva = u32_at(&header, 28);
        let names_rva = u32_at(&header, 32);
        let ordinals_rva = u32_at(&header, 36);
        if names == 0 {
            return Ok(false);
        }
        let mut name_rvas = vec![0u8; names * 4];
        self.read_rva(names_rva, &mut name_rvas)?;
        let mut ordinals = vec![0u8; names * 2];
        self.read_rva(ordinals_rva, &mut ordinals)?;
        let wanted = LOADER_EXPORT.as_bytes();
        let mut name = vec![0u8; wanted.len() + 1];
        for index in 0..names {
            // A name shorter than the wanted one may sit at the very end of its section: read what fits.
            if self.read_rva(u32_at(&name_rvas, index * 4), &mut name).is_err() || &name[..wanted.len()] != wanted || name[wanted.len()] != 0 {
                continue;
            }
            let ordinal = u32::from(u16_at(&ordinals, index * 2));
            if ordinal >= functions {
                return Err(format!("{LOADER_EXPORT}: ordinal out of range"));
            }
            let mut target = [0u8; 4];
            self.read_rva(functions_rva + ordinal * 4, &mut target)?;
            let target = u32::from_le_bytes(target);
            // A forwarder points back into the export directory; it is not code of this image.
            let forwarder = target >= directory && target < directory.saturating_add(size);
            return Ok(!forwarder && target != 0 && target == self.entry_rva);
        }
        Ok(false)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::testutil::TempDir;

    pub const PLUGIN_EXE: &str = "Z:/Work/RO/rodecomp-Plugins/build/exe/Ragexe.exe";
    pub const PLAIN_EXE: &str = "Z:/Work/RO/rodecomp-Plugins/rodecomp/build/exe/Ragexe.exe";
    const GRAVITY_EXE: &str = "C:/Users/Spl3en/clank/rodecomp/.td/target.exe";

    /// The real file, or None (and a note) when this machine does not have it.
    pub fn real(path: &str) -> Option<&Path> {
        let path = Path::new(path);
        if path.is_file() {
            Some(path)
        } else {
            eprintln!("skipped: {} is not here", path.display());
            None
        }
    }

    /// A minimal PE32 exe with one section holding an export directory that names `export` at `export_rva`.
    pub fn synthetic(export: &str, export_rva: u32, entry_rva: u32) -> Vec<u8> {
        let mut image = vec![0u8; 0x400];
        image[..2].copy_from_slice(b"MZ");
        image[0x3c..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        let nt = 0x80;
        image[nt..nt + 4].copy_from_slice(b"PE\0\0");
        let coff = nt + 4;
        image[coff..coff + 2].copy_from_slice(&0x14cu16.to_le_bytes());
        image[coff + 2..coff + 4].copy_from_slice(&1u16.to_le_bytes());
        image[coff + 16..coff + 18].copy_from_slice(&(96u16 + 16 * 8).to_le_bytes());
        image[coff + 18..coff + 20].copy_from_slice(&IMAGE_FILE_EXECUTABLE_IMAGE.to_le_bytes());
        let optional = coff + 20;
        image[optional..optional + 2].copy_from_slice(&0x10bu16.to_le_bytes());
        image[optional + 16..optional + 20].copy_from_slice(&entry_rva.to_le_bytes());
        image[optional + 92..optional + 96].copy_from_slice(&16u32.to_le_bytes());
        image[optional + 96..optional + 100].copy_from_slice(&0x1000u32.to_le_bytes());
        image[optional + 100..optional + 104].copy_from_slice(&0x100u32.to_le_bytes());
        let section = optional + 96 + 16 * 8;
        image[section..section + 5].copy_from_slice(b".text");
        image[section + 8..section + 12].copy_from_slice(&0x200u32.to_le_bytes());
        image[section + 12..section + 16].copy_from_slice(&0x1000u32.to_le_bytes());
        image[section + 16..section + 20].copy_from_slice(&0x200u32.to_le_bytes());
        image[section + 20..section + 24].copy_from_slice(&0x200u32.to_le_bytes());
        // Export directory at RVA 0x1000 (file 0x200): 1 function, 1 name.
        let raw = 0x200;
        let field = |image: &mut Vec<u8>, at: usize, value: u32| image[raw + at..raw + at + 4].copy_from_slice(&value.to_le_bytes());
        field(&mut image, 20, 1);
        field(&mut image, 24, 1);
        field(&mut image, 28, 0x1040);
        field(&mut image, 32, 0x1044);
        field(&mut image, 36, 0x1048);
        field(&mut image, 0x40, export_rva);
        field(&mut image, 0x44, 0x1050);
        image[raw + 0x50..raw + 0x50 + export.len()].copy_from_slice(export.as_bytes());
        image
    }

    #[test]
    fn real_files() {
        if let Some(path) = real(PLUGIN_EXE) {
            assert_eq!(exe_kind(path), ExeKind::PluginBuild);
        }
        for path in [PLAIN_EXE, GRAVITY_EXE].into_iter().filter_map(real) {
            assert_eq!(exe_kind(path), ExeKind::NoLoader, "{}", path.display());
        }
    }

    #[test]
    fn synthetic_images() {
        let dir = TempDir::new();
        let check = |bytes: Vec<u8>| {
            let path = dir.path().join("x.exe");
            std::fs::write(&path, bytes).unwrap();
            exe_kind(&path)
        };
        assert_eq!(check(synthetic("RoLoaderEntry", 0x1100, 0x1100)), ExeKind::PluginBuild);
        assert_eq!(check(synthetic("RoLoaderEntry", 0x1100, 0x1180)), ExeKind::NoLoader, "entry point elsewhere");
        assert_eq!(check(synthetic("RoLoaderEntry", 0x1010, 0x1010)), ExeKind::NoLoader, "a forwarder");
        assert_eq!(check(synthetic("RoLoaderEntryX", 0x1100, 0x1100)), ExeKind::NoLoader, "another name");
        assert_eq!(check(synthetic("RoLoader", 0x1100, 0x1100)), ExeKind::NoLoader, "a prefix");
        let mut truncated = synthetic("RoLoaderEntry", 0x1100, 0x1100);
        truncated.truncate(0x210);
        assert!(matches!(check(truncated), ExeKind::NotAnExe(_)));
        assert!(matches!(check(b"not an exe".to_vec()), ExeKind::NotAnExe(_)));
        assert!(matches!(exe_kind(&dir.path().join("missing.exe")), ExeKind::NotAnExe(_)));
    }
}
