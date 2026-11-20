//! Turning a client's exe into the plugin build and back: `setup` (the exe has no loader), `update` (it is a plugin
//! build) and `restore` (the original exe back).
//!
//! A *plugin set* is a build output: the plugin exe (`exe/Ragexe.exe`), `RoPluginHost.dll` and `plugins/`. The three
//! are one matched set, so setup and update always install all of it.
//!
//! Each operation checks everything first (exe kinds, backup, running game, source), then copies every new file next
//! to its target under a temporary name, and only then renames them into place. A failure before the renames leaves
//! the client untouched; the renames themselves do not fail short of a locked file, which the running check rules
//! out.

use std::fs;
use std::path::{Path, PathBuf};

use crate::exe::{ExeKind, exe_kind};
use crate::manifest::{self, Manifests, is_disabled};
use crate::ops::{Client, EXIT_OK, Outcome, Refused, copy_preserving_mtime};
use crate::resolve;
use crate::settings;
use crate::status::{Processes, Status};

/// Plugins a fresh setup turns on; every other new plugin starts off (in disabled.txt) so the game first behaves like
/// the exe it replaces. These are what a private server client needs to start and connect: ClientInfo (clientinfo.xml:
/// server address, langtype, the login packet rAthena expects), SystemTweaks (starts without the launcher's argument,
/// registry under HKCU so settings save without admin rights) and SoundSettings (starts without an audio device
/// instead of quitting). Their dependencies are turned on with them. LoginFlow and the rest change how the game looks
/// or plays and stay off until the user picks them.
pub const ESSENTIAL: &[&str] = &["ClientInfo", "SystemTweaks", "SoundSettings"];

pub const HOST_DLL: &str = "RoPluginHost.dll";
/// The suffix of a file copied next to its target and not renamed into place yet.
const STAGED: &str = "ro-plugins-new";

/// A build output holding a full plugin set.
#[derive(Debug, Clone)]
pub struct PluginSet {
    pub exe: PathBuf,
    pub host: PathBuf,
    pub plugins: PathBuf,
}

impl PluginSet {
    /// `build` is `<repo>/build` (or its `plugins/` directory): `exe/Ragexe.exe`, `RoPluginHost.dll`, `plugins/`.
    pub fn find(build: &Path) -> Result<PluginSet, Refused> {
        let root = if build.join(HOST_DLL).is_file() || !build.ends_with("plugins") {
            build.to_owned()
        } else {
            build.parent().map(Path::to_owned).unwrap_or_default()
        };
        let set = PluginSet { exe: root.join("exe").join("Ragexe.exe"), host: root.join(HOST_DLL), plugins: root.join("plugins") };
        for (what, path) in [("the plugin exe", &set.exe), ("the plugin host", &set.host)] {
            if !path.is_file() {
                return Err(Refused::new(format!(
                    "{} is not a plugin build output: {what} {} is missing; run scripts/build.py",
                    root.display(),
                    path.display()
                )));
            }
        }
        if !set.plugins.is_dir() {
            return Err(Refused::new(format!("{}: no plugins directory; run scripts/build.py", root.display())));
        }
        if let kind @ (ExeKind::NoLoader | ExeKind::NotAnExe(_)) = exe_kind(&set.exe) {
            return Err(Refused::new(format!("{}: {}, not the plugin exe", set.exe.display(), kind.describe())));
        }
        Ok(set)
    }

    pub fn exe_pdb(&self) -> PathBuf {
        self.exe.with_extension("pdb")
    }
}

/// `MyRO.exe` -> `MyRO.original.exe`: the user's exe as it was before setup.
pub fn backup_of(exe: &Path) -> PathBuf {
    with_suffix(exe, "original", "exe")
}

/// `<stem>.<tag>.<extension>` next to `path`.
fn with_suffix(path: &Path, tag: &str, extension: &str) -> PathBuf {
    let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    path.with_file_name(format!("{stem}.{tag}.{extension}"))
}

fn staged(path: &Path) -> PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_owned();
    name.push(format!(".{STAGED}"));
    path.with_file_name(name)
}

fn io_refused(action: &str, path: &Path, error: std::io::Error) -> Refused {
    Refused::new(format!("{action} {}: {error}", path.display()))
}

/// Byte-for-byte equality (sizes first).
fn same_contents(a: &Path, b: &Path) -> std::io::Result<bool> {
    use std::io::Read;
    if fs::metadata(a)?.len() != fs::metadata(b)?.len() {
        return Ok(false);
    }
    let (mut a, mut b) = (fs::File::open(a)?, fs::File::open(b)?);
    let (mut x, mut y) = (vec![0u8; 1 << 16], vec![0u8; 1 << 16]);
    loop {
        let n = a.read(&mut x)?;
        if n == 0 {
            return Ok(true);
        }
        b.read_exact(&mut y[..n])?;
        if x[..n] != y[..n] {
            return Ok(false);
        }
    }
}

/// Same size and modification time: the copies made here keep the source's time, so this tells a copy of the
/// installed build from a newer build without reading 13 MB on every refresh.
fn same_file(a: &Path, b: &Path) -> bool {
    match (fs::metadata(a), fs::metadata(b)) {
        (Ok(a), Ok(b)) => a.len() == b.len() && a.modified().ok() == b.modified().ok(),
        _ => false,
    }
}

/// Files copied next to their targets, renamed into place by `commit`; whatever is not committed is deleted on drop.
#[derive(Default)]
struct Staging {
    files: Vec<(PathBuf, PathBuf)>,
}

impl Staging {
    fn copy(&mut self, from: &Path, to: &Path) -> Result<(), Refused> {
        let temporary = staged(to);
        self.files.push((temporary.clone(), to.to_owned()));
        copy_preserving_mtime(from, &temporary).map_err(|error| io_refused("copying", from, error))
    }

    fn write(&mut self, to: &Path, text: &str) -> Result<(), Refused> {
        let temporary = staged(to);
        self.files.push((temporary.clone(), to.to_owned()));
        fs::write(&temporary, text).map_err(|error| io_refused("writing", &temporary, error))
    }

    /// Renames the next staged file whose target is `to` into place.
    fn commit_one(&mut self, to: &Path) -> Result<(), Refused> {
        let index = self.files.iter().position(|(_, target)| target == to).expect("staged before commit");
        let (temporary, target) = self.files.remove(index);
        fs::rename(&temporary, &target).map_err(|error| {
            let _ = fs::remove_file(&temporary);
            io_refused("replacing", &target, error)
        })
    }

    fn commit_all(&mut self) -> Result<(), Refused> {
        while let Some((_, target)) = self.files.first().cloned() {
            self.commit_one(&target)?;
        }
        Ok(())
    }
}

impl Drop for Staging {
    fn drop(&mut self) {
        for (temporary, _) in &self.files {
            let _ = fs::remove_file(temporary);
        }
    }
}

/// Every built plugin, staged into `client.plugins` with the user's setting values merged in; returns the names that
/// are new to this client.
fn stage_plugins(client: &Client, set: &PluginSet, staging: &mut Staging, out: &mut Vec<String>) -> Result<Vec<String>, Refused> {
    let plan = client.plan_install(&set.plugins, &[])?;
    out.extend(plan.notes.iter().filter(|note| note.starts_with("build: ")).cloned());
    out.extend(plan.problems.iter().map(|problem| format!("warning: {problem}")));
    let mut new = Vec::new();
    for item in &plan.items {
        let name = &item.manifest.name;
        let dll = set.plugins.join(format!("{name}.dll"));
        if !dll.is_file() {
            return Err(Refused::new(format!("{name}: {} is missing; rebuild with scripts/build.py", dll.display())));
        }
        staging.copy(&dll, &client.plugins.join(format!("{name}.dll")))?;
        if dll.with_extension("pdb").is_file() {
            staging.copy(&dll.with_extension("pdb"), &client.plugins.join(format!("{name}.pdb")))?;
        }
        let new_text = fs::read_to_string(&item.manifest.path).map_err(|error| io_refused("reading", &item.manifest.path, error))?;
        let target = client.plugins.join(format!("{name}.{}", manifest::EXTENSION));
        let old_text = fs::read_to_string(&target).ok();
        let (text, dropped) =
            settings::merge(&new_text, &item.manifest.settings, old_text.as_deref()).map_err(|why| Refused::new(format!("{name}: {why}")))?;
        staging.write(&target, &text)?;
        let change = match &item.previous {
            Some(previous) if *previous == item.manifest.version => format!("{previous}"),
            Some(previous) => format!("{previous} -> {}", item.manifest.version),
            None => {
                new.push(name.clone());
                format!("{} (new)", item.manifest.version)
            }
        };
        out.push(format!("installed {name} {change}"));
        for value in dropped {
            out.push(format!("{name}: dropped setting {} = {} ({})", value.key, value.value, value.why));
        }
    }
    Ok(new)
}

/// The [`ESSENTIAL`] plugins present in `available` and everything they need: what setup turns on, and what the window
/// marks as recommended.
pub fn recommended(available: &Manifests) -> Vec<String> {
    ESSENTIAL.iter().filter(|name| available.contains_key(**name)).flat_map(|name| resolve::requirements(available, name)).collect()
}

/// disabled.txt after installing `new` plugins: the user's entries, plus every new plugin outside [`recommended`].
fn disabled_after(available: &Manifests, existing: &[String], new: &[String]) -> (Vec<String>, Vec<String>) {
    let needed = recommended(available);
    let off: Vec<String> = new.iter().filter(|name| !needed.contains(name) && !is_disabled(existing, name)).cloned().collect();
    let mut all = existing.to_vec();
    all.extend(off.iter().cloned());
    (all, off)
}

/// Who may write `dir` without elevation: probed by creating and deleting a file, the only reliable test (ACLs,
/// virtualisation, read-only media).
pub fn writable(dir: &Path) -> bool {
    let probe = dir.join(format!(".ro-plugins-write-test-{}", std::process::id()));
    match fs::File::create(&probe) {
        Ok(file) => {
            drop(file);
            fs::remove_file(&probe).is_ok()
        }
        Err(_) => false,
    }
}

/// Whether an operation on `exe` needs administrator rights: its folder or its plugins folder is not writable.
pub fn needs_elevation(exe: &Path) -> bool {
    let client = exe.parent().unwrap_or(Path::new("."));
    let plugins = client.join("plugins");
    !writable(client) || (plugins.is_dir() && !writable(&plugins))
}

/// Gives `account` (a SID string `S-1-5-…` or a name) modify rights on `dir` and everything below, so the user can
/// change switches and settings later without elevation.
#[cfg(windows)]
pub fn grant_modify(dir: &Path, account: &str) -> Result<(), String> {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    // icacls takes a SID as `*S-1-…`.
    let account = if account.starts_with("S-1-") { format!("*{account}") } else { account.to_owned() };
    let output = std::process::Command::new("icacls")
        .arg(dir)
        .args(["/grant", &format!("{account}:(OI)(CI)M"), "/T", "/C", "/Q"])
        .creation_flags(CREATE_NO_WINDOW)
        .output()
        .map_err(|error| format!("icacls: {error}"))?;
    if output.status.success() {
        Ok(())
    } else {
        Err(format!("icacls: {}", String::from_utf8_lossy(&output.stdout).trim()))
    }
}

#[cfg(not(windows))]
pub fn grant_modify(_dir: &Path, _account: &str) -> Result<(), String> {
    Ok(())
}

fn client_of(exe: &Path) -> Client {
    Client::new(exe.parent().map(Path::to_owned).unwrap_or_default())
}

/// Installs the host and every plugin of `set` and `grant`s the plugins folder; shared by setup and update.
/// `swap_exe` renames the exe files once everything else is staged.
fn install_set(
    exe: &Path,
    set: &PluginSet,
    grant: Option<&str>,
    out: &mut Vec<String>,
    swap_exe: impl FnOnce(&mut Staging, &mut Vec<String>) -> Result<(), Refused>,
) -> Outcome {
    let client = client_of(exe);
    let fresh_plugins = !client.plugins.is_dir();
    fs::create_dir_all(&client.plugins).map_err(|error| io_refused("creating", &client.plugins, error))?;
    // Before any file is written, so every file below inherits the right.
    let granted = grant.map(|account| grant_modify(&client.plugins, account));

    let mut staging = Staging::default();
    let outcome = (|| {
        staging.copy(&set.exe, exe)?;
        if set.exe_pdb().is_file() {
            staging.copy(&set.exe_pdb(), &exe.with_extension("pdb"))?;
        }
        staging.copy(&set.host, &client.dir.join(HOST_DLL))?;
        let new = stage_plugins(&client, set, &mut staging, out)?;
        let (available, _) = manifest::manifests(&set.plugins);
        let existing = manifest::read_disabled(&client.plugins);
        let (disabled, off) = disabled_after(&available, &existing, &new);

        // Everything is copied; from here on only renames.
        swap_exe(&mut staging, out)?;
        staging.commit_all()?;
        for item in available.keys() {
            let leftover = client.plugins.join(format!("{item}.plugin"));
            if leftover.exists() {
                fs::remove_file(&leftover).map_err(|error| io_refused("removing", &leftover, error))?;
            }
        }
        if disabled.len() != existing.len() || !client.plugins.join(manifest::DISABLED_FILE).exists() {
            manifest::write_disabled(&client.plugins, &disabled)
                .map_err(|error| io_refused("writing", &client.plugins.join(manifest::DISABLED_FILE), error))?;
        }
        if !off.is_empty() {
            out.push(format!("turned off (new, not essential; turn on what you want): {}", off.join(", ")));
        }
        Ok(EXIT_OK)
    })();
    // The staged files go first, so a plugins folder created here is empty again.
    drop(staging);
    if outcome.is_err() && fresh_plugins {
        let _ = fs::remove_dir(&client.plugins);
    }
    if outcome.is_ok() {
        match granted {
            Some(Ok(())) => out.push(format!("{} is writable by your account", client.plugins.display())),
            Some(Err(why)) => out.push(format!("warning: could not grant write access to {}: {why}", client.plugins.display())),
            None => {}
        }
    }
    outcome
}

/// The exe has no plugin loader: it becomes `<stem>.original.exe` and the plugin exe takes its name; the host and
/// every plugin are installed (new ones off except [`ESSENTIAL`]); settings and disabled.txt are kept. `grant` is
/// the account given modify rights on the plugins folder.
pub fn setup(exe: &Path, set: &PluginSet, grant: Option<&str>, processes: &Processes, out: &mut Vec<String>) -> Outcome {
    match exe_kind(exe) {
        ExeKind::NoLoader => {}
        ExeKind::PluginBuild => return Err(Refused::new(format!("refused: {} is already a plugin build; use update", exe.display()))),
        ExeKind::NotAnExe(why) => return Err(Refused::new(format!("refused: {}: not a game executable ({why})", exe.display()))),
    }
    let backup = backup_of(exe);
    // An identical backup is reused; a different one is the user's and is never overwritten.
    let keep_backup = backup.exists();
    if keep_backup && !same_contents(exe, &backup).map_err(|error| io_refused("comparing", &backup, error))? {
        return Err(Refused::new(format!(
            "refused: {} already exists and differs from {}; move it away first (it is not overwritten)",
            backup.display(),
            exe.display()
        )));
    }
    let pdb = exe.with_extension("pdb");
    let pdb_backup = with_suffix(exe, "original", "pdb");
    client_of(exe).refuse_while_running(processes, "set up plugins")?;
    install_set(exe, set, grant, out, |staging, out| {
        if pdb.exists() && !pdb_backup.exists() {
            fs::rename(&pdb, &pdb_backup).map_err(|error| io_refused("renaming", &pdb, error))?;
        }
        if keep_backup {
            out.push(format!("kept the backup {}", backup.display()));
        } else {
            fs::rename(exe, &backup).map_err(|error| io_refused("renaming", exe, error))?;
            out.push(format!("renamed {} to {}", file_name(exe), file_name(&backup)));
        }
        if let Err(refused) = staging.commit_one(exe) {
            if !keep_backup {
                let _ = fs::rename(&backup, exe);
            }
            return Err(refused);
        }
        out.push(format!("copied the plugin exe {} as {}", set.exe.display(), file_name(exe)));
        Ok(())
    })
}

/// The exe is a plugin build: exe, host and every plugin are replaced from `set`; settings, disabled.txt and the
/// backup are kept.
pub fn update(exe: &Path, set: &PluginSet, grant: Option<&str>, processes: &Processes, out: &mut Vec<String>) -> Outcome {
    match exe_kind(exe) {
        ExeKind::PluginBuild => {}
        ExeKind::NoLoader => return Err(Refused::new(format!("refused: {} has no plugin loader; use setup", exe.display()))),
        ExeKind::NotAnExe(why) => return Err(Refused::new(format!("refused: {}: not a game executable ({why})", exe.display()))),
    }
    if crate::status::comparable(exe) == crate::status::comparable(&set.exe) {
        return Err(Refused::new(format!("refused: {} is the build output itself", exe.display())));
    }
    client_of(exe).refuse_while_running(processes, "update plugins")?;
    install_set(exe, set, grant, out, |staging, out| {
        staging.commit_one(exe)?;
        out.push(format!("replaced {} with the plugin exe {}", file_name(exe), set.exe.display()));
        Ok(())
    })
}

/// Puts `<stem>.original.exe` back as the exe. The plugin files stay: without the loader nothing reads them.
pub fn restore(exe: &Path, processes: &Processes, out: &mut Vec<String>) -> Outcome {
    let backup = backup_of(exe);
    if !backup.is_file() {
        return Err(Refused::new(format!("refused: no backup {}; nothing to restore", backup.display())));
    }
    client_of(exe).refuse_while_running(processes, "restore the original exe")?;
    let aside = staged(exe);
    if exe.exists() {
        fs::rename(exe, &aside).map_err(|error| io_refused("renaming", exe, error))?;
    }
    if let Err(error) = fs::rename(&backup, exe) {
        let _ = fs::rename(&aside, exe);
        return Err(io_refused("renaming", &backup, error));
    }
    let _ = fs::remove_file(&aside);
    out.push(format!("restored {} from {}", file_name(exe), file_name(&backup)));
    // Setup moved the user's pdb aside, so a pdb without a backup is the plugin exe's and would not match.
    let pdb_backup = with_suffix(exe, "original", "pdb");
    let pdb = exe.with_extension("pdb");
    let _ = fs::remove_file(&pdb);
    if pdb_backup.is_file() {
        fs::rename(&pdb_backup, &pdb).map_err(|error| io_refused("renaming", &pdb_backup, error))?;
    }
    out.push("the plugin files stay in plugins/; the original exe does not load them".to_owned());
    Ok(EXIT_OK)
}

fn file_name(path: &Path) -> String {
    path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

/// Why a plugin-build client should be updated from `set`, or None: its exe or host differs from the build output's
/// (size or modification time; copies keep the build's time), or the last run refused a plugin as built for a
/// different exe.
pub fn update_reason(exe: &Path, set: &PluginSet, status: &Status) -> Option<String> {
    if let Some((name, _)) = status.plugins.iter().find(|(_, r)| r.built_for_other_exe()) {
        return Some(format!("{name} was built for a different exe"));
    }
    if !same_file(exe, &set.exe) {
        return Some("the build output has a different plugin exe".to_owned());
    }
    let host = exe.parent().unwrap_or(Path::new(".")).join(HOST_DLL);
    if !same_file(&host, &set.host) {
        return Some("the build output has a different plugin host".to_owned());
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::exe::tests::{PLAIN_EXE, PLUGIN_EXE, real, synthetic};
    use crate::testutil::TempDir;

    fn plugin(name: &str, depends: &str) -> String {
        let depends = if depends.is_empty() { String::new() } else { format!("\n[dependencies]\n{depends} = \">=1.0.0 <2.0.0-0\"\n") };
        format!(
            "name = \"{name}\"\ncategory = \"T\"\nversion = \"1.0.0\"\n{depends}\n[settings.On]\ntype = \"bool\"\ndefault = true\n"
        )
    }

    /// A build output with a synthetic plugin exe (or the real one), the host and four plugins; a client with a
    /// plain exe `MyRO.exe`.
    fn fixture(real_exes: bool) -> Option<(TempDir, PathBuf, PluginSet)> {
        let dir = TempDir::new();
        fs::create_dir_all(dir.path().join("build/exe")).unwrap();
        fs::create_dir_all(dir.path().join("client")).unwrap();
        let exe = dir.path().join("client/MyRO.exe");
        if real_exes {
            fs::copy(real(PLUGIN_EXE)?, dir.path().join("build/exe/Ragexe.exe")).unwrap();
            fs::copy(real(PLAIN_EXE)?, &exe).unwrap();
        } else {
            fs::write(dir.path().join("build/exe/Ragexe.exe"), synthetic("RoLoaderEntry", 0x1100, 0x1100)).unwrap();
            fs::write(&exe, synthetic("Other", 0x1100, 0x1100)).unwrap();
        }
        dir.write("build/exe/Ragexe.pdb", "pdb");
        dir.write("build/RoPluginHost.dll", "host");
        for (name, depends) in [("ClientInfo", ""), ("SystemTweaks", ""), ("SoundSettings", "Base"), ("Base", ""), ("Zoom", "")] {
            dir.write(&format!("build/plugins/{name}.toml"), &plugin(name, depends));
            dir.write(&format!("build/plugins/{name}.dll"), name);
        }
        let set = PluginSet::find(&dir.path().join("build")).unwrap();
        Some((dir, exe, set))
    }

    fn read(path: &Path) -> String {
        fs::read_to_string(path).unwrap()
    }

    fn check_cycle(real_exes: bool) {
        let Some((dir, exe, set)) = fixture(real_exes) else { return };
        let processes = Processes::new();
        let original = fs::read(&exe).unwrap();
        dir.write("client/plugins/Zoom.toml", &plugin("Zoom", "").replace("default = true\n", "default = true\nvalue = false\n"));
        dir.write("client/plugins/disabled.txt", "# mine\nGone\n");
        let mut out = Vec::new();

        assert!(update(&exe, &set, None, &processes, &mut out).unwrap_err().message.contains("use setup"));
        assert_eq!(setup(&exe, &set, None, &processes, &mut out), Ok(EXIT_OK), "{out:#?}");
        let backup = dir.path().join("client/MyRO.original.exe");
        assert_eq!(fs::read(&backup).unwrap(), original);
        assert_eq!(exe_kind(&exe), ExeKind::PluginBuild);
        assert_eq!(read(&dir.path().join("client/MyRO.pdb")), "pdb");
        assert_eq!(read(&dir.path().join("client/RoPluginHost.dll")), "host");
        assert_eq!(read(&dir.path().join("client/plugins/Base.dll")), "Base");
        assert!(read(&dir.path().join("client/plugins/Zoom.toml")).contains("value = false"), "settings kept");
        // Zoom was installed before (not new); Base is needed by an essential plugin.
        assert_eq!(manifest::read_disabled(&dir.path().join("client/plugins")), ["Gone"]);
        assert!(read(&dir.path().join("client/plugins/disabled.txt")).starts_with("# mine\n"));
        let leftovers: Vec<_> = fs::read_dir(dir.path().join("client/plugins")).unwrap().flatten().filter(|e| e.file_name().to_string_lossy().ends_with(STAGED)).collect();
        assert!(leftovers.is_empty());

        out.clear();
        assert!(setup(&exe, &set, None, &processes, &mut out).unwrap_err().message.contains("already a plugin build"));
        assert_eq!(update_reason(&exe, &set, &Status::default()), None);
        dir.write("build/RoPluginHost.dll", "host 2");
        dir.write("build/plugins/New.toml", &plugin("New", ""));
        dir.write("build/plugins/New.dll", "New");
        let set = PluginSet::find(&dir.path().join("build/plugins")).unwrap();
        assert!(update_reason(&exe, &set, &Status::default()).is_some());
        assert_eq!(update(&exe, &set, None, &processes, &mut out), Ok(EXIT_OK), "{out:#?}");
        assert_eq!(read(&dir.path().join("client/RoPluginHost.dll")), "host 2");
        assert_eq!(manifest::read_disabled(&dir.path().join("client/plugins")), ["Gone", "New"]);
        assert_eq!(fs::read(&backup).unwrap(), original, "backup kept");
        assert_eq!(update_reason(&exe, &set, &Status::default()), None);

        out.clear();
        assert_eq!(restore(&exe, &processes, &mut out), Ok(EXIT_OK));
        assert_eq!(fs::read(&exe).unwrap(), original);
        assert!(!backup.exists());
        assert!(dir.path().join("client/plugins/New.dll").exists(), "plugins stay");
        assert!(restore(&exe, &processes, &mut out).unwrap_err().message.contains("no backup"));
    }

    #[test]
    fn setup_update_restore_synthetic() {
        check_cycle(false);
    }

    #[test]
    fn setup_update_restore_real_exes() {
        check_cycle(true);
    }

    #[test]
    fn setup_refuses_a_different_backup_and_keeps_an_identical_one() {
        let Some((dir, exe, set)) = fixture(false) else { return };
        let processes = Processes::new();
        let backup = dir.path().join("client/MyRO.original.exe");
        fs::write(&backup, "someone else's").unwrap();
        let mut out = Vec::new();
        let error = setup(&exe, &set, None, &processes, &mut out).unwrap_err();
        assert!(error.message.contains("differs"), "{}", error.message);
        assert_eq!(read(&backup), "someone else's");
        assert!(!dir.path().join("client/plugins").exists(), "nothing changed");
        assert!(!dir.path().join("client/RoPluginHost.dll").exists());

        fs::copy(&exe, &backup).unwrap();
        assert_eq!(setup(&exe, &set, None, &processes, &mut out), Ok(EXIT_OK));
        assert!(out.iter().any(|line| line.starts_with("kept the backup")));
    }

    #[test]
    fn a_failed_setup_leaves_the_client_as_it_was() {
        let Some((dir, exe, set)) = fixture(false) else { return };
        let processes = Processes::new();
        fs::remove_file(dir.path().join("build/plugins/Zoom.dll")).unwrap();
        let before = fs::read(&exe).unwrap();
        let error = setup(&exe, &set, None, &processes, &mut Vec::new()).unwrap_err();
        assert!(error.message.contains("Zoom.dll is missing"), "{}", error.message);
        assert_eq!(fs::read(&exe).unwrap(), before);
        assert!(!dir.path().join("client/MyRO.original.exe").exists());
        let entries: Vec<_> = fs::read_dir(dir.path().join("client")).unwrap().flatten().map(|e| e.file_name()).collect();
        assert_eq!(entries, ["MyRO.exe"]);
    }

    #[test]
    fn plugin_set_needs_the_plugin_exe() {
        let Some((dir, _, _)) = fixture(false) else { return };
        fs::write(dir.path().join("build/exe/Ragexe.exe"), synthetic("Other", 0x1100, 0x1100)).unwrap();
        assert!(PluginSet::find(&dir.path().join("build")).unwrap_err().message.contains("not the plugin exe"));
        assert!(PluginSet::find(&dir.path().join("nowhere")).is_err());
    }

    #[test]
    fn update_reason_reads_failed_loads() {
        let Some((_dir, exe, set)) = fixture(false) else { return };
        let status = Status::parse("RoPluginHost\t1.0.0\nZoom\t1.0.0\tfailed-load\tbuilt for a different Ragexe.exe: missing ?x\n");
        assert_eq!(update_reason(&exe, &set, &status).as_deref(), Some("Zoom was built for a different exe"));
    }
}
