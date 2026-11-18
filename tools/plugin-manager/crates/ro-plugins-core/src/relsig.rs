//! Change detection for the exe functions a plugin uses (scripts/relsig.py). The build writes `<exe stem>.relsig` next
//! to the exe (`# relsig-id <id>`, then `<linker symbol> <relsig>` per exported function) and records, in each
//! plugin's `[exe]` table, `relsig-id` plus the relsig of every exe function the plugin uses. A recorded function whose
//! relsig differs from the installed exe's is changed, one the exe no longer lists is missing. Values are comparable
//! only under the same id (compiler and flags); otherwise nothing can be checked.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const EXTENSION: &str = "relsig";
pub const ID_KEY: &str = "relsig-id";

/// A plugin's `[exe]` table.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Recorded {
    pub id: String,
    /// Linker symbol -> relsig.
    pub functions: BTreeMap<String, String>,
}

/// The installed exe's `.relsig` file.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ExeRelsig {
    pub id: String,
    pub functions: BTreeMap<String, String>,
}

impl ExeRelsig {
    pub fn parse(text: &str) -> ExeRelsig {
        let mut lines = text.lines();
        let id = lines
            .next()
            .and_then(|line| line.strip_prefix("# relsig-id "))
            .map(|id| id.trim().to_owned())
            .unwrap_or_default();
        let functions = lines
            .filter(|line| !line.is_empty() && !line.starts_with('#'))
            .filter_map(|line| line.split_once(' '))
            .map(|(name, sig)| (name.to_owned(), sig.trim().to_owned()))
            .collect();
        ExeRelsig { id, functions }
    }

    /// `exe` with the extension `.relsig`, when it exists and can be read.
    pub fn read(exe: &Path) -> Option<ExeRelsig> {
        let bytes = fs::read(exe.with_extension(EXTENSION)).ok()?;
        Some(ExeRelsig::parse(&String::from_utf8_lossy(&bytes)))
    }
}

/// The exe of a client folder whose `.relsig` counts: the exe named in status.txt, else Ragexe.exe.
pub fn exe_of(client: &Path, status: &crate::status::Status) -> PathBuf {
    let recorded = status.header("exe").filter(|exe| !exe.is_empty()).map(|exe| client.join(exe));
    recorded.filter(|exe| exe.is_file()).unwrap_or_else(|| client.join("Ragexe.exe"))
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Change {
    Changed,
    Missing,
}

impl Change {
    pub fn as_str(self) -> &'static str {
        match self {
            Change::Changed => "changed",
            Change::Missing => "missing",
        }
    }
}

/// The result of comparing a plugin's recorded functions with the installed exe.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Check {
    /// Why nothing could be compared: no `[exe]` table, no `.relsig` next to the exe, or another compiler/flags.
    Unchecked(&'static str),
    /// Every recorded function is unchanged (or the plugin uses none).
    Unchanged,
    /// The changed and missing functions, by name.
    Changed(Vec<(String, Change)>),
}

impl Check {
    pub fn changes(&self) -> &[(String, Change)] {
        match self {
            Check::Changed(changes) => changes,
            _ => &[],
        }
    }

    /// `?A@@YAXXZ changed, ?B@@YAXXZ missing`.
    pub fn summary(&self) -> String {
        self.changes().iter().map(|(name, change)| format!("{name} {}", change.as_str())).collect::<Vec<_>>().join(", ")
    }
}

pub fn check(recorded: Option<&Recorded>, exe: Option<&ExeRelsig>) -> Check {
    let Some(recorded) = recorded else { return Check::Unchecked("the plugin was built without exe signatures") };
    let Some(exe) = exe else { return Check::Unchecked("the game exe has no .relsig file") };
    if recorded.id.is_empty() || recorded.id != exe.id {
        return Check::Unchecked("the plugin and the game exe were built with another compiler or flags");
    }
    let changes: Vec<(String, Change)> = recorded
        .functions
        .iter()
        .filter_map(|(name, sig)| match exe.functions.get(name) {
            None => Some((name.clone(), Change::Missing)),
            Some(current) if current != sig => Some((name.clone(), Change::Changed)),
            Some(_) => None,
        })
        .collect();
    if changes.is_empty() { Check::Unchanged } else { Check::Changed(changes) }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn recorded(id: &str, functions: &[(&str, &str)]) -> Recorded {
        Recorded { id: id.into(), functions: functions.iter().map(|(n, s)| ((*n).into(), (*s).into())).collect() }
    }

    #[test]
    fn compares_by_name() {
        let exe = ExeRelsig::parse("# relsig-id v1-msvc-abc\n?A@@YAXXZ 1111\n?B@@YAXXZ 2222\n_lua_f 3333\n");
        assert_eq!(exe.id, "v1-msvc-abc");
        assert_eq!(exe.functions.len(), 3);
        let plugin = recorded("v1-msvc-abc", &[("?A@@YAXXZ", "1111"), ("?B@@YAXXZ", "9999"), ("?Gone@@YAXXZ", "4444")]);
        let result = check(Some(&plugin), Some(&exe));
        assert_eq!(
            result,
            Check::Changed(vec![("?B@@YAXXZ".into(), Change::Changed), ("?Gone@@YAXXZ".into(), Change::Missing)])
        );
        assert_eq!(result.summary(), "?B@@YAXXZ changed, ?Gone@@YAXXZ missing");
        assert_eq!(check(Some(&recorded("v1-msvc-abc", &[("_lua_f", "3333")])), Some(&exe)), Check::Unchanged);
        assert_eq!(check(Some(&recorded("v1-msvc-abc", &[])), Some(&exe)), Check::Unchanged);
    }

    #[test]
    fn cannot_check_quietly() {
        let exe = ExeRelsig::parse("# relsig-id v1-msvc-abc\n?A@@YAXXZ 1111\n");
        let other = recorded("v1-msvc-def", &[("?A@@YAXXZ", "0000")]);
        assert!(matches!(check(Some(&other), Some(&exe)), Check::Unchecked(_)));
        assert!(matches!(check(Some(&other), None), Check::Unchecked(_)));
        assert!(matches!(check(None, Some(&exe)), Check::Unchecked(_)));
        assert!(check(None, None).changes().is_empty());
        // A file without the header line has no id and checks nothing.
        assert!(matches!(check(Some(&recorded("", &[])), Some(&ExeRelsig::parse("?A@@YAXXZ 1\n"))), Check::Unchecked(_)));
    }
}
