//! `<Name>.plugin` manifests (written by scripts/build.py, read by Host.cpp), `disabled.txt`, and the
//! `Plugins/<Category>/<Name>/plugin.json` sources that carry the description.

use std::collections::BTreeMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use crate::semver::{Range, Version};

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
    pub depends: Vec<Dependency>,
    /// Plugins that must not run with this one (`conflicts <name>` lines); either side's line is enough.
    pub conflicts: Vec<String>,
}

/// Valid manifests by name (ordinal order, like Host.cpp's std::map), and the error of each invalid one.
pub type Manifests = BTreeMap<String, Manifest>;

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

impl Manifest {
    /// Parses like plugins.py, then also rejects what Host.cpp would refuse (a range that is not normalised), so the
    /// prediction matches the host.
    pub fn read(path: &Path) -> Result<Manifest, String> {
        let file = file_name(path);
        let bytes = fs::read(path).map_err(|error| format!("{file}: {error}"))?;
        let text = String::from_utf8_lossy(&bytes);
        let mut name = None;
        let mut version = None;
        let mut category = None;
        let mut depends = Vec::new();
        let mut conflicts = Vec::new();
        for (number, line) in text.lines().enumerate() {
            let number = number + 1;
            let mut fields = line.trim_start().splitn(2, char::is_whitespace);
            let Some(key) = fields.next().filter(|key| !key.is_empty()) else { continue };
            let remainder = fields.next().unwrap_or("").trim_start();
            let mut fields = remainder.splitn(2, char::is_whitespace);
            let value = fields.next().unwrap_or("");
            let rest = fields.next().unwrap_or("").trim();
            let not_understood = || format!("{file}: line {number}: not understood: {line}");
            match key {
                "name" if !value.is_empty() && rest.is_empty() => name = Some(value.to_owned()),
                "category" if !value.is_empty() && rest.is_empty() => category = Some(value.to_owned()),
                "version" if rest.is_empty() => {
                    version = Some(Version::parse(value).map_err(|error| format!("{file}: line {number}: {error}"))?)
                }
                "depends" if !value.is_empty() => {
                    let range = Range::parse(rest).map_err(|error| format!("{file}: line {number}: {error}"))?;
                    if !host_range_ok(rest) {
                        return Err(format!("{file}: line {number}: invalid range \"{rest}\""));
                    }
                    depends.push(Dependency { name: value.to_owned(), text: rest.to_owned(), range });
                }
                "conflicts" if !value.is_empty() && rest.is_empty() => conflicts.push(value.to_owned()),
                _ => return Err(not_understood()),
            }
        }
        let (Some(name), Some(category), Some(version)) = (name, category, version) else {
            return Err(format!("{file}: needs a name, a category and a version line"));
        };
        if !format!("{name}.plugin").eq_ignore_ascii_case(&file) {
            return Err(format!("{file}: names plugin {name}; the file must be {name}.plugin"));
        }
        Ok(Manifest { path: path.to_owned(), name, category, version, depends, conflicts })
    }

    pub fn text(&self) -> String {
        fs::read(&self.path).map(|bytes| String::from_utf8_lossy(&bytes).into_owned()).unwrap_or_default()
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

pub fn manifests(directory: &Path) -> (Manifests, Vec<String>) {
    let mut found = Manifests::new();
    let mut errors = Vec::new();
    for path in files_with_extension(directory, "plugin") {
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

/// `Plugins/<Category>/<Name>/plugin.json`: the build's source manifest. Only the fields the manager shows are kept.
#[derive(Debug, Clone, Default)]
pub struct PluginJson {
    pub name: String,
    pub category: String,
    pub version: String,
    pub description: String,
    pub dependencies: Vec<(String, String)>,
}

impl PluginJson {
    pub fn read(path: &Path) -> Result<PluginJson, String> {
        let text = fs::read_to_string(path).map_err(|error| format!("{}: {error}", path.display()))?;
        let value: serde_json::Value =
            serde_json::from_str(&text).map_err(|error| format!("{}: {error}", path.display()))?;
        let field = |key: &str| value.get(key).and_then(|v| v.as_str()).unwrap_or_default().to_owned();
        let dependencies = value
            .get("dependencies")
            .and_then(|v| v.as_object())
            .map(|map| map.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or_default().to_owned())).collect())
            .unwrap_or_default();
        Ok(PluginJson {
            name: field("name"),
            category: field("category"),
            version: field("version"),
            description: field("description"),
            dependencies,
        })
    }
}

/// Every `<sources>/<Category>/<Name>/plugin.json` by name; unreadable files are skipped.
pub fn plugin_sources(sources: &Path) -> BTreeMap<String, PluginJson> {
    let Ok(categories) = fs::read_dir(sources) else { return BTreeMap::new() };
    categories
        .filter_map(|entry| entry.ok())
        .filter_map(|category| fs::read_dir(category.path()).ok())
        .flatten()
        .filter_map(|entry| entry.ok())
        .filter_map(|entry| PluginJson::read(&entry.path().join("plugin.json")).ok())
        .filter(|json| !json.name.is_empty() && !json.category.is_empty())
        .map(|json| (json.name.clone(), json))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::TempDir;

    #[test]
    fn reads_valid_manifest() {
        let dir = TempDir::new();
        dir.write("A.plugin", "name A\r\ncategory C\nversion 1.2.3\n\ndepends B >=1.0.0 <2.0.0-0\n");
        let manifest = Manifest::read(&dir.path().join("A.plugin")).unwrap();
        assert_eq!(manifest.name, "A");
        assert_eq!(manifest.category, "C");
        assert_eq!(manifest.version.to_string(), "1.2.3");
        assert_eq!(manifest.depends[0].name, "B");
        assert_eq!(manifest.depends[0].text, ">=1.0.0 <2.0.0-0");
    }

    #[test]
    fn rejects_bad_manifests() {
        let dir = TempDir::new();
        let cases = [
            ("A.plugin", "name A\n", "A.plugin: needs a name, a category and a version line"),
            ("A.plugin", "name A\nversion 1.0.0\n", "A.plugin: needs a name, a category and a version line"),
            ("A.plugin", "name B\ncategory C\nversion 1.0.0\n", "A.plugin: names plugin B; the file must be B.plugin"),
            ("A.plugin", "name A\nversion 1.0\n", "A.plugin: line 2: invalid version '1.0' (expected MAJOR.MINOR.PATCH[-prerelease])"),
            ("A.plugin", "name A\nversion 1.0.0\nfoo bar\n", "A.plugin: line 3: not understood: foo bar"),
            ("A.plugin", "name A\nversion 1.0.0\ndepends B ^1.0.0\n", "A.plugin: line 3: invalid range \"^1.0.0\""),
            ("A.plugin", "name A\nversion 1.0.0\ndepends B\n", "A.plugin: line 3: invalid range \"\""),
            ("A.plugin", "name A\nversion 1.0.0\ndepends B >*\n", "A.plugin: line 3: '>*' matches nothing"),
        ];
        for (file, text, error) in cases {
            dir.write(file, text);
            assert_eq!(Manifest::read(&dir.path().join(file)).unwrap_err(), error);
        }
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

    #[test]
    fn reads_plugin_json() {
        let dir = TempDir::new();
        dir.write("Cat/X/plugin.json", r#"{"name": "X", "category": "Cat", "version": "1.0.0", "description": "d", "dependencies": {"Y": "^1"}}"#);
        dir.write("Cat/NoCategory/plugin.json", r#"{"name": "NoCategory", "version": "1.0.0"}"#);
        dir.write("Cat/Bad/plugin.json", "{");
        let sources = plugin_sources(dir.path());
        assert_eq!(sources.len(), 1);
        assert_eq!(sources["X"].description, "d");
        assert_eq!(sources["X"].category, "Cat");
        assert_eq!(sources["X"].dependencies, [("Y".to_owned(), "^1".to_owned())]);
    }
}
