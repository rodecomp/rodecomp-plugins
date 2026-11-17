//! `<Name>.toml` plugin files (the build output `build/plugins/<Name>.toml`, installed as `plugins\<Name>.toml` next to
//! the DLL and read by Host.cpp): manifest, setting declarations and the user's values. Also `disabled.txt`.

use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use toml_edit::{DocumentMut, Item};

use crate::semver::{Range, Version};
use crate::settings::{self, Setting, strings};

#[derive(Debug, Clone)]
pub struct Dependency {
    pub name: String,
    /// The range as written in the manifest.
    pub text: String,
    pub range: Range,
}

#[derive(Debug, Clone)]
pub struct Manifest {
    pub path: PathBuf,
    pub name: String,
    pub category: String,
    pub version: Version,
    pub description: String,
    pub depends: Vec<Dependency>,
    /// Plugins that must not run with this one; either side listing the other is enough.
    pub conflicts: Vec<String>,
    /// `[settings.*]` in file order.
    pub settings: Vec<Setting>,
}

/// Valid manifests by name (ordinal order, like Host.cpp's std::map).
pub type Manifests = BTreeMap<String, Manifest>;

pub const EXTENSION: &str = "toml";

fn file_name(path: &Path) -> String {
    path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

/// Host.cpp ParseVersion: like semver but at most 9 digits per core number.
fn host_version_ok(text: &str) -> bool {
    Version::parse(text).is_ok() && text.split('-').next().unwrap_or("").split('.').all(|part| part.len() <= 9)
}

/// Host.cpp ParseRange accepts only the normalised form: `op` + full version tokens, sets joined by `||`, none empty.
fn host_range_ok(text: &str) -> bool {
    let mut set_len = 0;
    for token in text.split_whitespace() {
        if token == "||" {
            if set_len == 0 {
                return false;
            }
            set_len = 0;
            continue;
        }
        let version = [">=", "<=", ">", "<", "="].iter().find_map(|op| token.strip_prefix(op));
        match version {
            Some(version) if host_version_ok(version) => set_len += 1,
            _ => return false,
        }
    }
    set_len > 0
}

const TOP_LEVEL: [&str; 8] = ["name", "category", "version", "description", "conflicts", "libs", "dependencies", "settings"];

impl Manifest {
    /// Reads and validates a plugin file; `<file>: <why>` when the host would refuse it.
    pub fn read(path: &Path) -> Result<Manifest, String> {
        let file = file_name(path);
        let text = fs::read_to_string(path).map_err(|error| format!("{file}: {error}"))?;
        Manifest::parse(path, &text).map_err(|why| format!("{file}: {why}"))
    }

    /// Validates `text` as the plugin file at `path` (its stem must be the name).
    pub fn parse(path: &Path, text: &str) -> Result<Manifest, String> {
        let document: DocumentMut = text.parse().map_err(|error: toml_edit::TomlError| {
            let line = error.span().map(|span| text[..span.start].matches('\n').count() + 1);
            match line {
                Some(line) => format!("line {line}: {}", error.message().trim_end()),
                None => error.message().trim_end().to_owned(),
            }
        })?;
        let root = document.as_table();
        for (key, _) in root.iter() {
            if !TOP_LEVEL.contains(&key) {
                return Err(format!("unknown key {key}"));
            }
        }
        let required = |key: &str| -> Result<String, String> {
            let item = root.get(key).ok_or_else(|| format!("needs {key}"))?;
            match item.as_str() {
                Some(text) if !text.is_empty() => Ok(text.to_owned()),
                _ => Err(format!("{key}: expected a non-empty string")),
            }
        };
        let name = required("name")?;
        let category = required("category")?;
        let version_text = required("version")?;
        let version = Version::parse(&version_text).map_err(|error| format!("version: {error}"))?;
        if !host_version_ok(&version_text) {
            return Err(format!("version: invalid version '{version_text}'"));
        }
        let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
        if !name.eq_ignore_ascii_case(&stem) {
            return Err(format!("names plugin {name}; the file must be {name}.toml"));
        }
        let description = match root.get("description") {
            None => String::new(),
            Some(item) => item.as_str().ok_or("description: expected a string")?.to_owned(),
        };
        let list = |key: &str| root.get(key).map(strings).transpose().map_err(|why| format!("{key}: {why}"));
        let conflicts = list("conflicts")?.unwrap_or_default();
        list("libs")?;
        let mut depends = Vec::new();
        if let Some(item) = root.get("dependencies") {
            let table = item.as_table_like().ok_or("dependencies: expected a table")?;
            for (dependency, range) in table.iter() {
                let text = range.as_str().ok_or_else(|| format!("dependencies.{dependency}: expected a string"))?;
                let parsed = Range::parse(text).map_err(|error| format!("dependencies.{dependency}: {error}"))?;
                if !host_range_ok(text) {
                    return Err(format!("dependencies.{dependency}: invalid range \"{text}\""));
                }
                depends.push(Dependency { name: dependency.to_owned(), text: text.to_owned(), range: parsed });
            }
        }
        let mut settings = Vec::new();
        if let Some(item) = root.get("settings") {
            let table = item.as_table_like().ok_or("settings: expected a table")?;
            for (key, item) in table.iter() {
                let declaration = item.as_table_like().ok_or_else(|| format!("settings.{key}: expected a table"))?;
                settings.push(Setting::parse(key, declaration).map_err(|why| format!("settings.{key}: {why}"))?);
            }
        }
        Ok(Manifest { path: path.to_owned(), name, category, version, description, depends, conflicts, settings })
    }

    pub fn text(&self) -> String {
        fs::read(&self.path).map(|bytes| String::from_utf8_lossy(&bytes).into_owned()).unwrap_or_default()
    }

    pub fn setting(&self, key: &str) -> Option<&Setting> {
        self.settings.iter().find(|setting| setting.key.eq_ignore_ascii_case(key))
    }

    /// Writes (`Some`, validated against the declaration) or removes (`None`) the `value` of setting `key` in the
    /// file, keeping everything else of it. Returns the canonical key.
    pub fn write_value(&self, key: &str, value: Option<&Item>) -> Result<String, String> {
        let setting = self.setting(key).ok_or_else(|| {
            let keys: Vec<&str> = self.settings.iter().map(|s| s.key.as_str()).collect();
            let keys = if keys.is_empty() { "none".to_owned() } else { keys.join(", ") };
            format!("{}: no setting {key} (settings: {keys})", self.name)
        })?;
        let checked = value
            .map(|item| setting.kind.check(item).map_err(|why| format!("{} {}: {why}", self.name, setting.key)))
            .transpose()?;
        let text = fs::read_to_string(&self.path).map_err(|error| format!("{}: {error}", self.path.display()))?;
        let mut document: DocumentMut = text.parse().map_err(|error: toml_edit::TomlError| error.message().to_owned())?;
        settings::edit_value(&mut document, &setting.key, checked.as_ref().map(settings::Value::to_toml))?;
        settings::write_atomically(&self.path, &document.to_string())
            .map_err(|error| format!("writing {}: {error}", self.path.display()))?;
        Ok(setting.key.clone())
    }
}

/// Files in `directory` with the extension `extension` (case-insensitive), sorted by name.
pub fn files_with_extension(directory: &Path, extension: &str) -> Vec<PathBuf> {
    let Ok(entries) = fs::read_dir(directory) else { return Vec::new() };
    let mut files: Vec<PathBuf> = entries
        .filter_map(|entry| entry.ok())
        .filter(|entry| entry.file_type().is_ok_and(|kind| kind.is_file()))
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|ext| ext.eq_ignore_ascii_case(extension)))
        .collect();
    files.sort();
    files
}

/// The stem of every file with `extension`, sorted.
pub fn stems(directory: &Path, extension: &str) -> Vec<String> {
    files_with_extension(directory, extension)
        .iter()
        .filter_map(|path| path.file_stem().map(|stem| stem.to_string_lossy().into_owned()))
        .collect()
}

/// Every valid `*.toml` of `directory` by name, and the error of each invalid one.
pub fn manifests(directory: &Path) -> (Manifests, Vec<String>) {
    let mut found = Manifests::new();
    let mut errors = Vec::new();
    for path in files_with_extension(directory, EXTENSION) {
        match Manifest::read(&path) {
            Ok(manifest) => {
                found.insert(manifest.name.clone(), manifest);
            }
            Err(error) => errors.push(error),
        }
    }
    (found, errors)
}

/// The name in `found` equal to `name` ignoring case.
pub fn canonical<'a>(found: &'a Manifests, name: &str) -> Option<&'a str> {
    found.keys().find(|candidate| candidate.eq_ignore_ascii_case(name)).map(String::as_str)
}

pub const DISABLED_FILE: &str = "disabled.txt";

/// plugins/disabled.txt: one name per line, `#` starts a comment.
pub fn read_disabled(plugins: &Path) -> Vec<String> {
    let Ok(bytes) = fs::read(plugins.join(DISABLED_FILE)) else { return Vec::new() };
    String::from_utf8_lossy(&bytes)
        .lines()
        .filter_map(|line| line.split('#').next().and_then(|text| text.split_whitespace().next()).map(str::to_owned))
        .collect()
}

pub fn is_disabled(disabled: &[String], name: &str) -> bool {
    disabled.iter().any(|entry| entry.eq_ignore_ascii_case(name))
}

/// Rewrites disabled.txt with `names` (sorted case-insensitively, deduplicated), keeping its comment lines, atomically.
pub fn write_disabled(plugins: &Path, names: &[String]) -> io::Result<()> {
    let path = plugins.join(DISABLED_FILE);
    let mut lines: Vec<String> = match fs::read(&path) {
        Ok(bytes) => String::from_utf8_lossy(&bytes)
            .lines()
            .filter(|line| line.trim_start().starts_with('#'))
            .map(str::to_owned)
            .collect(),
        Err(_) => vec!["# Plugins turned off: one name per line. Edited by ro-plugins.".to_owned()],
    };
    let mut sorted: Vec<String> = names.to_vec();
    sorted.sort();
    sorted.dedup();
    sorted.sort_by_key(|name| name.to_lowercase());
    lines.extend(sorted);
    let temporary = path.with_extension("tmp");
    fs::write(&temporary, lines.join("\n") + "\n")?;
    fs::rename(&temporary, &path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::Value;
    use crate::testutil::TempDir;

    #[test]
    fn reads_valid_manifest() {
        let dir = TempDir::new();
        dir.write(
            "A.toml",
            "name = \"A\"\ncategory = \"C\"\nversion = \"1.2.3\"\ndescription = \"d\"\nconflicts = [\"X\"]\nlibs = [\"user32.lib\"]\n\n\
             [dependencies]\nB = \">=1.0.0 <2.0.0-0\"\n\n[settings.Title]\ntype = \"string\"\ndefault = \"t\"\n",
        );
        let manifest = Manifest::read(&dir.path().join("A.toml")).unwrap();
        assert_eq!(manifest.name, "A");
        assert_eq!(manifest.category, "C");
        assert_eq!(manifest.description, "d");
        assert_eq!(manifest.version.to_string(), "1.2.3");
        assert_eq!(manifest.conflicts, ["X"]);
        assert_eq!(manifest.depends[0].name, "B");
        assert_eq!(manifest.depends[0].text, ">=1.0.0 <2.0.0-0");
        assert_eq!(manifest.settings[0].current(), &Value::Str("t".into()));
    }

    #[test]
    fn rejects_bad_manifests() {
        let dir = TempDir::new();
        let head = "name = \"A\"\ncategory = \"C\"\nversion = \"1.0.0\"\n";
        let cases = [
            ("name = \"A\"\n".to_owned(), "A.toml: needs category"),
            ("category = \"C\"\nversion = \"1.0.0\"\n".to_owned(), "A.toml: needs name"),
            ("name = \"B\"\ncategory = \"C\"\nversion = \"1.0.0\"\n".to_owned(), "A.toml: names plugin B; the file must be B.toml"),
            ("name = \"A\"\ncategory = \"C\"\nversion = \"1.0\"\n".to_owned(), "A.toml: version: invalid version '1.0' (expected MAJOR.MINOR.PATCH[-prerelease])"),
            ("name = \"A\"\ncategory = \"C\"\nversion = 1\n".to_owned(), "A.toml: version: expected a non-empty string"),
            (format!("{head}foo = 1\n"), "A.toml: unknown key foo"),
            (format!("{head}description = 1\n"), "A.toml: description: expected a string"),
            (format!("{head}conflicts = \"B\"\n"), "A.toml: conflicts: expected an array of strings, got a string"),
            (format!("{head}[dependencies]\nB = \"^1.0.0\"\n"), "A.toml: dependencies.B: invalid range \"^1.0.0\""),
            (format!("{head}[dependencies]\nB = 1\n"), "A.toml: dependencies.B: expected a string"),
            (format!("{head}[dependencies]\nB = \">*\"\n"), "A.toml: dependencies.B: '>*' matches nothing"),
            (format!("{head}[settings.K]\ntype = \"int\"\n"), "A.toml: settings.K: needs a default"),
            ("name = \"A\"\nname = \"B\"\n".to_owned(), "A.toml: line 2: duplicate key"),
        ];
        for (text, error) in cases {
            dir.write("A.toml", &text);
            assert_eq!(Manifest::read(&dir.path().join("A.toml")).unwrap_err(), error, "{text}");
        }
    }

    #[test]
    fn writes_and_resets_a_value() {
        let dir = TempDir::new();
        let text = "# mine\nname = \"A\"\ncategory = \"C\"\nversion = \"1.0.0\"\n\n[settings.N] # count\ntype = \"int\"\nmax = 9\ndefault = 1\n";
        dir.write("A.toml", text);
        let path = dir.path().join("A.toml");
        let manifest = Manifest::read(&path).unwrap();
        assert_eq!(manifest.write_value("n", Some(&toml_edit::value(4))).unwrap(), "N");
        assert_eq!(fs::read_to_string(&path).unwrap(), format!("{text}value = 4\n"));
        assert_eq!(Manifest::read(&path).unwrap().settings[0].current(), &Value::Int(4));
        assert_eq!(manifest.write_value("N", Some(&toml_edit::value(10))).unwrap_err(), "A N: 10 is above the maximum 9");
        assert_eq!(manifest.write_value("Z", None).unwrap_err(), "A: no setting Z (settings: N)");
        manifest.write_value("N", None).unwrap();
        assert_eq!(fs::read_to_string(&path).unwrap(), text);
    }

    #[test]
    fn disabled_round_trip() {
        let dir = TempDir::new();
        assert!(read_disabled(dir.path()).is_empty());
        dir.write("disabled.txt", "# keep me\nB  # trailing\n\n  a\n#C\n");
        assert_eq!(read_disabled(dir.path()), ["B", "a"]);
        write_disabled(dir.path(), &["b".into(), "A".into(), "b".into()]).unwrap();
        assert_eq!(fs::read_to_string(dir.path().join("disabled.txt")).unwrap(), "# keep me\n#C\nA\nb\n");
    }
}
