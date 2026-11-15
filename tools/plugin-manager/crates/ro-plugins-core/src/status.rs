//! plugins/status.txt (written by Host.cpp WriteStatus), its staleness, and detection of the running game.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

use sysinfo::{ProcessRefreshKind, ProcessesToUpdate, System, UpdateKind};

pub const STATUS_FILE: &str = "status.txt";

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Record {
    pub version: String,
    pub state: String,
    pub reason: String,
    /// `Dep=1.0.0,Other=missing` as written by the host.
    pub resolved: String,
}

impl Record {
    /// The resolved dependency versions as (name, version) pairs.
    pub fn resolved_pairs(&self) -> Vec<(&str, &str)> {
        self.resolved
            .split(',')
            .filter(|item| !item.is_empty())
            .map(|item| item.split_once('=').unwrap_or((item, "")))
            .collect()
    }
}

/// plugins/status.txt as written by the host, or empty when it does not exist.
#[derive(Debug, Clone, Default)]
pub struct Status {
    pub path: PathBuf,
    pub exists: bool,
    /// `host`, `api`, `pid`, `started`, `exe`.
    pub header: BTreeMap<String, String>,
    pub plugins: BTreeMap<String, Record>,
    pub written: Option<SystemTime>,
}

impl Status {
    pub fn read(plugins: &Path) -> Status {
        let path = plugins.join(STATUS_FILE);
        match fs::read(&path) {
            Ok(bytes) => {
                let written = fs::metadata(&path).and_then(|m| m.modified()).ok();
                let mut status = Status::parse(&String::from_utf8_lossy(&bytes));
                status.path = path;
                status.exists = true;
                status.written = written;
                status
            }
            Err(_) => Status { path, ..Status::default() },
        }
    }

    /// Parses the text like plugins.py: a `RoPluginHost\t<version>\t<key> <value>...` header, then one
    /// `name\tversion\tstate\treason\tresolved` line per plugin (missing fields are empty).
    pub fn parse(text: &str) -> Status {
        let mut status = Status::default();
        let mut lines = text.lines();
        let Some(first) = lines.next() else { return status };
        if let Some(fields) = first.strip_prefix("RoPluginHost\t") {
            let mut fields = fields.split('\t');
            status.header.insert("host".into(), fields.next().unwrap_or_default().into());
            for field in fields {
                let (key, value) = field.split_once(' ').unwrap_or((field, ""));
                status.header.insert(key.into(), value.into());
            }
        }
        for line in lines {
            let mut fields = line.split('\t');
            let mut next = || fields.next().unwrap_or_default().to_owned();
            let name = next();
            let record = Record { version: next(), state: next(), reason: next(), resolved: next() };
            status.plugins.insert(name, record);
        }
        status
    }

    pub fn header(&self, key: &str) -> Option<&str> {
        self.header.get(key).map(String::as_str)
    }

    pub fn pid(&self) -> u32 {
        self.header("pid").and_then(|pid| pid.trim().parse().ok()).unwrap_or(0)
    }
}

/// The later of the modification and the creation/change time: copies keep the build's mtime, so the time the file
/// appeared counts too.
pub fn touched(path: &Path) -> Option<SystemTime> {
    let metadata = fs::metadata(path).ok()?;
    let modified = metadata.modified().ok();
    #[cfg(unix)]
    let changed = {
        use std::os::unix::fs::MetadataExt;
        let seconds = metadata.ctime();
        let nanos = metadata.ctime_nsec();
        u64::try_from(seconds).ok().map(|s| SystemTime::UNIX_EPOCH + std::time::Duration::new(s, nanos as u32))
    };
    #[cfg(not(unix))]
    let changed = metadata.created().ok();
    modified.max(changed)
}

/// Absolute path for comparing directories: canonical when it exists, without the `\\?\` prefix, case-folded on
/// Windows and macOS (case-insensitive file systems).
pub fn comparable(path: &Path) -> String {
    let absolute = fs::canonicalize(path).unwrap_or_else(|_| std::path::absolute(path).unwrap_or_else(|_| path.to_owned()));
    let text = absolute.to_string_lossy();
    let text = text.strip_prefix(r"\\?\").unwrap_or(&text);
    if cfg!(any(windows, target_os = "macos")) { text.to_lowercase() } else { text.to_owned() }
}

/// A running process started from the client directory.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GameProcess {
    pub pid: u32,
    pub image: PathBuf,
}

impl GameProcess {
    pub fn name(&self) -> String {
        self.image.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
    }
}

/// The process table, refreshed on demand (only image paths are queried).
pub struct Processes {
    system: System,
}

impl Default for Processes {
    fn default() -> Self {
        Processes::new()
    }
}

impl Processes {
    pub fn new() -> Processes {
        let mut processes = Processes { system: System::new() };
        processes.refresh();
        processes
    }

    pub fn refresh(&mut self) {
        self.system.refresh_processes_specifics(
            ProcessesToUpdate::All,
            true,
            ProcessRefreshKind::nothing().with_exe(UpdateKind::OnlyIfNotSet),
        );
    }

    /// The image of a live process.
    pub fn image(&self, pid: u32) -> Option<&Path> {
        self.system.process(sysinfo::Pid::from_u32(pid)).and_then(|process| process.exe())
    }

    /// Every running process whose image is in `client` and is the exe recorded in status.txt or any `*.exe`.
    pub fn game(&self, client: &Path, status: &Status) -> Vec<GameProcess> {
        let client = comparable(client);
        let recorded = status.header("exe").filter(|exe| !exe.is_empty());
        let mut running: Vec<GameProcess> = self
            .system
            .processes()
            .iter()
            .filter_map(|(pid, process)| {
                let image = process.exe()?;
                let name = image.file_name()?.to_string_lossy();
                let is_exe = recorded.is_some_and(|exe| exe.eq_ignore_ascii_case(&name))
                    || image.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("exe"));
                (is_exe && comparable(image.parent()?) == client)
                    .then(|| GameProcess { pid: pid.as_u32(), image: image.to_owned() })
            })
            .collect();
        running.sort_by_key(|process| process.pid);
        running
    }
}

/// Whether status.txt is live, and why it may not describe the installed files any more.
#[derive(Debug, Clone, Default)]
pub struct Freshness {
    /// The recorded pid is still running as this client's exe.
    pub live: bool,
    /// Files changed since status.txt was written.
    pub changed: Vec<String>,
    /// The notes plugins.py prints, first the liveness, then the changes.
    pub notes: Vec<String>,
}

impl Freshness {
    pub fn is_stale(&self) -> bool {
        !self.live || !self.changed.is_empty()
    }
}

/// `processes` supplies the image of the pid recorded in status.txt.
pub fn freshness(client: &Path, plugins: &Path, status: &Status, processes: &Processes) -> Freshness {
    let pid = status.pid();
    freshness_with_image(client, plugins, status, if pid != 0 { processes.image(pid) } else { None })
}

/// Like [`freshness`], with `image` the live image of the recorded pid (None when it is not running).
pub fn freshness_with_image(client: &Path, plugins: &Path, status: &Status, image: Option<&Path>) -> Freshness {
    if !status.exists {
        return Freshness {
            notes: vec!["no status.txt yet: the game has not run with this host".into()],
            ..Freshness::default()
        };
    }
    let mut changed = Vec::new();
    if let Some(written) = status.written {
        if let Ok(entries) = fs::read_dir(plugins) {
            for entry in entries.filter_map(|entry| entry.ok()) {
                let path = entry.path();
                let name = entry.file_name().to_string_lossy().into_owned();
                let watched = name.eq_ignore_ascii_case(crate::manifest::DISABLED_FILE)
                    || path.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("plugin") || ext.eq_ignore_ascii_case("dll"));
                if watched && touched(&path).is_some_and(|time| time > written) {
                    changed.push(name);
                }
            }
        }
    }
    changed.sort();
    let pid = status.pid();
    let client = comparable(client);
    let recorded = status.header("exe").filter(|exe| !exe.is_empty());
    let live = image.is_some_and(|image| {
        image.parent().is_some_and(|parent| comparable(parent) == client)
            && recorded.is_none_or(|exe| image.file_name().is_some_and(|name| name.to_string_lossy().eq_ignore_ascii_case(exe)))
    });
    let mut notes = Vec::new();
    if live {
        let name = image.and_then(|image| image.file_name()).map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        notes.push(format!("live: written by {name} pid {pid}, still running"));
    } else {
        let pid = if pid != 0 { pid.to_string() } else { "?".into() };
        notes.push(format!(
            "game pid {pid} has exited; states are from its run started {}",
            status.header("started").unwrap_or("?")
        ));
    }
    if !changed.is_empty() {
        notes.push(format!("changed since it was written: {}", changed.join(", ")));
    }
    Freshness { live, changed, notes }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::TempDir;

    const TEXT: &str = "RoPluginHost\t1.1.0\tapi 3\tpid 53196\tstarted 2026-11-15T22:04:32\texe Ragexe.plugins.exe\n\
DX12Effects\t0.1.0\tdisabled\tneeds RendererDX12, which is turned off\tRendererDX12=1.0.0,Other=missing\n\
RendererDX12\t1.0.0\toff\tturned off in plugins\\disabled.txt\t\n\
Orphan\t-\trefused\tno valid Orphan.plugin manifest next to it\t\n\
Short\t1.0.0\n";

    #[test]
    fn parses_header_and_records() {
        let status = Status::parse(TEXT);
        assert_eq!(status.header("host"), Some("1.1.0"));
        assert_eq!(status.header("api"), Some("3"));
        assert_eq!(status.pid(), 53196);
        assert_eq!(status.header("started"), Some("2026-11-15T22:04:32"));
        assert_eq!(status.header("exe"), Some("Ragexe.plugins.exe"));
        let effects = &status.plugins["DX12Effects"];
        assert_eq!(effects.state, "disabled");
        assert_eq!(effects.reason, "needs RendererDX12, which is turned off");
        assert_eq!(effects.resolved_pairs(), [("RendererDX12", "1.0.0"), ("Other", "missing")]);
        assert_eq!(status.plugins["RendererDX12"].resolved, "");
        assert_eq!(status.plugins["Orphan"].state, "refused");
        assert_eq!(status.plugins["Short"], Record { version: "1.0.0".into(), ..Record::default() });
    }

    #[test]
    fn tolerates_garbage() {
        let status = Status::parse("garbage\nA");
        assert!(status.header.is_empty());
        assert_eq!(status.pid(), 0);
        assert_eq!(status.plugins["A"].state, "");
        assert!(Status::parse("").plugins.is_empty());
        let status = Status::parse("RoPluginHost\t1.0.0\tpid notanumber\n");
        assert_eq!(status.pid(), 0);
    }

    #[test]
    fn staleness_without_and_with_status() {
        let dir = TempDir::new();
        let plugins = dir.path().join("plugins");
        fs::create_dir_all(&plugins).unwrap();
        let processes = Processes::new();
        let none = freshness(dir.path(), &plugins, &Status::read(&plugins), &processes);
        assert!(none.is_stale());
        assert_eq!(none.notes, ["no status.txt yet: the game has not run with this host"]);

        dir.write("plugins/status.txt", "RoPluginHost\t1.1.0\tapi 3\tpid 0\tstarted S\texe Ragexe.exe\n");
        std::thread::sleep(std::time::Duration::from_millis(50));
        dir.write("plugins/A.plugin", "name A\nversion 1.0.0\n");
        dir.write("plugins/notes.md", "ignored");
        let status = Status::read(&plugins);
        let stale = freshness(dir.path(), &plugins, &status, &processes);
        assert!(!stale.live);
        assert_eq!(stale.changed, ["A.plugin"]);
        assert_eq!(
            stale.notes,
            ["game pid ? has exited; states are from its run started S", "changed since it was written: A.plugin"]
        );
    }

    #[test]
    fn recorded_pid_must_be_this_clients_exe() {
        let dir = TempDir::new();
        let plugins = dir.path().join("plugins");
        let own = std::process::id();
        dir.write("plugins/status.txt", &format!("RoPluginHost\t1.1.0\tapi 3\tpid {own}\tstarted S\texe Ragexe.exe\n"));
        let processes = Processes::new();
        // This test process is alive but not an exe in `dir`: stale.
        let fresh = freshness(dir.path(), &plugins, &Status::read(&plugins), &processes);
        assert!(!fresh.live);
        assert!(processes.game(dir.path(), &Status::read(&plugins)).is_empty());
        // Pointing the client at the test binary's directory and exe name makes it live and running.
        let image = processes.image(own).unwrap().to_owned();
        let client = image.parent().unwrap();
        let name = image.file_name().unwrap().to_string_lossy();
        let status = Status::parse(&format!("RoPluginHost\t1.1.0\tpid {own}\texe {name}\n"));
        let status = Status { exists: true, written: Some(SystemTime::now()), ..status };
        assert!(freshness(client, &plugins, &status, &processes).live);
        assert!(processes.game(client, &status).iter().any(|process| process.pid == own));
    }
}
